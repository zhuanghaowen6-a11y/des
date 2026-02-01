#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h> // For gettimeofday
#include <pthread.h> // For mutex/cond var for event queue protection
#include <errno.h>
#include <jansson.h> // For JSON parsing
#include <poll.h> // For poll() function
#include <stdint.h> // For uintptr_t

// --- Global DESD State ---
// 事件队列的访问需要互斥锁保护，因为 registration_thread 和主事件循环会并发访问
pthread_mutex_t event_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t event_queue_cond = PTHREAD_COND_INITIALIZER; // 用于pop_event在队列为空时等待事件

// Event Queue (min-heap)
// 方案B：队列容量 vs 总事件数上限 解耦
#define MAX_EVENT_QUEUE_SIZE 100000   // 队列容量：最多同时在队列里的事件数（10w），决定 event_queue 数组大小
#define MAX_TOTAL_EVENTS     3000000  // 总事件数上限：模拟过程中最多生成/处理的事件数（300w）
#define MAX_ACTIVE_EVENTS    MAX_TOTAL_EVENTS  // active/cancel 追踪数组容量，必须 >= MAX_TOTAL_EVENTS

Event event_queue[MAX_EVENT_QUEUE_SIZE + 1];
int event_queue_size = 0;

// Active Events (for quick lookup and cancellation)
// 现在不再保存实际事件的指针，因为事件是值传递的。用于标记事件是否已被取消。
// Event* active_events[MAX_ACTIVE_EVENTS]; // 不再需要，因为不再存储事件引用
int event_active_status[MAX_ACTIVE_EVENTS]; // 0: inactive, 1: active (简化)

// 方案二：存储每个事件的 router_id/thread_id，用于 cancel_event 时减少 pending_event_count
typedef struct {
    int router_id;
    int thread_id;
} EventOwnerInfo;
EventOwnerInfo event_owner_info[MAX_ACTIVE_EVENTS];

// Router States
#define MAX_ROUTERS 250 // Support up to 200 routers
#define MAX_CONNECTIONS_PER_ROUTER 200 // 每个路由器最多支持的连接数

// 连接信息结构
typedef struct {
    int socket_fd;          // 本地 socket fd
    int peer_router_id;     // 对端路由器ID
    int peer_socket_fd;     // 对端 socket fd（用于多连接场景的精确匹配）
    unsigned long connection_id; // 连接ID，用于精确配对
    int is_active;          // 连接是否活跃
    int peer_closed;        // 对端是否已关闭连接（用于单向close处理）
} ConnectionInfo;

// 数据包缓冲区（用于暂存发送的数据，直到虚拟时间到达）
#define MAX_PENDING_PACKETS 2048
#define MAX_PACKET_SIZE 8192

typedef struct {
    char data[MAX_PACKET_SIZE];  // 数据内容（hex编码）
    size_t data_len;             // 数据长度
    int socket_fd;               // 对应的 socket 文件描述符
    int is_used;                 // 是否被使用
} PacketBuffer;

// FIFO队列：记录pending connection的客户端信息
typedef struct {
    int client_router_id;  // 客户端路由器ID
    int client_socket_fd;  // 客户端的socket fd
    unsigned long connection_id; // 连接ID，用于精确配对
} PendingConnection;

// 接收缓冲区大小（足够容纳至少两条最大消息 + 余量）
#define RECV_BUF_SIZE (MAX_MSG_SIZE * 3)

// Per-thread state within a router
typedef struct {
    int thread_id;                      // 线程ID（在router内从0开始）
    int comm_socket_fd;                 // 与该线程通信的socket FD
    RouterStatus status;                // 线程状态 (RUNNING/BLOCKED/IDLE)
    char blocked_on_request_id[64];     // 线程正在等待的请求ID
    char blocked_on_function[64];       // 线程正在等待的函数类型
    unsigned long pending_timeout_event_id; // 记录正在等待的超时事件ID
    int monitored_fds[64];              // 监听的fd数组
    int monitored_fds_events[64];       // 对应的事件类型
    int monitored_fds_count;            // 监听的fd数量
    int is_active;                      // 线程是否活跃
    // 按行拆包缓冲区：解决 AF_UNIX 流式 socket 的粘包/半包问题
    char recv_buf[RECV_BUF_SIZE];       // 接收缓冲区
    size_t recv_len;                    // 缓冲区中有效字节数
    // DETACHED_WAITING 状态：线程已脱离 DES 阻塞控制，等待其重新 hook 回来
    int detached_waiting;               // 1 表示正在等待该线程重新 hook
    double detached_since_vt;           // 进入 DETACHED_WAITING 时的虚拟时间
    // 方案二：用于跟踪当前在事件队列中属于该线程的事件数量
    int pending_event_count;            // 队列中屚于该线程的活跃事件数
} ThreadInfo;

typedef struct {
    int router_id;
    int thread_count;                   // 当前活跃的线程数量
    ThreadInfo threads[MAX_THREADS_PER_ROUTER]; // Per-thread 状态
    // 以下是 router 级别的共享状态
    int pending_packets_count;          // 记录有多少个数据包已经在虚拟时间上到达但未被读取
    int pending_connections_count;      // 记录有多少个连接已经在虚拟时间上建立但未被accept
    PendingConnection pending_connections[MAX_PENDING_PACKETS]; // FIFO队列
    int pending_conn_head;
    int pending_conn_tail;
    int listen_count;
    char listen_addresses[10][256];
    int listening_socket_fds[10];
    ConnectionInfo connections[MAX_CONNECTIONS_PER_ROUTER];
    PacketBuffer packet_buffers[MAX_PENDING_PACKETS];
    int pending_buffer_indices[MAX_PENDING_PACKETS];
    int pending_buffer_head;
    int pending_buffer_tail;
} RouterInfo;

RouterInfo router_states[MAX_ROUTERS + 1]; // router_id 从 1 开始

// Global control socket for accepting initial libdeshook.so connections
static int desd_listen_fd = -1;

// Registration 线程相关
static pthread_t registration_thread;
static volatile int registration_thread_running = 0;

// 保护 router_states 的 mutex（用于 registration thread 与 event loop 之间的同步）
static pthread_mutex_t router_states_mutex = PTHREAD_MUTEX_INITIALIZER;

// DETACHED_WAITING 全局状态：跟踪有多少线程处于等待重新 hook 的状态
static int num_detached_waiting = 0;           // 当前处于 DETACHED_WAITING 的线程数
static double first_detached_vt = 0.0;         // 第一个线程进入 DETACHED_WAITING 时的 VT
#define DETACHED_WAIT_TIMEOUT_SEC 10           // 等待 DETACHED 线程重新 hook 的真实时间超时（秒）

// 心跳日志计数器（全局，便于在退出时统计）
static unsigned long heartbeat_event_counter = 0;

// === Mutex Hook 相关全局状态 ===
// Mutex 等待者队列结构（每个 mutex 最多 MAX_MUTEX_WAITERS 个等待线程）
#define MAX_TRACKED_MUTEXES 2048
#define MAX_MUTEX_WAITERS 32

typedef struct {
    int router_id;
    int thread_id;
} MutexWaiter;

typedef struct {
    uintptr_t mutex_addr;           // mutex 地址（作为唯一标识）
    int owner_router_id;            // 当前持有者的 router_id（0 表示无持有者）
    int owner_thread_id;            // 当前持有者的 thread_id
    MutexWaiter waiters[MAX_MUTEX_WAITERS];  // 等待队列（FIFO）
    int waiter_count;               // 等待者数量
    int is_active;                  // 该 slot 是否被使用
} MutexInfo;

static MutexInfo mutex_table[MAX_TRACKED_MUTEXES];
static int mutex_table_count = 0;  // 当前跟踪的 mutex 数量

// Forward declarations for functions used in registration_thread_func
void push_event(Event new_event);
unsigned long generate_event_id();
extern double current_virtual_time;

// 全局connection_id生成器
static unsigned long next_connection_id = 1;

static unsigned long generate_connection_id() {
    return next_connection_id++;
}

// === 最近关闭连接表：用于跟踪"client 先关闭"的情况 ===
// 当 client 在 server 完成 CONNECTION_INFO 前就 close 时，server 侧需要知道这件事
#define MAX_RECENT_CLOSED_CONNS 512

typedef struct {
    unsigned long conn_id;          // 连接ID
    int client_router_id;           // client 路由器ID
    int server_router_id;           // server 路由器ID（对端）
    int closed_side;                // 1=client 先关闭, 2=server 先关闭
    double vt_when_closed;          // 关闭时的虚拟时间
    int valid;                      // 该 slot 是否有效
} RecentClosedConn;

static RecentClosedConn recent_closed_conns[MAX_RECENT_CLOSED_CONNS];
static int recent_closed_conns_next_idx = 0;  // 环形写入位置

// 记录一条连接被关闭（用于后续检测 client 先关闭的情况）
static void record_closed_connection(unsigned long conn_id, int client_router_id, int server_router_id, int closed_side) {
    int idx = recent_closed_conns_next_idx;
    recent_closed_conns[idx].conn_id = conn_id;
    recent_closed_conns[idx].client_router_id = client_router_id;
    recent_closed_conns[idx].server_router_id = server_router_id;
    recent_closed_conns[idx].closed_side = closed_side;
    recent_closed_conns[idx].vt_when_closed = current_virtual_time;
    recent_closed_conns[idx].valid = 1;
    recent_closed_conns_next_idx = (idx + 1) % MAX_RECENT_CLOSED_CONNS;
    
    printf("[DESD-RECENT-CLOSED] Recorded conn_id=%lu closed by %s (R%d<->R%d) at VT=%.6f\n",
           conn_id, closed_side == 1 ? "client" : "server",
           client_router_id, server_router_id, current_virtual_time);
}

// 检查某个 conn_id 是否被 client 先关闭了
// 返回: 1=client 已关闭, 0=未找到或不是 client 关闭
static int check_client_already_closed(unsigned long conn_id, int expected_client_router_id) {
    for (int i = 0; i < MAX_RECENT_CLOSED_CONNS; i++) {
        if (recent_closed_conns[i].valid &&
            recent_closed_conns[i].conn_id == conn_id &&
            recent_closed_conns[i].client_router_id == expected_client_router_id &&
            recent_closed_conns[i].closed_side == 1) {
            return 1;
        }
    }
    return 0;
}

// --- Per-Thread Helper Functions ---

// 查找或创建指定 router 的线程状态
static ThreadInfo* get_thread_info(int router_id, int thread_id) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) return NULL;
    if (thread_id < 0 || thread_id >= MAX_THREADS_PER_ROUTER) return NULL;
    return &router_states[router_id].threads[thread_id];
}

// 根据 comm_socket_fd 查找线程信息
static ThreadInfo* find_thread_by_socket(int router_id, int comm_fd) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) return NULL;
    for (int i = 0; i < MAX_THREADS_PER_ROUTER; i++) {
        if (router_states[router_id].threads[i].is_active &&
            router_states[router_id].threads[i].comm_socket_fd == comm_fd) {
            return &router_states[router_id].threads[i];
        }
    }
    return NULL;
}

// 注册新线程
static ThreadInfo* register_thread(int router_id, int thread_id, int comm_fd) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) return NULL;
    if (thread_id < 0 || thread_id >= MAX_THREADS_PER_ROUTER) return NULL;
    
    ThreadInfo *ti = &router_states[router_id].threads[thread_id];
    ti->thread_id = thread_id;
    ti->comm_socket_fd = comm_fd;
    ti->status = IDLE;
    ti->is_active = 1;
    ti->monitored_fds_count = 0;
    ti->pending_timeout_event_id = 0;
    memset(ti->blocked_on_request_id, 0, sizeof(ti->blocked_on_request_id));
    memset(ti->blocked_on_function, 0, sizeof(ti->blocked_on_function));
    // 初始化接收缓冲区
    ti->recv_len = 0;
    memset(ti->recv_buf, 0, sizeof(ti->recv_buf));
    // 初始化 DETACHED_WAITING 状态
    ti->detached_waiting = 0;
    ti->detached_since_vt = 0.0;
    // 方案二：初始化 pending_event_count
    ti->pending_event_count = 0;
    
    // 更新 router 的线程计数
    int count = 0;
    for (int i = 0; i < MAX_THREADS_PER_ROUTER; i++) {
        if (router_states[router_id].threads[i].is_active) count++;
    }
    router_states[router_id].thread_count = count;
    
    return ti;
}

// 获取第一个活跃线程（兼容旧代码）
static ThreadInfo* get_first_active_thread(int router_id) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) return NULL;
    for (int i = 0; i < MAX_THREADS_PER_ROUTER; i++) {
        if (router_states[router_id].threads[i].is_active) {
            return &router_states[router_id].threads[i];
        }
    }
    return NULL;
}

// 查找指定阻塞类型的线程
static ThreadInfo* find_blocked_thread(int router_id, const char *blocked_func) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) return NULL;
    for (int i = 0; i < MAX_THREADS_PER_ROUTER; i++) {
        ThreadInfo *ti = &router_states[router_id].threads[i];
        if (ti->is_active && ti->status == BLOCKED &&
            strcmp(ti->blocked_on_function, blocked_func) == 0) {
            return ti;
        }
    }
    return NULL;
}

// 查找任意阻塞线程
static ThreadInfo* find_any_blocked_thread(int router_id) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) return NULL;
    for (int i = 0; i < MAX_THREADS_PER_ROUTER; i++) {
        ThreadInfo *ti = &router_states[router_id].threads[i];
        if (ti->is_active && ti->status == BLOCKED) {
            return ti;
        }
    }
    return NULL;
}

// ============================================================================
// read_one_json_message: 按行拆包的消息读取 helper
// ============================================================================
// 从 ThreadInfo 的接收缓冲区中读取一条完整的 JSON 消息（以 '\n' 为分隔符）。
// 
// 参数:
//   ti     - 线程信息（包含 recv_buf 和 recv_len）
//   flags  - recv 的 flags（如 MSG_DONTWAIT 用于非阻塞）
//   msg    - 输出参数，解析后的消息
//
// 返回值:
//   1  - 成功读取并解析了一条完整消息
//   0  - 没有完整消息可读（非阻塞模式下没有数据，或只有半包）
//   -1 - 错误或连接断开
//
// 注意：调用前应持有 router_states_mutex（如果需要线程安全）
// ============================================================================
static int read_one_json_message(ThreadInfo *ti, int flags, Message *msg) {
    if (!ti || ti->comm_socket_fd < 0) {
        return -1;
    }
    
    // 清空输出消息，防止解析失败时残留旧数据
    memset(msg, 0, sizeof(Message));
    msg->event_type = -1;  // 标记为未知类型
    
    while (1) {
        // Step 1: 在缓冲区中查找 '\n'
        char *newline_pos = memchr(ti->recv_buf, '\n', ti->recv_len);
        
        if (newline_pos != NULL) {
            // 找到完整的一行
            size_t line_len = newline_pos - ti->recv_buf;
            
            // 提取这一行（不含 '\n'）到临时缓冲区
            char line_buf[MAX_MSG_SIZE];
            if (line_len >= MAX_MSG_SIZE) {
                // 行太长，协议错误
                fprintf(stderr, "[DESD ERROR] read_one_json_message: line too long (%zu bytes)\n", line_len);
                // 丢弃这一行，继续处理
                size_t remaining = ti->recv_len - line_len - 1;
                memmove(ti->recv_buf, newline_pos + 1, remaining);
                ti->recv_len = remaining;
                continue;
            }
            
            memcpy(line_buf, ti->recv_buf, line_len);
            line_buf[line_len] = '\0';
            
            // 移除已处理的数据（包括 '\n'）
            size_t remaining = ti->recv_len - line_len - 1;
            memmove(ti->recv_buf, newline_pos + 1, remaining);
            ti->recv_len = remaining;
            
            // 解析 JSON
            if (line_len > 0) {
                json_to_message(line_buf, msg);
                
                // 检查解析是否成功（event_type 不应该是 -1）
                if (msg->event_type == -1 && msg->message_type != DESD_TO_HOOK) {
                    // 打印出错的 JSON 行和线程上下文，便于调试
                    size_t snippet_len = line_len;
                    if (snippet_len > 200) {
                        snippet_len = 200; // 避免日志过长
                    }
                    fprintf(stderr,
                            "[DESD WARNING] read_one_json_message: failed to parse JSON line (thread_id=%d, comm_fd=%d, len=%zu, snippet='%.*s')\n",
                            ti->thread_id,
                            ti->comm_socket_fd,
                            line_len,
                            (int)snippet_len,
                            line_buf);
                    // 继续尝试下一行
                    continue;
                }
                
                return 1;  // 成功读取一条消息
            }
            // 空行，继续查找下一行
            continue;
        }
        
        // Step 2: 缓冲区中没有完整行，尝试从 socket 读取更多数据
        
        // 检查缓冲区是否还有空间
        if (ti->recv_len >= RECV_BUF_SIZE - 1) {
            // 缓冲区满了但没有 '\n'，协议错误
            fprintf(stderr, "[DESD ERROR] read_one_json_message: buffer full but no newline (protocol error)\n");
            // 清空缓冲区，放弃当前数据
            ti->recv_len = 0;
            return -1;
        }
        
        // 从 socket 读取数据
        ssize_t bytes_received = recv(ti->comm_socket_fd, 
                                       ti->recv_buf + ti->recv_len,
                                       RECV_BUF_SIZE - 1 - ti->recv_len,
                                       flags);
        
        if (bytes_received > 0) {
            ti->recv_len += bytes_received;
            // 继续循环，查找 '\n'
            continue;
        } else if (bytes_received == 0) {
            // 连接关闭
            fprintf(stderr,
                    "[DESD ERROR] read_one_json_message: recv returned 0 (peer closed) for thread_id=%d, fd=%d\n",
                    ti->thread_id,
                    ti->comm_socket_fd);
            return -1;
        } else {
            // recv 返回 -1
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞模式下没有更多数据
                return 0;  // 没有完整消息可读
            } else if (errno == EINTR) {
                // 被信号中断，重试
                continue;
            } else {
                // 真正的错误
                fprintf(stderr,
                        "[DESD ERROR] read_one_json_message: recv failed (errno=%d) for thread_id=%d, fd=%d\n",
                        errno,
                        ti->thread_id,
                        ti->comm_socket_fd);
                return -1;
            }
        }
    }
}

// 直接向指定 fd 发送 SUCCESS 响应（用于 registration thread，不查找 router_id）
static void send_success_response_to_fd(int fd, const char* request_id, const char* message) {
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "status", json_string("SUCCESS"));
    if (message) json_object_set_new(payload_obj, "message", json_string(message));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    json_decref(payload_obj);

    Message response;
    memset(&response, 0, sizeof(response));
    response.message_type = DESD_TO_HOOK;
    response.virtual_time = 0.0;  // 注册时虚拟时间为 0
    strncpy(response.request_id, request_id, 63);
    response.request_id[63] = '\0';
    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);

    char *json_str = message_to_json(&response);
    if (json_str) {
        size_t len = strlen(json_str);
        char *with_newline = malloc(len + 2);
        if (with_newline) {
            strcpy(with_newline, json_str);
            with_newline[len] = '\n';
            with_newline[len + 1] = '\0';
            send(fd, with_newline, strlen(with_newline), 0);
            free(with_newline);
        }
        free(json_str);
    }
}

// Registration 线程入口函数：运行期持续接受新线程的注册连接
static void* registration_thread_func(void* arg) {
    (void)arg;
    
    printf("[DESD-REG] Registration thread started, listening for new thread connections.\n");
    
    while (registration_thread_running) {
        int conn_fd = accept(desd_listen_fd, NULL, NULL);
        
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            if (!registration_thread_running) break;
            perror("[DESD-REG] accept error");
            continue;
        }
        
        printf("[DESD-REG] Accepted new thread connection (fd: %d).\n", conn_fd);
        
        char buffer[MAX_MSG_SIZE];
        ssize_t bytes_received = recv(conn_fd, buffer, MAX_MSG_SIZE - 1, 0);
        if (bytes_received <= 0) {
            fprintf(stderr, "[DESD-REG] Failed to receive registration message from fd %d.\n", conn_fd);
            close(conn_fd);
            continue;
        }
        buffer[bytes_received] = '\0';
        
        Message register_msg;
        json_to_message(buffer, &register_msg);
        
        if (register_msg.message_type != HOOK_TO_DESD || 
            register_msg.event_type != ROUTER_START) {
            fprintf(stderr, "[DESD-REG] Expected ROUTER_START, got type=%d event=%d from fd %d\n",
                    register_msg.message_type, register_msg.event_type, conn_fd);
            close(conn_fd);
            continue;
        }
        
        int router_id = register_msg.router_id;
        int thread_id = register_msg.thread_id;
        
        if (router_id <= 0 || router_id > MAX_ROUTERS ||
            thread_id < 0 || thread_id >= MAX_THREADS_PER_ROUTER) {
            fprintf(stderr, "[DESD-REG] Invalid router_id=%d or thread_id=%d from fd %d\n", 
                    router_id, thread_id, conn_fd);
            close(conn_fd);
            continue;
        }
        
        // 加锁保护 router_states 的并发访问
        pthread_mutex_lock(&router_states_mutex);
        ThreadInfo *ti = register_thread(router_id, thread_id, conn_fd);
        pthread_mutex_unlock(&router_states_mutex);
        
        if (ti) {
            printf("[DESD-REG] R%d T%d registered (fd=%d, thread_count=%d).\n",
                   router_id, thread_id, conn_fd, router_states[router_id].thread_count);
            
            // 方案 A：不立即发 SUCCESS，push ROUTER_START 事件到队列
            // 事件循环处理 ROUTER_START 时才发送 SUCCESS，线程在此期间阻塞
            Event start_event;
            memset(&start_event, 0, sizeof(Event));
            start_event.timestamp = current_virtual_time;
            start_event.router_id = router_id;
            start_event.thread_id = thread_id;
            start_event.event_type = ROUTER_START;
            start_event.event_id = generate_event_id();
            // 将 request_id 写入 payload，供 handle_router_start 使用
            snprintf(start_event.payload.json_str, MAX_MSG_SIZE,
                     "{\"request_id\":\"%s\"}", register_msg.request_id);
            
            push_event(start_event);
            printf("[DESD-REG] R%d T%d ROUTER_START event queued (EventID: %lu), thread will block until event is processed.\n",
                   router_id, thread_id, start_event.event_id);
        } else {
            fprintf(stderr, "[DESD-REG] Failed to register R%d T%d\n", router_id, thread_id);
            close(conn_fd);
        }
    }
    
    printf("[DESD-REG] Registration thread exiting.\n");
    return NULL;
}

// --- Function Prototypes ---
void init_desd();
void cleanup_desd();

// Event Queue management
void push_event(Event new_event);
Event pop_event();
void cancel_event(unsigned long event_id);
int is_event_queue_empty();

// Router communication - 现在支持 per-thread 通信
void send_message_to_thread(int router_id, int thread_id, const Message* msg);
int receive_blocking_from_thread(int router_id, int thread_id, Message* msg_out);
// 兼容函数：使用消息中的 thread_id
void send_message_to_router(int router_id, const Message* msg);

// Helper to convert EventType to string for logging
const char* event_type_to_string(EventType type);

// DESD event loop
void desd_event_loop();
void handle_event(Event event);

// 新架构：drain 阶段和单路由器交互函数
void drain_all_messages_nonblocking();
void interact_with_router_until_it_blocks(int active_router_id, int active_thread_id);

// Event Handlers
void handle_router_start(Event event);
void handle_listen_event(Event event);
void handle_connect_request_event(Event event);
void handle_connection_established_event(Event event);
void handle_connection_info_event(Event event);
void handle_close_socket_event(Event event);
void handle_router_block_request(Event event);
void handle_packet_send_event(Event event);
void handle_packet_receive_event(Event event);
void handle_timeout_event(Event event);
void handle_get_virtual_time_event(Event event);
void handle_cancel_block_request(Event event);
void handle_mutex_retry_event(Event event);

// Mutex Hook 相关函数
static MutexInfo* find_or_create_mutex(uintptr_t mutex_addr);
static void handle_mutex_lock_acquired(int router_id, int thread_id, uintptr_t mutex_addr);
static void handle_mutex_wait_start(int router_id, int thread_id, uintptr_t mutex_addr);
static void handle_mutex_unlock_msg(int router_id, int thread_id, uintptr_t mutex_addr);

// Helper functions for sending specific responses
void send_success_response(int router_id, int thread_id, const char* request_id, const char* blocked_func, const char* message, const char* connection_id_str);
void send_error_response(int router_id, int thread_id, const char* request_id, const char* error_msg);
void send_timeout_response(int router_id, int thread_id, const char* request_id, const char* timeout_type);
void send_eagain_response(int router_id, int thread_id, const char* request_id);

// 直接向指定 fd 发送响应（用于 registration thread）
static void send_success_response_to_fd(int fd, const char* request_id, const char* message);

// Registration 线程入口函数
static void* registration_thread_func(void* arg);

// Connection management helper functions
int find_router_by_listen_address(const char *address, int caller_router_id);
void register_connection(int router_id, int socket_fd, int peer_router_id, int peer_socket_fd);
int find_peer_router(int router_id, int socket_fd);

// New helper for reading router messages and enqueuing events
// void read_and_enqueue_router_message(int router_id, int comm_fd);


// --- Main ---
int main(int argc, char *argv[]) {
    // 设置stdout和stderr为无缓冲，确保日志立即输出
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    // Parse expected router count from command line (default: 2)
    int expected_routers = 2;
    if (argc > 1) {
        expected_routers = atoi(argv[1]);
        if (expected_routers <= 0 || expected_routers > MAX_ROUTERS) {
            fprintf(stderr, "[DESD ERROR] Invalid router count: %d (must be 1-%d)\n", expected_routers, MAX_ROUTERS);
            fprintf(stderr, "[DESD-EXIT] Reason: Invalid command line argument. Code=1\n");
            exit(1);
        }
    }
    printf("[DESD] Starting daemon, expecting %d router(s) to connect.\n", expected_routers);
    
    // Setup control socket for initial connections
    if (access(DESD_CONTROL_SOCKET_PATH, F_OK) == 0) {
        unlink(DESD_CONTROL_SOCKET_PATH);
    }
    desd_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (desd_listen_fd < 0) {
        perror("[DESD ERROR] socket for listening");
        fprintf(stderr, "[DESD-EXIT] Reason: Failed to create control socket. Code=1\n");
        exit(1);
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DESD_CONTROL_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (bind(desd_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[DESD ERROR] bind for listening");
        close(desd_listen_fd);
        fprintf(stderr, "[DESD-EXIT] Reason: Failed to bind control socket. Code=1\n");
        exit(1);
    }
    if (listen(desd_listen_fd, expected_routers) < 0) {
        perror("[DESD ERROR] listen for connections");
        close(desd_listen_fd);
        fprintf(stderr, "[DESD-EXIT] Reason: Failed to listen on control socket. Code=1\n");
        exit(1);
    }
    printf("[DESD] Listening for initial libdeshook.so connections on %s\n", DESD_CONTROL_SOCKET_PATH);

    // 初始化 DESD 内部状态（router_states 等）
    init_desd();

    // Accept initial router connections and process REGISTER_ROUTER
    int connected_routers = 0;
    while (connected_routers < expected_routers) {
        printf("[DESD] Waiting for router %d/%d to connect...\n", connected_routers + 1, expected_routers);
        int conn_fd = accept(desd_listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            perror("[DESD ERROR] accept initial connection");
            continue;
        }
        printf("[DESD] Accepted initial connection (fd: %d).\n", conn_fd);

        // Blocking receive for REGISTER_ROUTER message
        char buffer[MAX_MSG_SIZE];
        ssize_t bytes_received = recv(conn_fd, buffer, MAX_MSG_SIZE - 1, 0);
        if (bytes_received <= 0) {
            fprintf(stderr, "[DESD ERROR] Failed to receive REGISTER_ROUTER from new connection (fd: %d).\n", conn_fd);
            close(conn_fd);
            continue;
        }
        buffer[bytes_received] = '\0';

        Message register_msg;
        json_to_message(buffer, &register_msg);

        if (register_msg.message_type == HOOK_TO_DESD && register_msg.event_type == ROUTER_START) {
            int router_id = register_msg.router_id;
            int thread_id = register_msg.thread_id;  // 从消息中获取 thread_id
            
            if (router_id > 0 && router_id <= MAX_ROUTERS && thread_id >= 0 && thread_id < MAX_THREADS_PER_ROUTER) {
                // 注册线程
                ThreadInfo *ti = register_thread(router_id, thread_id, conn_fd);
                if (ti) {
                    router_states[router_id].router_id = router_id;
                    
                    // 第一个线程注册时计数加一
                    if (thread_id == 0) {
                        connected_routers++;
                    }
                    
                    printf("[DESD] Router %d Thread %d registered with fd %d. (routers: %d/%d, threads: %d)\n", 
                           router_id, thread_id, conn_fd, connected_routers, expected_routers,
                           router_states[router_id].thread_count);
                    
                    // 第一个线程注册时调度 ROUTER_START 事件
                    // 方案 A：不立即回复 SUCCESS，等 handle_router_start 处理时再回复
                    if (thread_id == 0) {
                        Event start_event = {
                            .timestamp = 0.0,
                            .router_id = router_id,
                            .thread_id = 0,
                            .event_type = ROUTER_START,
                            .event_id = generate_event_id(),
                        };
                        // 将 request_id 写入 payload，供 handle_router_start 使用
                        snprintf(start_event.payload.json_str, MAX_MSG_SIZE,
                                 "{\"request_id\":\"%s\"}", register_msg.request_id);
                        push_event(start_event);
                        // 不再立即回复，线程将阻塞等待 handle_router_start 发送 SUCCESS
                        printf("[DESD] R%d T0 ROUTER_START event queued, thread will block until event is processed.\n", router_id);
                    } else {
                        // 非 T0 线程：同样不立即回复，等事件循环处理
                        Event start_event = {
                            .timestamp = 0.0,
                            .router_id = router_id,
                            .thread_id = thread_id,
                            .event_type = ROUTER_START,
                            .event_id = generate_event_id(),
                        };
                        snprintf(start_event.payload.json_str, MAX_MSG_SIZE,
                                 "{\"request_id\":\"%s\"}", register_msg.request_id);
                        push_event(start_event);
                        printf("[DESD] R%d T%d ROUTER_START event queued, thread will block until event is processed.\n", router_id, thread_id);
                    }
                } else {
                    fprintf(stderr, "[DESD ERROR] Failed to register thread %d for router %d\n", thread_id, router_id);
                    send_error_response(router_id, thread_id, register_msg.request_id, "Thread Registration Failed");
                    close(conn_fd);
                }
            } else {
                fprintf(stderr, "[DESD ERROR] Invalid router ID %d or thread ID %d (fd: %d).\n", router_id, thread_id, conn_fd);
                send_error_response(router_id, thread_id, register_msg.request_id, "Invalid Router or Thread ID");
                close(conn_fd);
            }
        } else {
            fprintf(stderr, "[DESD ERROR] Expected ROUTER_START message for registration, got type %d event %d from fd %d.\n",
                    register_msg.message_type, register_msg.event_type, conn_fd);
            send_error_response(0, 0, "unknown_req", "Invalid Registration Message");
            close(conn_fd);
        }
    }
    
    // 启动 registration 线程，用于运行期接受后续线程的注册
    // 不再 close(desd_listen_fd)，保留给 registration thread 使用
    registration_thread_running = 1;
    if (pthread_create(&registration_thread, NULL, registration_thread_func, NULL) != 0) {
        perror("[DESD ERROR] Failed to create registration thread");
        // 非致命错误，可以继续运行（但后续线程无法注册）
    } else {
        printf("[DESD] Registration thread started for handling additional thread connections.\n");
    }

    printf("[DESD] All routers (T0) connected and registered. Starting event loop...\n");
    desd_event_loop(); // Start the single-threaded event loop

    // 停止 registration 线程
    registration_thread_running = 0;
    close(desd_listen_fd);  // 关闭 listen fd，使 accept() 返回错误
    pthread_join(registration_thread, NULL);
    
    cleanup_desd();
    return 0;
}

// --- DESD Initialization and Cleanup ---
void init_desd() {
    for (int i = 0; i <= MAX_ROUTERS; ++i) {
        router_states[i].router_id = i;
        router_states[i].thread_count = 0;
        router_states[i].pending_packets_count = 0;
        router_states[i].pending_connections_count = 0;
        router_states[i].pending_conn_head = 0;
        router_states[i].pending_conn_tail = 0;
        router_states[i].listen_count = 0;
        
        // 初始化所有线程状态
        for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
            router_states[i].threads[t].thread_id = t;
            router_states[i].threads[t].comm_socket_fd = -1;
            router_states[i].threads[t].status = IDLE;
            router_states[i].threads[t].is_active = 0;
            router_states[i].threads[t].pending_timeout_event_id = 0;
            router_states[i].threads[t].monitored_fds_count = 0;
            memset(router_states[i].threads[t].blocked_on_request_id, 0, 64);
            memset(router_states[i].threads[t].blocked_on_function, 0, 64);
        }
        
        for (int j = 0; j < 10; j++) {
            memset(router_states[i].listen_addresses[j], 0, sizeof(router_states[i].listen_addresses[j]));
        }
        
        // 初始化连接表
        for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; ++j) {
            router_states[i].connections[j].socket_fd = -1;
            router_states[i].connections[j].peer_router_id = -1;
            router_states[i].connections[j].peer_socket_fd = -1;
            router_states[i].connections[j].connection_id = 0;
            router_states[i].connections[j].is_active = 0;
        }
        
        // 初始化数据包缓冲区
        for (int j = 0; j < MAX_PENDING_PACKETS; ++j) {
            router_states[i].packet_buffers[j].is_used = 0;
            router_states[i].packet_buffers[j].data_len = 0;
            router_states[i].packet_buffers[j].socket_fd = -1;
            memset(router_states[i].packet_buffers[j].data, 0, MAX_PACKET_SIZE);
            router_states[i].pending_buffer_indices[j] = -1;
        }
        router_states[i].pending_buffer_head = 0;
        router_states[i].pending_buffer_tail = 0;
    }
    for (int i = 0; i < MAX_ACTIVE_EVENTS; ++i) {
        event_active_status[i] = 0; // inactive
    }
}

void cleanup_desd() {
    unlink(DESD_CONTROL_SOCKET_PATH);
    for (int i = 0; i <= MAX_ROUTERS; ++i) {
        for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
            if (router_states[i].threads[t].comm_socket_fd != -1) {
                close(router_states[i].threads[t].comm_socket_fd);
            }
        }
    }
    printf("[DESD] Cleaned up.\n");
}

// --- Connection Management Helper Functions ---

// 根据监听地址查找路由器ID
int find_router_by_listen_address(const char *address, int caller_router_id) {
    // 提取目标地址的端口
    const char *target_port = strrchr(address, ':');
    if (!target_port) {
        // 没有端口，使用精确匹配（用于UDS路径）
        for (int i = 1; i <= MAX_ROUTERS; i++) {
            // 排除发起连接的router自己，避免自连接
            if (i == caller_router_id) {
                continue;
            }
            for (int j = 0; j < router_states[i].listen_count; j++) {
                if (strcmp(router_states[i].listen_addresses[j], address) == 0) {
                    return i;
                }
            }
        }
        return -1;
    }
    
    // 对于TCP地址（包含端口），优先从IP地址提取router_id
    // 网络规则：10.0.X.X → RX
    int octets[4];
    if (sscanf(address, "%d.%d.%d.%d:", &octets[0], &octets[1], &octets[2], &octets[3]) == 4) {
        if (octets[0] == 10 && octets[1] == 0) {
            // 从第三个八位提取router_id
            int extracted_router_id = octets[2];
            if (extracted_router_id > 0 && extracted_router_id <= MAX_ROUTERS &&
                extracted_router_id != caller_router_id &&
                router_states[extracted_router_id].thread_count > 0) {
                return extracted_router_id;
            }
        }
    }
    
    // 回退：尝试精确IP+端口匹配
    for (int i = 1; i <= MAX_ROUTERS; i++) {
        if (i == caller_router_id) continue;
        for (int j = 0; j < router_states[i].listen_count; j++) {
            if (strcmp(router_states[i].listen_addresses[j], address) == 0) {
                return i;
            }
        }
    }
    
    // 最后尝试0.0.0.0匹配
    for (int i = 1; i <= MAX_ROUTERS; i++) {
        if (i == caller_router_id) continue;
        for (int j = 0; j < router_states[i].listen_count; j++) {
            const char *listen_addr = router_states[i].listen_addresses[j];
            if (strncmp(listen_addr, "0.0.0.0:", 8) == 0) {
                const char *listen_port = listen_addr + 7;
                if (strcmp(listen_port, target_port) == 0) {
                    return i;
                }
            }
        }
    }
    return -1;
}

// 记录连接映射
void register_connection(int router_id, int socket_fd, int peer_router_id, int peer_socket_fd) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) {
        fprintf(stderr, "[DESD ERROR] register_connection: Invalid router_id %d\n", router_id);
        return;
    }
    
    // 🔑 先检查该socket_fd是否已经注册过
    // 如果存在，更新而不是创建新记录（防止同一FD多次注册导致查找混乱）
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (router_states[router_id].connections[i].is_active &&
            router_states[router_id].connections[i].socket_fd == socket_fd) {
            // FD已存在，更新连接信息（保留 connection_id，不要覆盖）
            printf("[DESD-UPDATE] R%d fd:%d already registered at slot %d (conn_id=%lu), updating peer info\n", 
                   router_id, socket_fd, i, router_states[router_id].connections[i].connection_id);
            router_states[router_id].connections[i].peer_router_id = peer_router_id;
            router_states[router_id].connections[i].peer_socket_fd = peer_socket_fd;
            router_states[router_id].connections[i].peer_closed = 0;
            // 注意：不再重置 connection_id，保留原有的连接标识
            printf("[DESD] Updated connection: R%d (fd:%d) <-> R%d (fd:%d)\n", 
                   router_id, socket_fd, peer_router_id, peer_socket_fd);
            return;
        }
    }
    
    // FD不存在，注册新连接
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (!router_states[router_id].connections[i].is_active) {
            router_states[router_id].connections[i].socket_fd = socket_fd;
            router_states[router_id].connections[i].peer_router_id = peer_router_id;
            router_states[router_id].connections[i].peer_socket_fd = peer_socket_fd;
            router_states[router_id].connections[i].is_active = 1;
            router_states[router_id].connections[i].peer_closed = 0;  // 初始化：对端未关闭
            printf("[DESD] Registered connection: R%d (fd:%d) <-> R%d (fd:%d)\n", 
                   router_id, socket_fd, peer_router_id, peer_socket_fd);
            return;
        }
    }
    fprintf(stderr, "[DESD ERROR] R%d connection table full!\n", router_id);
}

// 查找连接对端路由器
int find_peer_router(int router_id, int socket_fd) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) {
        return -1;
    }
    
    // 移除详细的内部查找输出
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (router_states[router_id].connections[i].is_active) {
            if (router_states[router_id].connections[i].socket_fd == socket_fd) {
                return router_states[router_id].connections[i].peer_router_id;
            }
        }
    }
    return -1;
}

// 查找路由器连接到特定对端路由器的 socket_fd（精确匹配对端的 socket_fd）
int find_socket_fd_for_peer(int router_id, int peer_router_id, int peer_socket_fd) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) {
        return -1;
    }
    
    int potential_match_fd = -1;
    unsigned long potential_match_conn_id = 0;
    int potential_match_peer_closed = 0;
    int matches_found = 0;

    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (router_states[router_id].connections[i].is_active &&
            router_states[router_id].connections[i].peer_router_id == peer_router_id) {

            int local_fd = router_states[router_id].connections[i].socket_fd;
            int peer_fd = router_states[router_id].connections[i].peer_socket_fd;
            unsigned long conn_id = router_states[router_id].connections[i].connection_id;
            int peer_closed = router_states[router_id].connections[i].peer_closed;

            // 详细记录当前候选条目，方便调试FD复用/映射问题
            printf("[DESD-CONN-MAP] R%d candidate idx=%d local_fd=%d peer_rid=%d peer_fd=%d conn_id=%lu peer_closed=%d\n",
                   router_id, i, local_fd, peer_router_id, peer_fd, conn_id, peer_closed);
            
            // 完美匹配：对端 FD 也对得上
            if (peer_fd == peer_socket_fd) {
                printf("[DESD-CONN-MAP] R%d PERFECT match for peer R%d: local_fd=%d peer_fd=%d conn_id=%lu peer_closed=%d\n",
                       router_id, peer_router_id, local_fd, peer_fd, conn_id, peer_closed);
                return local_fd;
            }
            
            // 候选匹配：对端 FD 还是 -1（说明对端还未发送 CONNECTION_INFO_EVENT，或对端已关闭但记录未清理）
            if (peer_fd == -1) {
                potential_match_fd = local_fd;
                potential_match_conn_id = conn_id;
                potential_match_peer_closed = peer_closed;
                matches_found++;
            }
        }
    }
    
    // 如果没有完美匹配，但只有一个候选匹配，我们也认为是对的（适用于单连接或时序滞后场景）
    if (matches_found == 1) {
        printf("[DESD-CONN-MAP] R%d using POTENTIAL match local_fd=%d for peer R%d (exact peer_fd %d not yet registered). potential_conn_id=%lu peer_closed=%d\n",
               router_id, potential_match_fd, peer_router_id, peer_socket_fd,
               potential_match_conn_id, potential_match_peer_closed);
        return potential_match_fd;
    }
    
    return -1;
}

// 调试辅助：打印指定路由器的所有活跃连接映射
static void dump_router_connections(int router_id, const char *tag) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) {
        return;
    }

    const char *label = tag ? tag : "NO_TAG";
    printf("[DESD-CONN-DUMP] %s R%d BEGIN\n", label, router_id);

    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (router_states[router_id].connections[i].is_active) {
            int local_fd = router_states[router_id].connections[i].socket_fd;
            int peer_rid = router_states[router_id].connections[i].peer_router_id;
            int peer_fd = router_states[router_id].connections[i].peer_socket_fd;
            unsigned long conn_id = router_states[router_id].connections[i].connection_id;
            int peer_closed = router_states[router_id].connections[i].peer_closed;

            printf("[DESD-CONN-DUMP] %s R%d idx=%d local_fd=%d peer_rid=%d peer_fd=%d conn_id=%lu peer_closed=%d\n",
                   label, router_id, i, local_fd, peer_rid, peer_fd, conn_id, peer_closed);
        }
    }

    printf("[DESD-CONN-DUMP] %s R%d END\n", label, router_id);
}

// --- Event Queue Management (Min-Heap Implementation) ---
// 比较两个事件的优先级：先按 timestamp，再按 event_id（确保同时间戳事件按入队顺序处理）
static int event_less_than(const Event* a, const Event* b) {
    if (a->timestamp != b->timestamp) {
        return a->timestamp < b->timestamp;
    }
    // 时间戳相同时，event_id 小的优先（先入队的先处理）
    return a->event_id < b->event_id;
}

void heapify_up(int idx) {
    while (idx > 0 && event_less_than(&event_queue[idx], &event_queue[(idx - 1) / 2])) {
        Event temp = event_queue[idx];
        event_queue[idx] = event_queue[(idx - 1) / 2];
        event_queue[(idx - 1) / 2] = temp;
        idx = (idx - 1) / 2;
    }
}

void heapify_down(int idx) {
    int left_child = 2 * idx + 1;
    int right_child = 2 * idx + 2;
    int smallest = idx;

    if (left_child < event_queue_size && event_less_than(&event_queue[left_child], &event_queue[smallest])) {
        smallest = left_child;
    }
    if (right_child < event_queue_size && event_less_than(&event_queue[right_child], &event_queue[smallest])) {
        smallest = right_child;
    }

    if (smallest != idx) {
        Event temp = event_queue[idx];
        event_queue[idx] = event_queue[smallest];
        event_queue[smallest] = temp;
        heapify_down(smallest);
    }
}

void push_event(Event new_event) {
    pthread_mutex_lock(&event_queue_mutex);

    // 调试限制：不再接收超过 MAX_TOTAL_EVENTS 的事件
    if (new_event.event_id >= MAX_TOTAL_EVENTS) {
        fprintf(stderr, "[DESD WARNING] Event ID %lu exceeds MAX_TOTAL_EVENTS (%d), dropping.\n",
                new_event.event_id, MAX_TOTAL_EVENTS);
        pthread_mutex_unlock(&event_queue_mutex);
        return;
    }

    if (event_queue_size >= MAX_EVENT_QUEUE_SIZE) {
        fprintf(stderr, "[DESD ERROR] Event queue is full (size=%d, max=%d)!\n",
                event_queue_size, MAX_EVENT_QUEUE_SIZE);
        pthread_mutex_unlock(&event_queue_mutex);
        return;
    }
    event_queue[event_queue_size] = new_event;
    heapify_up(event_queue_size);
    
    // 标记事件为活跃状态
    if (new_event.event_id < MAX_ACTIVE_EVENTS) {
        event_active_status[new_event.event_id] = 1;
        // 方案二：记录事件所有者信息
        event_owner_info[new_event.event_id].router_id = new_event.router_id;
        event_owner_info[new_event.event_id].thread_id = new_event.thread_id;
    }
    event_queue_size++;
    
    // 方案二：增加该线程的 pending_event_count
    ThreadInfo *ti = get_thread_info(new_event.router_id, new_event.thread_id);
    if (ti) {
        ti->pending_event_count++;
    }
    pthread_cond_signal(&event_queue_cond); // Signal event loop (if it's waiting)
    pthread_mutex_unlock(&event_queue_mutex);
}

Event pop_event() {
    pthread_mutex_lock(&event_queue_mutex);
    while (event_queue_size == 0) {
        // 等待新事件的到来
        pthread_cond_wait(&event_queue_cond, &event_queue_mutex);
    }

    Event root = event_queue[0];
    // 标记事件为非活跃状态 (只有在事件被取消时才应该设置)
    // if (root.event_id < MAX_ACTIVE_EVENTS) {
    //     event_active_status[root.event_id] = 0; // Clear status
    // }

    event_queue_size--;
    event_queue[0] = event_queue[event_queue_size];
    heapify_down(0);

    pthread_mutex_unlock(&event_queue_mutex);
    return root;
}

// 取消事件：将事件标记为非活跃状态，事件循环会跳过它
void cancel_event(unsigned long event_id) {
    pthread_mutex_lock(&event_queue_mutex);
    if (event_id < MAX_ACTIVE_EVENTS && event_active_status[event_id] == 1) {
        event_active_status[event_id] = 0; // Mark as inactive
        printf("[DESD] Canceled event %lu.\n", event_id);
        
        // 方案二：减少对应线程的 pending_event_count
        int router_id = event_owner_info[event_id].router_id;
        int thread_id = event_owner_info[event_id].thread_id;
        ThreadInfo *ti = get_thread_info(router_id, thread_id);
        if (ti && ti->pending_event_count > 0) {
            ti->pending_event_count--;
        }
    }
    pthread_mutex_unlock(&event_queue_mutex);
}

int is_event_queue_empty() {
    pthread_mutex_lock(&event_queue_mutex);
    int empty = (event_queue_size == 0);
    pthread_mutex_unlock(&event_queue_mutex);
    return empty;
}

// --- Router Communication (Per-Thread) ---
// 这些函数用于desd与libdeshook之间的控制消息通信

void send_message_to_thread(int router_id, int thread_id, const Message* msg) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) {
        fprintf(stderr, "[DESD ERROR] Invalid router ID %d for send_message_to_thread.\n", router_id);
        return;
    }
    if (thread_id < 0 || thread_id >= MAX_THREADS_PER_ROUTER) {
        fprintf(stderr, "[DESD ERROR] Invalid thread ID %d for R%d.\n", thread_id, router_id);
        return;
    }
    
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    if (!ti || ti->comm_socket_fd == -1) {
        fprintf(stderr, "[DESD ERROR] Cannot send message to R%d T%d: Not connected.\n", router_id, thread_id);
        return;
    }
    
    char *json_str = message_to_json(msg);
    if (!json_str) {
        fprintf(stderr, "[DESD ERROR] Failed to serialize message for R%d T%d.\n", router_id, thread_id);
        return;
    }
    
    size_t json_len = strlen(json_str);
    char *json_with_newline = (char*)malloc(json_len + 2);
    if (!json_with_newline) {
        fprintf(stderr, "[DESD ERROR] Failed to allocate memory for message to R%d T%d.\n", router_id, thread_id);
        free(json_str);
        return;
    }
    strcpy(json_with_newline, json_str);
    json_with_newline[json_len] = '\n';
    json_with_newline[json_len + 1] = '\0';
    free(json_str);
    
    if (send(ti->comm_socket_fd, json_with_newline, strlen(json_with_newline), 0) < 0) {
        perror("[DESD ERROR] send_message_to_thread");
    }
    free(json_with_newline);
}

// 兼容函数：使用消息中的 thread_id
void send_message_to_router(int router_id, const Message* msg) {
    send_message_to_thread(router_id, msg->thread_id, msg);
}

int receive_blocking_from_thread(int router_id, int thread_id, Message* msg_out) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) {
        fprintf(stderr, "[DESD ERROR] Invalid router ID %d for receive_blocking_from_thread.\n", router_id);
        return 0;
    }
    
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    if (!ti || ti->comm_socket_fd == -1) {
        fprintf(stderr, "[DESD ERROR] R%d T%d communication FD not found for blocking receive.\n", router_id, thread_id);
        return 0;
    }
    
    int comm_fd = ti->comm_socket_fd;
    char buffer[MAX_MSG_SIZE];
    ssize_t bytes_received = recv(comm_fd, buffer, MAX_MSG_SIZE - 1, 0);

    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            fprintf(stderr, "[DESD ERROR] R%d T%d disconnected during blocking receive.\n", router_id, thread_id);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "[DESD ERROR] R%d T%d timed out waiting for next event.\n", router_id, thread_id);
            } else {
                perror("[DESD ERROR] receive_blocking_from_thread");
            }
        }
        ti->comm_socket_fd = -1;
        ti->status = IDLE;
        ti->is_active = 0;
        return 0;
    }
    buffer[bytes_received] = '\0';

    json_to_message(buffer, msg_out);
    //GET_VIRTUAL_TIME_EVENT 不打印
    if (msg_out->event_type != GET_VIRTUAL_TIME_EVENT) {
        printf("[DESD-received-message] Received message (Type: %s, Event: %s, ReqID: %s) from R%d.\n",
               msg_out->message_type == HOOK_TO_DESD ? "HOOK_TO_DESD" : "DESD_TO_HOOK",
               event_type_to_string(msg_out->event_type), msg_out->request_id, router_id);
    }

    // 极轻量统计 GET_VIRTUAL_TIME_EVENT 调用次数，避免为每次调用都打印日志
    if (msg_out->event_type == GET_VIRTUAL_TIME_EVENT) {
        static unsigned long get_vtime_recv_count = 0;
        get_vtime_recv_count++;

        if (get_vtime_recv_count == 1 ||
            get_vtime_recv_count == 10 ||
            get_vtime_recv_count == 100 ||
            (get_vtime_recv_count % 1000) == 0) {
            printf("[DESD-GETVT-RECV] R%d T%d received GET_VIRTUAL_TIME_EVENT %lu time(s) (ReqID: %s, current_VT=%.6f)\n",
                   router_id, thread_id, get_vtime_recv_count,
                   msg_out->request_id, current_virtual_time);
        }
    }
    
    return 1; // Success
}

// Helper to convert EventType to string for logging
const char* event_type_to_string(EventType type) {
    switch (type) {
        case ROUTER_START: return "ROUTER_START";
        case ROUTER_BLOCK_REQUEST: return "ROUTER_BLOCK_REQUEST";
        case PACKET_SEND_EVENT: return "PACKET_SEND_EVENT";
        case PACKET_RECEIVE_EVENT: return "PACKET_RECEIVE_EVENT";
        case TIMEOUT_EVENT: return "TIMEOUT_EVENT";
        case CONNECT_REQUEST_EVENT: return "CONNECT_REQUEST_EVENT";
        case CONNECTION_ESTABLISHED_EVENT: return "CONNECTION_ESTABLISHED_EVENT";
        case LISTEN_EVENT: return "LISTEN_EVENT";
        case CONNECTION_INFO_EVENT: return "CONNECTION_INFO_EVENT";
        case CLOSE_SOCKET_EVENT: return "CLOSE_SOCKET_EVENT";
        case GET_VIRTUAL_TIME_EVENT: return "GET_VIRTUAL_TIME_EVENT";
        case CANCEL_BLOCK_REQUEST: return "CANCEL_BLOCK_REQUEST";
        case MUTEX_LOCK_ACQUIRED: return "MUTEX_LOCK_ACQUIRED";
        case MUTEX_WAIT_START: return "MUTEX_WAIT_START";
        case MUTEX_UNLOCK: return "MUTEX_UNLOCK";
        case MUTEX_RETRY_EVENT: return "MUTEX_RETRY_EVENT";
        default: return "UNKNOWN_EVENT";
    }
}

// --- 新架构核心函数 ---

// Forward declaration
void wait_for_detached_threads_to_rehook();

// drain_all_messages_nonblocking: 在每次 pop_event 之前调用
// 非阻塞地扫描所有线程的 socket，将到达的消息转换为事件入队
// - 所有消息类型（包括 GET_VIRTUAL_TIME_EVENT）统一入队，由主事件循环处理
// - 如果线程处于 DETACHED_WAITING 状态，收到任何消息都表示它已重新 hook
void drain_all_messages_nonblocking() {
    struct pollfd poll_fds[MAX_ROUTERS * MAX_THREADS_PER_ROUTER];
    int fd_to_router[MAX_ROUTERS * MAX_THREADS_PER_ROUTER];
    int fd_to_thread[MAX_ROUTERS * MAX_THREADS_PER_ROUTER];
    
    while (1) {
        int poll_count = 0;
        
        // 收集所有活跃线程的 socket fd
        pthread_mutex_lock(&router_states_mutex);
        for (int r = 1; r <= MAX_ROUTERS; r++) {
            for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
                ThreadInfo *ti = &router_states[r].threads[t];
                if (ti->is_active && ti->comm_socket_fd >= 0) {
                    poll_fds[poll_count].fd = ti->comm_socket_fd;
                    poll_fds[poll_count].events = POLLIN;
                    poll_fds[poll_count].revents = 0;
                    fd_to_router[poll_count] = r;
                    fd_to_thread[poll_count] = t;
                    poll_count++;
                }
            }
        }
        pthread_mutex_unlock(&router_states_mutex);
        
        if (poll_count == 0) {
            return; // 没有活跃线程
        }
        
        // 非阻塞 poll (timeout = 0)
        int poll_result = poll(poll_fds, poll_count, 0);
        
        if (poll_result <= 0) {
            // 没有数据可读，drain 完成
            return;
        }
        
        // 处理所有有数据的 fd
        for (int i = 0; i < poll_count; i++) {
            if (!(poll_fds[i].revents & POLLIN)) {
                continue;
            }
            
            int router_id = fd_to_router[i];
            int thread_id = fd_to_thread[i];
            
            // 使用按行拆包的 helper 读取消息
            pthread_mutex_lock(&router_states_mutex);
            ThreadInfo *ti = get_thread_info(router_id, thread_id);
            pthread_mutex_unlock(&router_states_mutex);
            
            if (!ti) {
                continue;
            }
            
            // 方案一：循环读取同一 fd 上所有完整 JSON 消息
            // 解决 CANCEL + GETVT 粘在一次 recv 时只处理 CANCEL 的问题
            while (1) {
                Message msg;
                int read_result = read_one_json_message(ti, MSG_DONTWAIT, &msg);
                
                if (read_result < 0) {
                    // 连接断开或错误
                    fprintf(stderr,
                            "[DESD ERROR] drain_all_messages_nonblocking: R%d T%d read_one_json_message returned < 0 (status=%d, blocked_on_request_id='%s', blocked_on_function='%s', pending_timeout_event_id=%lu, monitored_fds_count=%d, VT=%.6f)\n",
                            router_id,
                            thread_id,
                            ti ? ti->status : -1,
                            ti ? ti->blocked_on_request_id : "",
                            ti ? ti->blocked_on_function : "",
                            ti ? ti->pending_timeout_event_id : 0,
                            ti ? ti->monitored_fds_count : 0,
                            current_virtual_time);
                    pthread_mutex_lock(&router_states_mutex);
                    ti = get_thread_info(router_id, thread_id);
                    if (ti) {
                        ti->comm_socket_fd = -1;
                        ti->status = IDLE;
                        ti->is_active = 0;
                        ti->recv_len = 0;  // 清空接收缓冲区
                    }
                    pthread_mutex_unlock(&router_states_mutex);
                    break;  // 连接断开，退出此 fd 的循环
                } else if (read_result == 0) {
                    // 没有完整消息可读（只有半包或无数据），退出此 fd 的循环
                    break;
                }
                
                // 成功读取一条消息
                pthread_mutex_lock(&router_states_mutex);
                ti = get_thread_info(router_id, thread_id);
                RouterStatus thread_status = ti ? ti->status : IDLE;
                
                // 如果该线程处于 DETACHED_WAITING 状态，收到任何消息意味着它已经重新 hook 回来
                if (ti && ti->detached_waiting) {
                    ti->detached_waiting = 0;
                    num_detached_waiting--;
                    printf("[DESD-DETACH] R%d T%d re-hooked at VT=%.3f with %s (remaining detached: %d)\n",
                           router_id, thread_id, current_virtual_time, 
                           event_type_to_string(msg.event_type), num_detached_waiting);
                }
                pthread_mutex_unlock(&router_states_mutex);
                
                // 根据消息类型和线程状态分类处理
                if (msg.event_type == CANCEL_BLOCK_REQUEST) {
                    // ===== 方案二: CANCEL 入队处理 =====
                    // 这样可以保证 CANCEL 事件在对应的 BLOCK 事件之后被处理，消除竞态条件
                    // handle_cancel_block_request 会根据 pending_event_count 决定是否进入 DETACHED_WAITING
                    Event cancel_event = {
                        .timestamp = current_virtual_time,
                        .router_id = router_id,
                        .thread_id = thread_id,
                        .event_type = CANCEL_BLOCK_REQUEST,
                        .event_id = generate_event_id(),
                        .payload = msg.payload
                    };
                    push_event(cancel_event);
                    printf("[DESD-DRAIN] R%d T%d sent CANCEL_BLOCK_REQUEST (ReqID: %s), queued at VT=%.3f (EventID: %lu).\n",
                           router_id, thread_id, msg.request_id, current_virtual_time, cancel_event.event_id);
                    
                } else if (msg.event_type == GET_VIRTUAL_TIME_EVENT) {
                    // GET_VIRTUAL_TIME: 统一入队处理，由主事件循环分发到 handle_get_virtual_time_event
                    // 重要：GETVT 也是阻塞型 RPC，线程在等 DESD 回复期间是 BLOCKED 状态
                    Event vt_event = {
                        .timestamp = current_virtual_time,
                        .router_id = router_id,
                        .thread_id = thread_id,
                        .event_type = GET_VIRTUAL_TIME_EVENT,
                        .event_id = generate_event_id(),
                        .payload = msg.payload
                    };
                    push_event(vt_event);
                    
                    // 标记线程为 BLOCKED，等待 GETVT 事件被处理并回复
                    pthread_mutex_lock(&router_states_mutex);
                    ti = get_thread_info(router_id, thread_id);
                    if (ti) {
                        strncpy(ti->blocked_on_request_id, msg.request_id, 63);
                        ti->blocked_on_request_id[63] = '\0';
                        ti->status = BLOCKED;
                    }
                    pthread_mutex_unlock(&router_states_mutex);
                    
                    printf("[DESD-DRAIN] R%d T%d sent GET_VIRTUAL_TIME_EVENT (ReqID: %s), marked BLOCKED, queued at VT=%.3f.\n",
                           router_id, thread_id, msg.request_id, current_virtual_time);
                    
                } else if (msg.event_type == ROUTER_BLOCK_REQUEST) {
                    // ===== ROUTER_BLOCK_REQUEST 特殊处理：只入队，不改 status =====
                    // 原因：handle_router_block_request 会根据是否有 ready fd/pending packet 等
                    // 决定是"立即唤醒"（status 保持 RUNNING）还是"真正阻塞"（status 改为 BLOCKED）。
                    // 如果在 drain 阶段预先标记为 BLOCKED，会导致"立即唤醒"的 BLOCK 被视为
                    // BLOCKED->RUNNING 转换，从而错误地触发 interact_with_router_until_it_blocks。
                    Event block_event = {
                        .timestamp = current_virtual_time,
                        .router_id = router_id,
                        .thread_id = thread_id,
                        .event_type = msg.event_type,
                        .event_id = generate_event_id(),
                        .payload = msg.payload
                    };
                    push_event(block_event);
                    
                    const char* status_desc = (thread_status == RUNNING_DETACHED) ? "was DETACHED" : 
                                             (thread_status == RUNNING) ? "was RUNNING" :
                                             (thread_status == IDLE) ? "was IDLE" : "unknown status";
                    printf("[DESD-DRAIN] R%d T%d (%s) sent ROUTER_BLOCK_REQUEST (ReqID: %s), queued at VT=%.3f (EventID: %lu). Status NOT changed.\n",
                           router_id, thread_id, status_desc, msg.request_id, current_virtual_time, block_event.event_id);
                    
                } else if (msg.event_type == PACKET_SEND_EVENT ||
                           msg.event_type == CONNECT_REQUEST_EVENT ||
                           thread_status == RUNNING_DETACHED) {
                    // 阻塞类请求（包括来自 RUNNING_DETACHED 线程的"重新 hook"请求）
                    // 构造 Event 入队，标记线程为 BLOCKED
                    double event_timestamp = current_virtual_time;
                    if (msg.event_type == PACKET_SEND_EVENT) {
                        event_timestamp = current_virtual_time + 0.002;
                    }
                    
                    Event new_event = {
                        .timestamp = event_timestamp,
                        .router_id = router_id,
                        .thread_id = thread_id,
                        .event_type = msg.event_type,
                        .event_id = generate_event_id(),
                        .payload = msg.payload
                    };
                    
                    // 记录线程正在等待的请求，标记为 BLOCKED
                    pthread_mutex_lock(&router_states_mutex);
                    ti = get_thread_info(router_id, thread_id);
                    if (ti) {
                        strncpy(ti->blocked_on_request_id, msg.request_id, 63);
                        ti->blocked_on_request_id[63] = '\0';
                        ti->status = BLOCKED; // 标记为 BLOCKED，等待事件被 pop 时处理
                    }
                    pthread_mutex_unlock(&router_states_mutex);
                    
                    push_event(new_event);
                    
                    const char* status_desc = (thread_status == RUNNING_DETACHED) ? "was DETACHED" : 
                                             (thread_status == RUNNING) ? "was RUNNING" :
                                             (thread_status == IDLE) ? "was IDLE" : "unknown status";
                    printf("[DESD-DRAIN] R%d T%d (%s) sent %s (ReqID: %s), marked BLOCKED, queued at VT=%.3f (EventID: %lu).\n",
                           router_id, thread_id, status_desc, event_type_to_string(msg.event_type),
                           msg.request_id, event_timestamp, new_event.event_id);
                    
                } else if (msg.event_type == LISTEN_EVENT ||
                           msg.event_type == CONNECTION_INFO_EVENT ||
                           msg.event_type == CLOSE_SOCKET_EVENT) {
                    // LISTEN/CONNECTION_INFO/CLOSE_SOCKET 也是阻塞型 RPC，需要标记 BLOCKED
                    Event instant_event = {
                        .timestamp = current_virtual_time,
                        .router_id = router_id,
                        .thread_id = thread_id,
                        .event_type = msg.event_type,
                        .event_id = generate_event_id(),
                        .payload = msg.payload
                    };
                    push_event(instant_event);
                    
                    // 标记线程为 BLOCKED，等待事件被处理并回复
                    pthread_mutex_lock(&router_states_mutex);
                    ti = get_thread_info(router_id, thread_id);
                    if (ti) {
                        strncpy(ti->blocked_on_request_id, msg.request_id, 63);
                        ti->blocked_on_request_id[63] = '\0';
                        ti->status = BLOCKED;
                    }
                    pthread_mutex_unlock(&router_states_mutex);
                    
                    printf("[DESD-DRAIN] R%d T%d sent %s (ReqID: %s), marked BLOCKED, queued at VT=%.3f.\n",
                           router_id, thread_id, event_type_to_string(msg.event_type),
                           msg.request_id, current_virtual_time);
                           
                } else if (msg.event_type == MUTEX_LOCK_ACQUIRED ||
                           msg.event_type == MUTEX_WAIT_START ||
                           msg.event_type == MUTEX_UNLOCK) {
                    // === Mutex Hook 消息：即时处理，不入队列 ===
                    // 从 payload 解析 mutex_addr
                    uintptr_t mutex_addr = 0;
                    json_error_t json_err;
                    json_t *mutex_payload = json_loads(msg.payload.json_str, 0, &json_err);
                    if (mutex_payload) {
                        json_t *addr_json = json_object_get(mutex_payload, "mutex_addr");
                        if (addr_json && json_is_integer(addr_json)) {
                            mutex_addr = (uintptr_t)json_integer_value(addr_json);
                        }
                        json_decref(mutex_payload);
                    }
                    
                    if (msg.event_type == MUTEX_LOCK_ACQUIRED) {
                        handle_mutex_lock_acquired(router_id, thread_id, mutex_addr);
                    } else if (msg.event_type == MUTEX_WAIT_START) {
                        handle_mutex_wait_start(router_id, thread_id, mutex_addr);
                    } else if (msg.event_type == MUTEX_UNLOCK) {
                        handle_mutex_unlock_msg(router_id, thread_id, mutex_addr);
                    }
                    // Mutex 消息不需要回复，继续处理下一条消息
                    
                } else {
                    // 未知消息类型
                    fprintf(stderr, "[DESD-DRAIN WARNING] Unknown message from R%d T%d (status=%d): %s\n",
                            router_id, thread_id, thread_status, event_type_to_string(msg.event_type));
                }
            }  // end while(1) for this fd
        }
        // 继续循环，直到没有更多数据
    }
}

// interact_with_router_until_it_blocks: 与单个路由器的单个线程交互直到它再次阻塞
// 职责单一化：只监听 active_router 的 active_thread_id 线程
// 其他 router/thread 的消息由主循环开头的 drain_all_messages_nonblocking() 统一处理
void interact_with_router_until_it_blocks(int active_router_id, int active_thread_id) {
    struct pollfd poll_fd;
    
    while (1) {
        // 检查当前线程是否仍然 RUNNING
        pthread_mutex_lock(&router_states_mutex);
        ThreadInfo *ti = get_thread_info(active_router_id, active_thread_id);
        if (!ti || !ti->is_active || ti->comm_socket_fd < 0 || ti->status != RUNNING) {
            // 线程不存在、不活跃、或者已经不是 RUNNING 状态，退出 interact
            pthread_mutex_unlock(&router_states_mutex);
            return;
        }
        poll_fd.fd = ti->comm_socket_fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        pthread_mutex_unlock(&router_states_mutex);
        
        // 阻塞等待该线程有数据
        int poll_result = poll(&poll_fd, 1, -1);
        
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue; // 被信号中断，重试
            }
            perror("[DESD ERROR] interact_with_router_until_it_blocks poll");
            return;
        }
        
        // 检查是否有数据
        if (!(poll_fd.revents & POLLIN)) {
            continue;
        }
        
        int router_id = active_router_id;
        int thread_id = active_thread_id;
        
        // 使用按行拆包的 helper 读取消息
        pthread_mutex_lock(&router_states_mutex);
        ti = get_thread_info(router_id, thread_id);
        pthread_mutex_unlock(&router_states_mutex);
        
        if (!ti) {
            return;
        }
        
        // 循环读取同一 fd 上所有完整 JSON 消息
        // 解决 CANCEL + GETVT 粘在一次 recv 时只处理 CANCEL 的问题
        while (1) {
            // interact 阶段：用非阻塞读取（MSG_DONTWAIT）
            // 因为 poll 只告诉我们有数据，但不知道有几条完整 JSON
            Message msg;
            int read_result = read_one_json_message(ti, MSG_DONTWAIT, &msg);
            
            if (read_result < 0) {
                // 连接断开或错误
                fprintf(stderr, "[DESD ERROR] R%d T%d disconnected or read error.\n", router_id, thread_id);
                pthread_mutex_lock(&router_states_mutex);
                ti = get_thread_info(router_id, thread_id);
                if (ti) {
                    fprintf(stderr,
                            "[DESD ERROR] R%d T%d disconnect context: status=%d, blocked_on_request_id='%s', blocked_on_function='%s', pending_timeout_event_id=%lu, monitored_fds_count=%d, VT=%.6f\n",
                            router_id,
                            thread_id,
                            ti->status,
                            ti->blocked_on_request_id,
                            ti->blocked_on_function,
                            ti->pending_timeout_event_id,
                            ti->monitored_fds_count,
                            current_virtual_time);
                    ti->comm_socket_fd = -1;
                    ti->status = IDLE;
                    ti->is_active = 0;
                    ti->recv_len = 0;  // 清空接收缓冲区
                }
                pthread_mutex_unlock(&router_states_mutex);
                return;  // 连接断开，退出 interact
            } else if (read_result == 0) {
                // 没有完整消息可读，退出内层循环，外层会重新检查线程状态
                break;
            }
            
            // 成功读取一条消息
            pthread_mutex_lock(&router_states_mutex);
            ti = get_thread_info(router_id, thread_id);
            
            // 如果该线程处于 DETACHED_WAITING 状态，收到任何消息意味着它已经重新 hook 回来
            if (ti && ti->detached_waiting) {
                ti->detached_waiting = 0;
                num_detached_waiting--;
                printf("[DESD-DETACH] R%d T%d re-hooked at VT=%.3f with %s (remaining detached: %d)\n",
                       router_id, thread_id, current_virtual_time, 
                       event_type_to_string(msg.event_type), num_detached_waiting);
            }
            pthread_mutex_unlock(&router_states_mutex);
            
            // ===== 处理 active_thread 的消息 =====
            
            // CANCEL 入队处理，继续读取后续消息（不 return）
            if (msg.event_type == CANCEL_BLOCK_REQUEST) {
                Event cancel_event = {
                    .timestamp = current_virtual_time,
                    .router_id = router_id,
                    .thread_id = thread_id,
                    .event_type = CANCEL_BLOCK_REQUEST,
                    .event_id = generate_event_id(),
                    .payload = msg.payload
                };
                push_event(cancel_event);
                printf("[DESD-INTERACT] R%d T%d sent CANCEL_BLOCK_REQUEST, queued at VT=%.3f (EventID: %lu). Continuing to read more messages.\n",
                       router_id, thread_id, current_virtual_time, cancel_event.event_id);
                continue;  // 继续读取下一条消息
            }
            
            // GET_VIRTUAL_TIME: 统一入队处理，立即返回主循环让事件被处理
            // 重要：GETVT 也是阻塞型 RPC，线程在等 DESD 回复期间是 BLOCKED 状态
            if (msg.event_type == GET_VIRTUAL_TIME_EVENT) {
                Event vt_event = {
                    .timestamp = current_virtual_time,
                    .router_id = router_id,
                    .thread_id = thread_id,
                    .event_type = GET_VIRTUAL_TIME_EVENT,
                    .event_id = generate_event_id(),
                    .payload = msg.payload
                };
                push_event(vt_event);
                
                // 标记线程为 BLOCKED，等待 GETVT 事件被处理并回复
                pthread_mutex_lock(&router_states_mutex);
                ti = get_thread_info(router_id, thread_id);
                if (ti) {
                    strncpy(ti->blocked_on_request_id, msg.request_id, 63);
                    ti->blocked_on_request_id[63] = '\0';
                    ti->status = BLOCKED;
                }
                pthread_mutex_unlock(&router_states_mutex);
                
                printf("[DESD-INTERACT] R%d T%d sent GET_VIRTUAL_TIME_EVENT (ReqID: %s), marked BLOCKED, queued at VT=%.3f. Returning to main loop.\n",
                       router_id, thread_id, msg.request_id, current_virtual_time);
                return;  // 返回主循环处理 GETVT
            }
            
            // LISTEN/CONNECTION_INFO/CLOSE_SOCKET: 也是阻塞型 RPC，需要标记 BLOCKED
            if (msg.event_type == LISTEN_EVENT ||
                msg.event_type == CONNECTION_INFO_EVENT ||
                msg.event_type == CLOSE_SOCKET_EVENT) {
                Event instant_event = {
                    .timestamp = current_virtual_time,
                    .router_id = router_id,
                    .thread_id = thread_id,
                    .event_type = msg.event_type,
                    .event_id = generate_event_id(),
                    .payload = msg.payload
                };
                push_event(instant_event);
                
                // 标记线程为 BLOCKED，等待事件被处理并回复
                pthread_mutex_lock(&router_states_mutex);
                ti = get_thread_info(router_id, thread_id);
                if (ti) {
                    strncpy(ti->blocked_on_request_id, msg.request_id, 63);
                    ti->blocked_on_request_id[63] = '\0';
                    ti->status = BLOCKED;
                }
                pthread_mutex_unlock(&router_states_mutex);
                
                printf("[DESD-INTERACT] R%d T%d sent %s (ReqID: %s), marked BLOCKED, queued at VT=%.3f. Returning to main loop.\n",
                       router_id, thread_id, event_type_to_string(msg.event_type),
                       msg.request_id, current_virtual_time);
                return;  // 返回主循环处理
            }
            
            // === Mutex Hook 消息：即时处理，不入队列 ===
            if (msg.event_type == MUTEX_LOCK_ACQUIRED ||
                msg.event_type == MUTEX_WAIT_START ||
                msg.event_type == MUTEX_UNLOCK) {
                // 从 payload 解析 mutex_addr
                uintptr_t mutex_addr = 0;
                json_error_t json_err;
                json_t *mutex_payload = json_loads(msg.payload.json_str, 0, &json_err);
                if (mutex_payload) {
                    json_t *addr_json = json_object_get(mutex_payload, "mutex_addr");
                    if (addr_json && json_is_integer(addr_json)) {
                        mutex_addr = (uintptr_t)json_integer_value(addr_json);
                    }
                    json_decref(mutex_payload);
                }
                
                if (msg.event_type == MUTEX_LOCK_ACQUIRED) {
                    handle_mutex_lock_acquired(router_id, thread_id, mutex_addr);
                    continue;  // 继续读取下一条消息
                } else if (msg.event_type == MUTEX_WAIT_START) {
                    handle_mutex_wait_start(router_id, thread_id, mutex_addr);
                    // MUTEX_WAIT_START 会将线程标记为 BLOCKED
                    // 外层 while 开头会检查线程状态，发现 BLOCKED 后会自动退出
                    break;  // 退出内层读取循环
                } else if (msg.event_type == MUTEX_UNLOCK) {
                    handle_mutex_unlock_msg(router_id, thread_id, mutex_addr);
                    continue;  // 继续读取下一条消息
                }
            }
            
            // ===== ROUTER_BLOCK_REQUEST 特殊处理：只入队，不改 status =====
            // 原因：handle_router_block_request 会根据是否有 ready fd/pending packet 等
            // 决定是"立即唤醒"（status 保持 RUNNING）还是"真正阻塞"（status 改为 BLOCKED）。
            if (msg.event_type == ROUTER_BLOCK_REQUEST) {
                Event block_event = {
                    .timestamp = current_virtual_time,
                    .router_id = router_id,
                    .thread_id = thread_id,
                    .event_type = msg.event_type,
                    .event_id = generate_event_id(),
                    .payload = msg.payload
                };
                push_event(block_event);
                
                printf("[DESD-INTERACT] R%d T%d sent ROUTER_BLOCK_REQUEST (ReqID: %s), queued at VT=%.3f (EventID: %lu). Status NOT changed. Returning to main loop.\n",
                       router_id, thread_id, msg.request_id, current_virtual_time, block_event.event_id);
                
                // 返回主循环处理入队的事件
                return;
            }
            
            // 其他阻塞类请求（PACKET_SEND, CONNECT_REQUEST 等）
            double event_timestamp = current_virtual_time;
            if (msg.event_type == PACKET_SEND_EVENT) {
                event_timestamp = current_virtual_time + 0.002;
            }
            
            Event next_event = {
                .timestamp = event_timestamp,
                .router_id = router_id,
                .thread_id = thread_id,
                .event_type = msg.event_type,
                .event_id = generate_event_id(),
                .payload = msg.payload
            };
            push_event(next_event);
            
            // 记录线程正在等待的请求，标记为 BLOCKED
            pthread_mutex_lock(&router_states_mutex);
            ti = get_thread_info(router_id, thread_id);
            if (ti) {
                strncpy(ti->blocked_on_request_id, msg.request_id, 63);
                ti->blocked_on_request_id[63] = '\0';
                ti->status = BLOCKED;
            }
            pthread_mutex_unlock(&router_states_mutex);
            
            printf("[DESD-INTERACT] R%d T%d sent %s (ReqID: %s), marked BLOCKED, queued at VT=%.3f. Returning to main loop.\n",
                   router_id, thread_id, event_type_to_string(msg.event_type),
                   msg.request_id, event_timestamp);
            
            // 返回主循环处理入队的事件
            return;
        }  // end while(1) for reading messages
    }  // end while(1) for interact loop
}

// wait_for_detached_threads_to_rehook: 等待所有 DETACHED_WAITING 线程重新 hook 回来
// 语义：DETACHED 期间的操作在 VT 上是“瞬时”的，在线程重新 hook 之前不应该推进 VT
// 这个函数会阻塞等待，直到所有 DETACHED_WAITING 线程都发送了新消息
void wait_for_detached_threads_to_rehook() {
    if (num_detached_waiting <= 0) {
        return;  // 没有需要等待的线程
    }
    
    struct pollfd poll_fds[MAX_ROUTERS * MAX_THREADS_PER_ROUTER];
    int fd_to_router[MAX_ROUTERS * MAX_THREADS_PER_ROUTER];
    int fd_to_thread[MAX_ROUTERS * MAX_THREADS_PER_ROUTER];
    
    time_t start_time = time(NULL);
    
    while (num_detached_waiting > 0) {
        int poll_count = 0;
        
        // 收集所有 DETACHED_WAITING 线程的 socket fd
        pthread_mutex_lock(&router_states_mutex);
        for (int r = 1; r <= MAX_ROUTERS; r++) {
            for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
                ThreadInfo *ti = &router_states[r].threads[t];
                if (ti->is_active && ti->detached_waiting && ti->comm_socket_fd >= 0) {
                    poll_fds[poll_count].fd = ti->comm_socket_fd;
                    poll_fds[poll_count].events = POLLIN;
                    poll_fds[poll_count].revents = 0;
                    fd_to_router[poll_count] = r;
                    fd_to_thread[poll_count] = t;
                    printf("[DESD-DETACH-WAIT DEBUG] tracking R%d T%d fd=%d as DETACHED_WAITING (since VT=%.3f)\n",
                           r, t, ti->comm_socket_fd, ti->detached_since_vt);
                    poll_count++;
                }
            }
        }
        pthread_mutex_unlock(&router_states_mutex);
        
        if (poll_count == 0) {
            // 没有找到 DETACHED_WAITING 线程，但 num_detached_waiting > 0，可能是计数器不一致
            fprintf(stderr, "[DESD-DETACH-WAIT WARNING] num_detached_waiting=%d but no DETACHED_WAITING threads found. Resetting counter.\n",
                    num_detached_waiting);
            num_detached_waiting = 0;
            return;
        }
        
        printf("[DESD-DETACH-WAIT] Waiting for %d DETACHED_WAITING thread(s) at VT=%.3f...\n",
               poll_count, current_virtual_time);
        
        // 检查是否超时（真实时间）
        time_t elapsed = time(NULL) - start_time;
        if (elapsed >= DETACHED_WAIT_TIMEOUT_SEC) {
            fprintf(stderr, "[DESD-DETACH-WAIT ERROR] Timeout after %ld seconds waiting for DETACHED threads to re-hook at VT=%.3f.\n",
                    (long)elapsed, current_virtual_time);
            fprintf(stderr, "[DESD-DETACH-WAIT ERROR] The following threads are still DETACHED_WAITING:\n");
            pthread_mutex_lock(&router_states_mutex);
            for (int r = 1; r <= MAX_ROUTERS; r++) {
                for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
                    ThreadInfo *ti = &router_states[r].threads[t];
                    if (ti->is_active && ti->detached_waiting) {
                        fprintf(stderr, "  - R%d T%d: detached since VT=%.3f\n",
                                r, t, ti->detached_since_vt);
                    }
                }
            }
            pthread_mutex_unlock(&router_states_mutex);
            fprintf(stderr, "[DESD-DETACH-WAIT ERROR] This indicates a logic error: thread(s) detached but never re-hooked.\n");
            fprintf(stderr, "[DESD-DETACH-WAIT ERROR] Exiting simulation to prevent infinite stall.\n");
            fprintf(stderr, "[DESD-EXIT] Reason: DETACHED thread timeout (waited %d seconds). VT=%.3f, ProcessedEvents=%lu. Code=1\n",
                    DETACHED_WAIT_TIMEOUT_SEC, current_virtual_time, heartbeat_event_counter);
            exit(1);
        }
        
        // 阻塞等待（每次最多 1 秒，便于定期检查超时）
        int poll_result = poll(poll_fds, poll_count, 1000);
        
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("[DESD-DETACH-WAIT ERROR] poll failed");
            continue;
        }
        
        if (poll_result == 0) {
            // 1 秒内没有数据，继续等待
            continue;
        }
        
        printf("[DESD-DETACH-WAIT DEBUG] poll() reported %d ready fd(s) among %d tracked DETACHED_WAITING thread(s).\n",
               poll_result, poll_count);
        
        // 处理所有有数据的 fd
        for (int i = 0; i < poll_count; i++) {
            if (!(poll_fds[i].revents & POLLIN)) {
                continue;
            }
            
            int router_id = fd_to_router[i];
            int thread_id = fd_to_thread[i];
            
            pthread_mutex_lock(&router_states_mutex);
            ThreadInfo *ti = get_thread_info(router_id, thread_id);
            pthread_mutex_unlock(&router_states_mutex);
            
            if (!ti) {
                continue;
            }
            
            printf("[DESD-DETACH-WAIT DEBUG] POLLIN for R%d T%d (fd=%d), calling read_one_json_message()...\n",
                   router_id, thread_id, ti->comm_socket_fd);
            
            // 方案一：循环读取同一 fd 上所有完整 JSON 消息
            while (1) {
                Message msg;
                int read_result = read_one_json_message(ti, MSG_DONTWAIT, &msg);
                
                if (read_result < 0) {
                    // 连接断开
                    fprintf(stderr, "[DESD-DETACH-WAIT ERROR] R%d T%d disconnected while waiting for re-hook.\n",
                            router_id, thread_id);
                    pthread_mutex_lock(&router_states_mutex);
                    ti = get_thread_info(router_id, thread_id);
                    if (ti) {
                        if (ti->detached_waiting) {
                            ti->detached_waiting = 0;
                            num_detached_waiting--;
                        }
                        ti->comm_socket_fd = -1;
                        ti->status = IDLE;
                        ti->is_active = 0;
                        ti->recv_len = 0;
                    }
                    pthread_mutex_unlock(&router_states_mutex);
                    break;  // 连接断开，退出此 fd 的循环
                } else if (read_result == 0) {
                    // 没有完整消息，退出此 fd 的循环
                    break;
                }
                
                // 成功读取消息，清除 DETACHED_WAITING 状态
                pthread_mutex_lock(&router_states_mutex);
                ti = get_thread_info(router_id, thread_id);
                
                if (ti && ti->detached_waiting) {
                    ti->detached_waiting = 0;
                    num_detached_waiting--;
                    printf("[DESD-DETACH] R%d T%d re-hooked at VT=%.3f with %s (remaining detached: %d)\n",
                           router_id, thread_id, current_virtual_time, 
                           event_type_to_string(msg.event_type), num_detached_waiting);
                }
                pthread_mutex_unlock(&router_states_mutex);
                
                // 将消息统一处理为事件入队（包括 GET_VIRTUAL_TIME_EVENT）
                double event_timestamp = current_virtual_time;
                if (msg.event_type == PACKET_SEND_EVENT) {
                    event_timestamp = current_virtual_time + 0.002;
                }
                
                Event new_event = {
                    .timestamp = event_timestamp,
                    .router_id = router_id,
                    .thread_id = thread_id,
                    .event_type = msg.event_type,
                    .event_id = generate_event_id(),
                    .payload = msg.payload
                };
                
                // 如果是阻塞类请求，标记线程为 BLOCKED
                if (msg.event_type == ROUTER_BLOCK_REQUEST ||
                    msg.event_type == PACKET_SEND_EVENT ||
                    msg.event_type == CONNECT_REQUEST_EVENT) {
                    pthread_mutex_lock(&router_states_mutex);
                    ti = get_thread_info(router_id, thread_id);
                    if (ti) {
                        strncpy(ti->blocked_on_request_id, msg.request_id, 63);
                        ti->blocked_on_request_id[63] = '\0';
                        ti->status = BLOCKED;
                    }
                    pthread_mutex_unlock(&router_states_mutex);
                }
                
                push_event(new_event);
                printf("[DESD-DETACH-WAIT] R%d T%d sent %s (ReqID: %s), queued at VT=%.3f.\n",
                       router_id, thread_id, event_type_to_string(msg.event_type),
                       msg.request_id, event_timestamp);
            }  // end while(1) for this fd
        }
    }
    
    printf("[DESD-DETACH-WAIT] All DETACHED threads have re-hooked. Continuing...\n");
}

// --- DESD Event Loop (Single-threaded, New Three-Phase Architecture) ---
// 新架构保证：同一时刻只有一个路由器在 DES 控制下运行
// 阶段 0: drain - 处理所有已到达的 CANCEL 和 DETACHED 线程的重新 hook 请求
// 阶段 1: pop_event + handle_event - 取出并处理一个事件
// 阶段 2: interact - 与 active_router 交互直到它再次阻塞
// 心跳日志：每处理 HEARTBEAT_INTERVAL 个事件打印一次状态
#define HEARTBEAT_INTERVAL 500

void desd_event_loop() {
    printf("[DESD-HEARTBEAT] Event loop started. Will print heartbeat every %d events.\n", HEARTBEAT_INTERVAL);
    while (1) {
        // ===== 阶段 0: Drain 所有已到达的消息 =====
        // 处理 CANCEL_BLOCK_REQUEST 和 RUNNING_DETACHED 线程的"重新 hook"请求
        drain_all_messages_nonblocking();
        
        // ===== 关键检查：只要有线程处于 DETACHED_WAITING，就不处理任何事件 =====
        // 语义：任何线程脱离 DES 控制时，整个事件循环暂停，直到所有线程重新 hook 回来
        if (num_detached_waiting > 0) {
            printf("[DESD-DETACH-WAIT] %d thread(s) are DETACHED_WAITING at VT=%.3f, pausing event processing...\n",
                   num_detached_waiting, current_virtual_time);
            
            // 等待所有 DETACHED_WAITING 线程重新 hook 回来
            wait_for_detached_threads_to_rehook();
            
            // 线程可能发送了新事件，重新 drain 后再开始下一轮循环
            continue;
        }
        
        // ===== 阶段 1: Pop 事件并处理 =====
        printf("[DESD-DEBUG] Before pop_event: event_queue_size=%d, VT=%.3f\n",
               event_queue_size, current_virtual_time);
        Event current_event = pop_event(); // 如果队列为空，这里会阻塞
        printf("[DESD-DEBUG] After pop_event: EventID=%lu, Type=%s, Router=%d T%d, VT=%.3f\n",
               current_event.event_id,
               event_type_to_string(current_event.event_type),
               current_event.router_id,
               current_event.thread_id,
               current_event.timestamp);

        // 方案二：pop 出来后减少该线程的 pending_event_count
        // 注意：只有当事件仍然是 active 时才减（被 cancel 的事件在 cancel_event 中已减过）
        if (current_event.event_id < MAX_ACTIVE_EVENTS && 
            event_active_status[current_event.event_id] == 1) {
            ThreadInfo *pop_ti = get_thread_info(current_event.router_id, current_event.thread_id);
            if (pop_ti && pop_ti->pending_event_count > 0) {
                pop_ti->pending_event_count--;
            }
        }

        // 检查事件是否已被取消
        if (current_event.event_id < MAX_ACTIVE_EVENTS && 
            event_active_status[current_event.event_id] == 0) {
            printf("[DESD-pop-queue-skipped] Skipped cancelled or inactive event %lu (Type: %s).\n", 
                   current_event.event_id, event_type_to_string(current_event.event_type));
            continue;
        }

        if (current_event.timestamp < current_virtual_time) {
            printf("[DESD WARNING] Event %lu (Type: %s) at %.3f is in the past (Current VT: %.3f). Skipping.\n",
                   current_event.event_id, event_type_to_string(current_event.event_type),
                   current_event.timestamp, current_virtual_time);
            continue;
        }

        // 调试限制：达到 MAX_TOTAL_EVENTS 个事件后停止
        if (current_event.event_id >= MAX_TOTAL_EVENTS/6) {
            printf("[DESD-STOP] Reached %d events limit (EventID: %lu). Stopping simulation.\n",
                   MAX_TOTAL_EVENTS/6, current_event.event_id);
            printf("[DESD-EXIT] Reason: Event limit reached (%d). VT=%.3f, ProcessedEvents=%lu. Code=0 (normal)\n",
                   MAX_TOTAL_EVENTS/6, current_virtual_time, heartbeat_event_counter);
            exit(0);
        }

        // 更新虚拟时间
        current_virtual_time = current_event.timestamp;
        
        // 心跳日志：每处理 HEARTBEAT_INTERVAL 个事件打印一次
        heartbeat_event_counter++;
        if (heartbeat_event_counter % HEARTBEAT_INTERVAL == 0) {
            printf("[DESD-HEARTBEAT] Processed %lu events. Last EventID=%lu, VT=%.3f, QueueSize=%d\n",
                   heartbeat_event_counter, current_event.event_id, current_virtual_time, event_queue_size);
            fflush(stdout);
        }
        
        // GET_VIRTUAL_TIME_EVENT 不打印
        if (current_event.event_type != GET_VIRTUAL_TIME_EVENT) {
            printf("[DESD-pop-queue] Processing event %s for R%d T%d at VT=%.3f (EventID: %lu)\n",
                   event_type_to_string(current_event.event_type),
                   current_event.router_id, current_event.thread_id, 
                   current_virtual_time, current_event.event_id);
        }
        
        // 记录线程在处理事件前的状态
        ThreadInfo *thread_info = get_thread_info(current_event.router_id, current_event.thread_id);
        RouterStatus status_before_event = thread_info ? thread_info->status : IDLE;
        
        // 处理事件
        handle_event(current_event);

        // ===== 阶段 2: 与 active_router 交互直到它再次阻塞 =====
        // 判断是否需要等待该路由器的下一个事件
        RouterStatus status_after_event = thread_info ? thread_info->status : IDLE;
        
        // 路由器主动发起的事件类型（不含 ROUTER_START，后者单独用 IDLE->RUNNING 判断）
        // 注意：GET_VIRTUAL_TIME_EVENT 不在此列表中，因为：
        // 1. GETVT 被建模为阻塞型 RPC：收到时线程标记为 BLOCKED，回复后改回 RUNNING
        // 2. handle_get_virtual_time_event 处理完后线程变成 RUNNING，但这只是一次同步查询（RPC 返回），
        //    之后线程可能长时间只在应用侧执行 CPU/mutex 操作，而不再立即发起新的 DES 事件
        // 3. 如果此时进入 interact 并持续等待该线程的消息，而线程只发送 MUTEX_LOCK/UNLOCK 等瞬时事件，
        //    会导致 DESD 长时间停留在 interact 阶段，其他路由器事件得不到调度
        // 注意：ROUTER_BLOCK_REQUEST 不再包含在 is_router_initiated_event 中
        // 原因：ROUTER_BLOCK_REQUEST 可能"立即唤醒"（有 ready fd/pending packet），
        // 此时 status 保持 RUNNING，如果触发 interact，而线程不再发 RPC，会导致死锁。
        // handle_router_block_request 会在"真正阻塞"时将 status 改为 BLOCKED，
        // 之后由其他事件（如 PACKET_RECEIVE/TIMEOUT）触发 BLOCKED->RUNNING 转换来进入 interact。
        int is_router_initiated_event = (
            current_event.event_type == PACKET_SEND_EVENT ||
            current_event.event_type == CONNECT_REQUEST_EVENT
        );

        int is_getvt_event = (current_event.event_type == GET_VIRTUAL_TIME_EVENT);
        
        // CANCEL_BLOCK_REQUEST 特殊处理：
        // CANCEL 导致的 BLOCKED->RUNNING 不应触发 interact，因为：
        // 1. 线程取消阻塞后会在应用侧本地运行一段时间，不受 DES 控制
        // 2. 线程后续的 re-hook（如 LISTEN/新 BLOCK）已经在队列中排队等待处理
        // 3. 如果此时进入 interact，会卡死等待一个不会再发 DES 请求的线程
        int is_cancel_event = (current_event.event_type == CANCEL_BLOCK_REQUEST);

        // 方案 A 配合：
        // - ROUTER_START 的 SUCCESS 现在在 handle_router_start 中发送
        // - 注册线程在此之前不会发送 GETVT/阻塞请求
        // 因此，当 ROUTER_START 将线程从 IDLE 置为 RUNNING 时，可以安全地进入一次 interact，
        // 专门等待该线程“回来”发出下一条 DES 事件（如 GETVT、BLOCK 等）。
        int is_initial_start = (
            current_event.event_type == ROUTER_START &&
            status_before_event == IDLE &&
            status_after_event == RUNNING
        );

        int should_wait_for_router = !is_cancel_event && (
            (status_before_event == BLOCKED && status_after_event == RUNNING) || // 路由器解除阻塞
            is_initial_start ||                                                 // 首次 ROUTER_START: IDLE -> RUNNING
            (is_router_initiated_event && status_after_event == RUNNING)        // 处理了路由器主动发起的事件
        );

        printf("[DESD-DEBUG] Before interact: Router=%d, EventType=%s, status_before=%d, status_after=%d, should_wait=%d\n",
               current_event.router_id,
               event_type_to_string(current_event.event_type),
               status_before_event,
               status_after_event,
               should_wait_for_router);

        if (should_wait_for_router && thread_info) {
            // 重要检查：确认该 router 是否真的有 RUNNING 线程
            // 可能出现的情况：ROUTER_START 事件被处理，但线程在 drain 阶段已经因为
            // 发送 ROUTER_BLOCK_REQUEST 而被标记为 BLOCKED，这时不应该调用 interact
            pthread_mutex_lock(&router_states_mutex);
            int has_running_threads = 0;
            for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
                ThreadInfo *ti = &router_states[current_event.router_id].threads[t];
                if (ti->is_active && ti->status == RUNNING) {
                    has_running_threads = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&router_states_mutex);
            
            if (has_running_threads) {
                // 进入单线程交互阶段
                // 只与当前事件对应的线程交互，直到它再次阻塞
                printf("[DESD-DEBUG] Enter interact_with_router_until_it_blocks(router=%d, thread=%d)\n",
                       current_event.router_id, current_event.thread_id);
                interact_with_router_until_it_blocks(current_event.router_id, current_event.thread_id);
                printf("[DESD-DEBUG] Leave interact_with_router_until_it_blocks(router=%d, thread=%d)\n",
                       current_event.router_id, current_event.thread_id);
            } else {
                // 该 router 没有 RUNNING 线程（可能都已经 BLOCKED），跳过 interact
                printf("[DESD-LOOP] R%d has no RUNNING threads after processing event %s, skipping interact phase.\n",
                       current_event.router_id, event_type_to_string(current_event.event_type));
            }
        }
    }
}

void handle_event(Event event) {
    //fprintf(stderr, "[DESD DEBUG] handle_event: %s,router_id: %d\n", event_type_to_string(event.event_type), event.router_id);
    switch (event.event_type) {
        case ROUTER_START:
            handle_router_start(event);
            break;
        case LISTEN_EVENT:
            handle_listen_event(event);
            break;
        case CONNECT_REQUEST_EVENT:
            handle_connect_request_event(event);
            break;
        case CONNECTION_ESTABLISHED_EVENT:
            handle_connection_established_event(event);
            break;
        case CONNECTION_INFO_EVENT:
            handle_connection_info_event(event);
            break;
        case CLOSE_SOCKET_EVENT:
            handle_close_socket_event(event);
            break;
        case ROUTER_BLOCK_REQUEST:
            handle_router_block_request(event);
            break;
        case PACKET_SEND_EVENT:
            handle_packet_send_event(event);
            break;
        case PACKET_RECEIVE_EVENT:
            handle_packet_receive_event(event);
            break;
        case TIMEOUT_EVENT:
            handle_timeout_event(event);
            break;
        case GET_VIRTUAL_TIME_EVENT:
            handle_get_virtual_time_event(event);
            break;
        case CANCEL_BLOCK_REQUEST:
            handle_cancel_block_request(event);
            break;
        case MUTEX_RETRY_EVENT:
            handle_mutex_retry_event(event);
            break;
        default:
            fprintf(stderr, "[DESD WARNING] Unknown event type in handle_event: %d\n", event.event_type);
            break;
    }
}

// --- Event Handlers ---
void handle_router_start(Event event) {
    int router_id = event.router_id;
    int thread_id = event.thread_id;
    
    if (router_id > 0 && router_id <= MAX_ROUTERS &&
        thread_id >= 0 && thread_id < MAX_THREADS_PER_ROUTER) {
        // 获取事件对应线程的状态（可以是 T0 或 T>0）
        ThreadInfo *ti = get_thread_info(router_id, thread_id);
        if (!ti) {
            fprintf(stderr, "[DESD ERROR] handle_router_start: Thread R%d T%d not found.\n", router_id, thread_id);
            return;
        }

        // 方案 A：从 payload 中解析 request_id
        char request_id[64] = {0};
        json_error_t error;
        json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
        if (payload_obj) {
            const char *req_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
            if (req_id_ptr) {
                strncpy(request_id, req_id_ptr, 63);
                request_id[63] = '\0';
            }
            json_decref(payload_obj);
        }
        
        if (strlen(request_id) == 0) {
            fprintf(stderr, "[DESD WARNING] handle_router_start: R%d T%d payload missing request_id, cannot send SUCCESS.\n",
                    router_id, thread_id);
        }

        RouterStatus old_status = ti->status;

        // 保守策略：
        // - 只有在线程处于 IDLE 时才将其置为 RUNNING（首次启动）
        // - 如果线程已经是 BLOCKED，说明它已经向 DESD 发送过阻塞请求并在等待响应，
        //   此时不应该被 ROUTER_START 事件"解锁"，否则会打乱状态机并导致死锁。
        // - 如果线程已经是 RUNNING 或 RUNNING_DETACHED，则忽略重复的 ROUTER_START。
        if (old_status == IDLE) {
            ti->status = RUNNING;
            printf("[DESD] R%d T%d is now RUNNING (was IDLE).\n", router_id, thread_id);
        } else if (old_status == BLOCKED) {
            printf("[DESD] R%d T%d received ROUTER_START but is already BLOCKED (keeping BLOCKED).\n",
                   router_id, thread_id);
        } else if (old_status == RUNNING || old_status == RUNNING_DETACHED) {
            printf("[DESD] R%d T%d received duplicate ROUTER_START (status=%d), ignoring.\n",
                   router_id, thread_id, old_status);
        }
        
        // 方案 A：在此统一发送 SUCCESS 响应，解锁阻塞在 ensure_thread_registered 的线程
        if (strlen(request_id) > 0) {
            const char *msg = (thread_id == 0) ? "Router Started" : "Thread Registered";
            send_success_response(router_id, thread_id, request_id, "ROUTER_START", msg, NULL);
            printf("[DESD] R%d T%d SUCCESS response sent for ROUTER_START (request_id=%s).\n",
                   router_id, thread_id, request_id);
        }
    }
}

void handle_listen_event(Event event) {
    int router_id = event.router_id;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_listen_event: Failed to parse payload JSON.\n");
        return;
    }

    const char *req_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    if (req_id_ptr == NULL) {
        fprintf(stderr, "[DESD ERROR] handle_listen_event: 'request_id' missing or not a string.\n");
        json_decref(payload_obj);
        return;
    }
    char request_id[64];
    strncpy(request_id, req_id_ptr, 63);
    request_id[63] = '\0';

    const char *listen_addr_ptr = json_string_value(json_object_get(payload_obj, "listen_address"));
    if (listen_addr_ptr == NULL) {
        fprintf(stderr, "[DESD ERROR] handle_listen_event: 'listen_address' missing or not a string.\n");
        json_decref(payload_obj);
        return;
    }
    
    // 复制地址到本地缓冲区，防止 json_decref 后访问无效内存
    char listen_address_local[256];
    strncpy(listen_address_local, listen_addr_ptr, sizeof(listen_address_local) - 1);
    listen_address_local[sizeof(listen_address_local) - 1] = '\0';
    
    // 获取socket_fd
    int socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));
    
    json_decref(payload_obj);

    if (router_id > 0 && router_id <= MAX_ROUTERS) {
        // 检查是否已达到监听地址上限
        if (router_states[router_id].listen_count >= 10) {
            fprintf(stderr, "[DESD ERROR] R%d listen address limit reached (max: 10)!\n", router_id);
            send_error_response(router_id, event.thread_id, request_id, "Too many listen addresses");
            // 即使出错也要把线程改回 RUNNING
            pthread_mutex_lock(&router_states_mutex);
            ThreadInfo *ti_err = get_thread_info(router_id, event.thread_id);
            if (ti_err && ti_err->status == BLOCKED) {
                ti_err->status = RUNNING;
                ti_err->blocked_on_request_id[0] = '\0';
            }
            pthread_mutex_unlock(&router_states_mutex);
            return;
        }
        
        // 检查是否已存在该地址（避免重复）
        for (int i = 0; i < router_states[router_id].listen_count; i++) {
            if (strcmp(router_states[router_id].listen_addresses[i], listen_address_local) == 0) {
                printf("[DESD] R%d already listening on %s, ignoring duplicate.\n", 
                       router_id, listen_address_local);
                send_success_response(router_id, event.thread_id, request_id, "LISTEN", "Already Listening", NULL);
                // 重复 listen 也要把线程改回 RUNNING
                pthread_mutex_lock(&router_states_mutex);
                ThreadInfo *ti_dup = get_thread_info(router_id, event.thread_id);
                if (ti_dup && ti_dup->status == BLOCKED) {
                    ti_dup->status = RUNNING;
                    ti_dup->blocked_on_request_id[0] = '\0';
                }
                pthread_mutex_unlock(&router_states_mutex);
                return;
            }
        }
        
        // 添加新的监听地址和socket_fd
        int index = router_states[router_id].listen_count;
        strncpy(router_states[router_id].listen_addresses[index], listen_address_local, 255);
        router_states[router_id].listen_addresses[index][255] = '\0';
        router_states[router_id].listening_socket_fds[index] = socket_fd;
        router_states[router_id].listen_count++;
        
        // LISTEN事件信息已在下面显示，移除DEBUG输出
        printf("[DESD-debug-listen] R%d is now listening on %s (fd:%d, total: %d address%s).\n", 
               router_id, listen_address_local, socket_fd,
               router_states[router_id].listen_count,
               router_states[router_id].listen_count > 1 ? "es" : "");
        
        // 发送响应
        send_success_response(router_id, event.thread_id, request_id, "LISTEN", "Listen Successful", NULL);
        
        // 回复发送后，将线程状态从 BLOCKED 改回 RUNNING
        pthread_mutex_lock(&router_states_mutex);
        ThreadInfo *ti = get_thread_info(router_id, event.thread_id);
        if (ti && ti->status == BLOCKED) {
            if (strncmp(ti->blocked_on_request_id, request_id, 63) == 0) {
                ti->status = RUNNING;
                ti->blocked_on_request_id[0] = '\0';
            }
        }
        pthread_mutex_unlock(&router_states_mutex);
    }
}

void handle_connect_request_event(Event event) {
    int router_id = event.router_id;
    char request_id[64];
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_connect_request_event: Failed to parse payload JSON.\n");
        return;
    }

    const char *req_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    if (req_id_ptr == NULL) {
        fprintf(stderr, "[DESD ERROR] handle_connect_request_event: 'request_id' missing or not a string.\n");
        json_decref(payload_obj); // 释放JSON对象
        return;
    }
    strncpy(request_id, req_id_ptr, 63);
    request_id[63] = '\0';

    const char *dest_addr_ptr = json_string_value(json_object_get(payload_obj, "destination_abstract_address"));
    if (dest_addr_ptr == NULL) {
        fprintf(stderr, "[DESD ERROR] handle_connect_request_event: 'destination_abstract_address' missing or not a string.\n");
        json_decref(payload_obj);
        return;
    }
    char destination_abstract_address_local[MAX_MSG_SIZE]; // Local buffer
    strncpy(destination_abstract_address_local, dest_addr_ptr, MAX_MSG_SIZE - 1);
    destination_abstract_address_local[MAX_MSG_SIZE - 1] = '\0';
    
    // 解析 socket_fd
    int client_socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));

    json_decref(payload_obj); // Now it's safe to decref

    int thread_id = event.thread_id;
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    
    if (ti) {
        printf("[DEBUG-CONN-REQ] R%d T%d CONNECT_REQUEST to %s (client_fd=%d), ThreadInfo before connect: status=%d blocked_on_function='%s' blocked_on_request_id='%s'\n",
               router_id, thread_id, destination_abstract_address_local, client_socket_fd,
               ti->status, ti->blocked_on_function, ti->blocked_on_request_id);
    } else {
        printf("[DEBUG-CONN-REQ] R%d T%d CONNECT_REQUEST to %s (client_fd=%d), but no ThreadInfo found\n",
               router_id, thread_id, destination_abstract_address_local, client_socket_fd);
    }
    fflush(stdout);
    
    if (router_id > 0 && router_id <= MAX_ROUTERS && ti) {
        ti->status = BLOCKED;
        strncpy(ti->blocked_on_request_id, request_id, 63);
        ti->blocked_on_request_id[63] = '\0';
        strncpy(ti->blocked_on_function, "CONNECT_CALL", 63);
        ti->blocked_on_function[63] = '\0';

        double connection_delay = 0.05; // 50ms 连接建立延迟
        double connection_established_time = current_virtual_time + connection_delay;

        // 使用监听地址查找目标路由器
        // 简化：CONNECT信息在后面会显示
        
        int target_router_id = find_router_by_listen_address(destination_abstract_address_local, router_id);

        if (target_router_id == -1) {
            // 目标路由器没有 listen，连接失败
            fprintf(stderr, "[DESD ERROR] R%d attempted to connect to %s, but no router is listening on that address.\n",
                    router_id, destination_abstract_address_local);
            // 错误时显示可用地址
            printf("[DESD] Available listen addresses:\n");
            for (int r = 1; r <= MAX_ROUTERS; r++) {
                if (router_states[r].listen_count > 0) {
                    printf("[DESD]   R%d listening on:", r);
                    for (int j = 0; j < router_states[r].listen_count; j++) {
                        printf(" %s", router_states[r].listen_addresses[j]);
                    }
                    printf("\n");
                }
            }
            send_error_response(router_id, thread_id, request_id, "Connection refused: Target not listening");
            ti->status = RUNNING; // 错误发生，解除阻塞
            memset(ti->blocked_on_request_id, 0, 64);
            memset(ti->blocked_on_function, 0, 64);
            return;
        }
        
        // 额外的自连接检测（理论上不应该发生，因为find函数已经排除了）
        if (target_router_id == router_id) {
            fprintf(stderr, "[DESD ERROR] Self-connection detected! R%d trying to connect to itself at %s\n",
                    router_id, destination_abstract_address_local);
            send_error_response(router_id, thread_id, request_id, "Connection refused: Cannot connect to self");
            ti->status = RUNNING;
            memset(ti->blocked_on_request_id, 0, 64);
            memset(ti->blocked_on_function, 0, 64);
            return;
        }
        
        // ========== Collision Policy: 只允许较小 ID 向较大 ID 主动连接 ==========
        // 目的：消灭 simultaneous open，避免 BIRD 自身的 collision 逻辑与 DES 控制逻辑互相叠加
        // 规则：如果 client_router_id > server_router_id，则抑制此次 connect，让对端作为 active 侧
        if (router_id > target_router_id) {
            printf("[DESD-COLLISION-POLICY] Suppressing CONNECT_REQUEST from R%d to R%d "
                   "(policy: only smaller ID initiates, R%d will act as active side)\n",
                   router_id, target_router_id, target_router_id);
            send_error_response(router_id, thread_id, request_id, 
                                "Connect suppressed by collision policy");
            ti->status = RUNNING;  // 解除阻塞，让 BIRD 正常处理连接失败
            memset(ti->blocked_on_request_id, 0, 64);
            memset(ti->blocked_on_function, 0, 64);
            return;
        }
        
        // 简化：只显示连接结果，不显示匹配过程
        
        // 生成唯一连接ID
        unsigned long connection_id = generate_connection_id();
        
        printf("[DESD] R%d sent CONNECT_REQUEST to %s (target R%d), conn_id=%lu, scheduling CONNECTION_ESTABLISHED at VT=%.3f.\n",
               router_id, destination_abstract_address_local, target_router_id, connection_id, connection_established_time);

        // 同时为发起连接的客户端路由器调度一个CONNECTION_ESTABLISHED_EVENT
        // 重要：客户端事件略早于服务器事件，确保 real_connect() 先于 real_accept() 执行
        json_t *source_payload_obj = json_object();
        json_object_set_new(source_payload_obj, "client_router_id", json_integer(router_id));
        json_object_set_new(source_payload_obj, "server_router_id", json_integer(target_router_id));
        json_object_set_new(source_payload_obj, "client_socket_fd", json_integer(client_socket_fd));
        json_object_set_new(source_payload_obj, "connection_id", json_integer(connection_id));
        json_object_set_new(source_payload_obj, "request_id", json_string(request_id));
        char *source_payload_str = json_dumps(source_payload_obj, JSON_COMPACT);
        json_decref(source_payload_obj);

        Event source_conn_est_event = {
            .timestamp = connection_established_time - 0.001, // 客户端事件早 1ms，确保先执行 connect
            .router_id = router_id,
            .thread_id = event.thread_id,
            .event_type = CONNECTION_ESTABLISHED_EVENT,
            .event_id = generate_event_id()
        };
        strncpy(source_conn_est_event.payload.json_str, source_payload_str, MAX_MSG_SIZE - 1);
        source_conn_est_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
        free(source_payload_str);
        push_event(source_conn_est_event);
        
        // 为目标路由器（服务器）调度 CONNECTION_ESTABLISHED_EVENT
        json_t *target_payload_obj = json_object();
        json_object_set_new(target_payload_obj, "client_router_id", json_integer(router_id));
        json_object_set_new(target_payload_obj, "server_router_id", json_integer(target_router_id));
        json_object_set_new(target_payload_obj, "client_socket_fd", json_integer(client_socket_fd));
        json_object_set_new(target_payload_obj, "connection_id", json_integer(connection_id));
        json_object_set_new(target_payload_obj, "request_id", json_string(request_id));
        char *target_payload_str = json_dumps(target_payload_obj, JSON_COMPACT);
        json_decref(target_payload_obj);

        Event conn_est_event = {
            .timestamp = connection_established_time,
            .router_id = target_router_id,
            .event_type = CONNECTION_ESTABLISHED_EVENT,
            .event_id = generate_event_id()
        };
        strncpy(conn_est_event.payload.json_str, target_payload_str, MAX_MSG_SIZE - 1);
        conn_est_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
        free(target_payload_str);
        push_event(conn_est_event);
    }
}

void handle_connection_established_event(Event event) {
    int router_id = event.router_id;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_connection_established_event: Failed to parse payload JSON.\n");
        return;
    }

    const char *req_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    if (req_id_ptr == NULL) {
        fprintf(stderr, "[DESD ERROR] handle_connection_established_event: 'request_id' missing or not a string.\n");
        json_decref(payload_obj);
        return;
    }
    char request_id_local[64]; // Local buffer
    strncpy(request_id_local, req_id_ptr, 63);
    request_id_local[63] = '\0';

    int client_router_id = json_integer_value(json_object_get(payload_obj, "client_router_id"));
    int server_router_id = json_integer_value(json_object_get(payload_obj, "server_router_id"));
    int client_socket_fd = json_integer_value(json_object_get(payload_obj, "client_socket_fd"));
    unsigned long connection_id = (unsigned long)json_integer_value(json_object_get(payload_obj, "connection_id"));

    json_decref(payload_obj);

    // 判断这个事件是发给连接发起方（CONNECT）还是接收方（ACCEPT）
    int is_connector = (router_id == client_router_id);
    
    // 获取线程信息（使用事件中的 thread_id）
    int thread_id = event.thread_id;
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    
    if (ti) {
        printf("[DEBUG-CONN-EST] R%d T%d has ThreadInfo for CONNECTION_ESTABLISHED: is_connector=%d client=%d server=%d client_fd=%d conn_id=%lu status=%d blocked_on_function='%s' blocked_on_request_id='%s'\n",
               router_id, thread_id, is_connector, client_router_id, server_router_id, client_socket_fd, connection_id,
               ti->status, ti->blocked_on_function, ti->blocked_on_request_id);
    } else {
        printf("[DEBUG-CONN-EST] R%d T%d has NO ThreadInfo for CONNECTION_ESTABLISHED: is_connector=%d client=%d server=%d client_fd=%d conn_id=%lu\n",
               router_id, thread_id, is_connector, client_router_id, server_router_id, client_socket_fd, connection_id);
    }
    fflush(stdout);
    
    if (router_id > 0 && router_id <= MAX_ROUTERS && ti) {
        // 对于 CONNECT 方（发起连接的路由器）
        if (is_connector) {
            if (ti->status == BLOCKED &&
                strcmp(ti->blocked_on_function, "CONNECT_CALL") == 0) {
                printf("[DEBUG-CONN-EST] R%d (client) will register connection: client_fd=%d server=%d conn_id=%lu\n",
                       router_id, client_socket_fd, server_router_id, connection_id);
                dump_router_connections(client_router_id, "CONN_EST_CLIENT_BEFORE");
                
                // 保存路由器自己的 request_id
                char router_request_id[64];
                strncpy(router_request_id, ti->blocked_on_request_id, 63);
                router_request_id[63] = '\0';
                
                // 注册客户端的连接映射，带上connection_id（暂时用 -1 作为对端 socket_fd 的占位符）
                // 完整的双向映射将在服务器 accept() 并发送 CONNECTION_INFO_EVENT 后建立
                // 需要找到该连接并设置connection_id
                register_connection(client_router_id, client_socket_fd, server_router_id, -1);
                // 设置connection_id
                for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
                    if (router_states[client_router_id].connections[i].is_active &&
                        router_states[client_router_id].connections[i].socket_fd == client_socket_fd) {
                        router_states[client_router_id].connections[i].connection_id = connection_id;
                        break;
                    }
                }
                
                ti->status = RUNNING;
                memset(ti->blocked_on_request_id, 0, 64);
                memset(ti->blocked_on_function, 0, 64);

                dump_router_connections(client_router_id, "CONN_EST_CLIENT_AFTER");

                printf("[DESD] R%d (client) connect() completed with R%d.\n", router_id, server_router_id);
                send_success_response(router_id, ti->thread_id, router_request_id, "CONNECT", "Connection Established", NULL);
            } else {
                printf("[DESD WARNING] R%d received CONNECTION_ESTABLISHED_EVENT but not blocked on connect (currently %s).\n", 
                       router_id, 
                       ti->status == BLOCKED ? ti->blocked_on_function : "not blocked");
                printf("[DEBUG-CONN-EST] R%d T%d unexpected CONNECT_ESTABLISHED state: client=%d server=%d client_fd=%d conn_id=%lu status=%d blocked_on_function='%s' blocked_on_request_id='%s'\n",
                       router_id, thread_id, client_router_id, server_router_id, client_socket_fd, connection_id,
                       ti->status, ti->blocked_on_function, ti->blocked_on_request_id);
                fflush(stdout);
            }
        }
        // 对于 ACCEPT 方（接收连接的路由器）
        else {
            // 立即为 server 端注册虚拟连接（模拟内核的 child socket 行为）
            // 使用负数作为虚拟 fd，格式为 -(connection_id + 1000)
            // 🔥 关键修复：使用偏移量避免与 -1（"未设置"标志）冲突
            int virtual_server_fd = -(int)(connection_id + 1000);
            printf("[DEBUG-CONN-EST] R%d (server) registering virtual connection: virtual_fd=%d client=%d client_fd=%d conn_id=%lu\n",
                   server_router_id, virtual_server_fd, client_router_id, client_socket_fd, connection_id);
            dump_router_connections(server_router_id, "CONN_EST_SERVER_BEFORE");
            register_connection(server_router_id, virtual_server_fd, client_router_id, client_socket_fd);
            
            // 设置 connection_id
            for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
                if (router_states[server_router_id].connections[i].is_active &&
                    router_states[server_router_id].connections[i].socket_fd == virtual_server_fd) {
                    router_states[server_router_id].connections[i].connection_id = connection_id;
                    break;
                }
            }
            
            // 同时更新 client 端的 peer_socket_fd（指向虚拟 fd）
            for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
                if (router_states[client_router_id].connections[i].is_active &&
                    router_states[client_router_id].connections[i].socket_fd == client_socket_fd &&
                    router_states[client_router_id].connections[i].connection_id == connection_id) {
                    router_states[client_router_id].connections[i].peer_socket_fd = virtual_server_fd;
                    break;
                }
            }
            dump_router_connections(server_router_id, "CONN_EST_SERVER_AFTER");
            dump_router_connections(client_router_id, "CONN_EST_CLIENT_PEER_AFTER");
            
            printf("[DESD] R%d (server) virtual connection registered: virtual_fd=%d <-> R%d (client) fd=%d (conn_id=%lu, pending accept).\n",
                   server_router_id, virtual_server_fd, client_router_id, client_socket_fd, connection_id);
            
            // 对于服务器端，查找所有线程看哪个在等待 ACCEPT
            ThreadInfo *server_ti = NULL;
            for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
                ThreadInfo *check_ti = get_thread_info(router_id, t);
                if (check_ti && check_ti->is_active && check_ti->status == BLOCKED &&
                    strcmp(check_ti->blocked_on_function, "ACCEPT_CALL") == 0) {
                    server_ti = check_ti;
                    break;
                }
            }
            
            if (server_ti) {
                // 保存路由器自己的 request_id
                char router_request_id[64];
                strncpy(router_request_id, server_ti->blocked_on_request_id, 63);
                router_request_id[63] = '\0';
                
                server_ti->status = RUNNING;
                memset(server_ti->blocked_on_request_id, 0, 64);
                memset(server_ti->blocked_on_function, 0, 64);

                printf("[DESD] R%d (server) accept() completed due to connection from R%d.\n", 
                       router_id, client_router_id);
                send_success_response(router_id, server_ti->thread_id, router_request_id, "ACCEPT", "Connection Established", NULL);
            } else {
                // 连接已经建立，但路由器还没有调用 accept()，加入FIFO队列
                if (router_states[router_id].pending_connections_count >= MAX_PENDING_PACKETS) {
                    fprintf(stderr, "[DESD ERROR] R%d pending connection queue full! Dropping oldest connection.\n", router_id);
                    int head = router_states[router_id].pending_conn_head;
                    router_states[router_id].pending_conn_head = (head + 1) % MAX_PENDING_PACKETS;
                    router_states[router_id].pending_connections_count--;
                }
                
                int tail = router_states[router_id].pending_conn_tail;
                router_states[router_id].pending_connections[tail].client_router_id = client_router_id;
                router_states[router_id].pending_connections[tail].client_socket_fd = client_socket_fd;
                router_states[router_id].pending_connections[tail].connection_id = connection_id;
                router_states[router_id].pending_conn_tail = (tail + 1) % MAX_PENDING_PACKETS;
                router_states[router_id].pending_connections_count++;
                
                // 查找是否有线程在 SELECT_CALL 上阻塞
                ThreadInfo *select_ti = NULL;
                for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
                    ThreadInfo *check_ti = get_thread_info(router_id, t);
                    if (check_ti && check_ti->is_active && check_ti->status == BLOCKED &&
                        strcmp(check_ti->blocked_on_function, "SELECT_CALL") == 0) {
                        select_ti = check_ti;
                        break;
                    }
                }
                
                printf("[DEBUG-CONN] R%d: connection arrived from R%d(fd:%d), pending_count=%d\n",
                       router_id, client_router_id, client_socket_fd, router_states[router_id].pending_connections_count);
                
                if (select_ti) {
                    char request_id[64];
                    strncpy(request_id, select_ti->blocked_on_request_id, 63);
                    request_id[63] = '\0';
                    
                    if (select_ti->pending_timeout_event_id > 0) {
                        cancel_event(select_ti->pending_timeout_event_id);
                        printf("[DESD] Canceled timeout event %lu for R%d (connection arrived before timeout).\n",
                               select_ti->pending_timeout_event_id, router_id);
                        select_ti->pending_timeout_event_id = 0;
                    }
                    
                    select_ti->status = RUNNING;
                    memset(select_ti->blocked_on_request_id, 0, 64);
                    memset(select_ti->blocked_on_function, 0, 64);
                    
                    // 构建就绪FD列表，包含所有listening socket（它们都有pending connection）
                    json_t *ready_fds_array = json_array();
                    for (int i = 0; i < router_states[router_id].listen_count; i++) {
                        int listen_fd = router_states[router_id].listening_socket_fds[i];
                        if (listen_fd >= 0) {
                            json_t *ready_fd_info = json_object();
                            json_object_set_new(ready_fd_info, "fd", json_integer(listen_fd));
                            json_object_set_new(ready_fd_info, "revents", json_integer(0x001));  // POLLIN - 新连接待accept
                            json_array_append_new(ready_fds_array, ready_fd_info);
                        }
                    }
                    
                    // 构建响应
                    size_t ready_fds_count = json_array_size(ready_fds_array);
                    json_t *response_payload = json_object();
                    json_object_set_new(response_payload, "status", json_string("SUCCESS"));
                    json_object_set_new(response_payload, "blocked_function", json_string("SELECT"));
                    json_object_set_new(response_payload, "message", json_string("Connection Available"));
                    json_object_set_new(response_payload, "ready_fds", ready_fds_array);
                    
                    char *response_payload_str = json_dumps(response_payload, JSON_COMPACT);
                    json_decref(response_payload);
                    
                    Message response = {
                        .message_type = DESD_TO_HOOK,
                        .router_id = router_id,
                        .thread_id = select_ti->thread_id,  // 修复：使用被唤醒线程的 thread_id，而非事件的 thread_id
                        .virtual_time = current_virtual_time
                    };
                    strncpy(response.request_id, request_id, 63);
                    response.request_id[63] = '\0';
                    strncpy(response.payload.json_str, response_payload_str, MAX_MSG_SIZE - 1);
                    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                    free(response_payload_str);
                    send_message_to_router(router_id, &response);
                    
                    printf("[DESD] R%d T%d was blocked on select and now awakened by CONNECTION_ESTABLISHED_EVENT (listening socket now has POLLIN, %zu ready FD(s)).\n",
                           router_id, select_ti->thread_id, ready_fds_count);
                    printf("[DEBUG-WAKEUP] R%d awakened from SELECT, pending_connections=%d, ready_fds_count=%zu\n",
                           router_id, router_states[router_id].pending_connections_count, ready_fds_count);
                } else {
                    printf("[DESD] R%d has %d pending connection(s) (connection established but accept not called yet).\n", 
                           router_id, router_states[router_id].pending_connections_count);
                    printf("[DEBUG-NO-WAKEUP] R%d NOT on SELECT when connection arrived (pending=%d)\n",
                           router_id, router_states[router_id].pending_connections_count);
                }
            }
        }
    }
}

void handle_connection_info_event(Event event) {
    int router_id = event.router_id;
    printf("[DEBUG-CONN-INFO] Received CONNECTION_INFO_EVENT from R%d\n", router_id);
    fflush(stdout);
    
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_connection_info_event: Failed to parse payload JSON: %s\n", error.text);
        fflush(stderr);
        return;
    }

    const char *req_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    char request_id[64] = {0};
    if (req_id_ptr) {
        strncpy(request_id, req_id_ptr, 63);
    }
    
    int socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));
    unsigned long connection_id = (unsigned long)json_integer_value(json_object_get(payload_obj, "connection_id"));
    json_t *is_client_json = json_object_get(payload_obj, "is_client");
    int is_client = is_client_json && json_is_true(is_client_json);
    
    // 检查是否是 accept 失败通知
    json_t *accept_failed_json = json_object_get(payload_obj, "accept_failed");
    int accept_failed = accept_failed_json && json_is_true(accept_failed_json);
    
    printf("[DEBUG-CONN-INFO] R%d sent CONNECTION_INFO: socket_fd=%d, conn_id=%lu, is_client=%d, accept_failed=%d\n",
           router_id, socket_fd, connection_id, is_client, accept_failed);
    fflush(stdout);
    
    // 处理 accept 失败的情况：清理虚拟连接
    if (accept_failed && socket_fd == -1) {
        printf("[DESD] R%d accept FAILED for conn_id=%lu. Cleaning up virtual connection.\n",
               router_id, connection_id);
        
        int virtual_server_fd = -(int)(connection_id + 1000);
        int cleaned = 0;
        
        // 清理 server 端的虚拟连接
        for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
            if (router_states[router_id].connections[i].is_active &&
                router_states[router_id].connections[i].socket_fd == virtual_server_fd &&
                router_states[router_id].connections[i].connection_id == connection_id) {
                
                int client_router_id = router_states[router_id].connections[i].peer_router_id;
                int client_socket_fd = router_states[router_id].connections[i].peer_socket_fd;
                
                printf("[DESD] Removing virtual connection: R%d (virtual_fd:%d) <-> R%d (fd:%d)\n",
                       router_id, virtual_server_fd, client_router_id, client_socket_fd);
                
                // 删除 server 侧的虚拟连接
                router_states[router_id].connections[i].is_active = 0;
                router_states[router_id].connections[i].socket_fd = -1;
                router_states[router_id].connections[i].peer_router_id = -1;
                router_states[router_id].connections[i].peer_socket_fd = -1;
                router_states[router_id].connections[i].connection_id = 0;
                cleaned++;
                
                // 同时清理 client 端的 peer_socket_fd（从虚拟 fd 改回 -1）
                if (client_router_id > 0 && client_router_id <= MAX_ROUTERS && client_socket_fd != -1) {
                    for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                        if (router_states[client_router_id].connections[j].is_active &&
                            router_states[client_router_id].connections[j].socket_fd == client_socket_fd &&
                            router_states[client_router_id].connections[j].connection_id == connection_id) {
                            
                            printf("[DESD] Clearing client peer_socket_fd: R%d (fd:%d) peer_fd: %d -> -1\n",
                                   client_router_id, client_socket_fd, virtual_server_fd);
                            router_states[client_router_id].connections[j].peer_socket_fd = -1;
                            break;
                        }
                    }
                }
                break;
            }
        }
        
        if (cleaned > 0) {
            printf("[DESD] R%d cleaned %d virtual connection(s) for failed accept (conn_id=%lu).\n",
                   router_id, cleaned, connection_id);
        } else {
            printf("[DESD WARNING] R%d could not find virtual connection to clean for conn_id=%lu\n",
                   router_id, connection_id);
        }
        
        json_decref(payload_obj);
        send_success_response(router_id, event.thread_id, request_id, "CONNECTION_INFO", "Accept failure processed", NULL);
        // accept failure 处理后也要把线程改回 RUNNING
        pthread_mutex_lock(&router_states_mutex);
        ThreadInfo *ti_fail = get_thread_info(router_id, event.thread_id);
        if (ti_fail && ti_fail->status == BLOCKED) {
            ti_fail->status = RUNNING;
            ti_fail->blocked_on_request_id[0] = '\0';
        }
        pthread_mutex_unlock(&router_states_mutex);
        return;
    }
    
    if (is_client) {
        // 客户端发送的 CONNECTION_INFO_EVENT
        // 查找客户端是否已经有一个到服务端的连接（在 CONNECTION_ESTABLISHED_EVENT 中创建的）
        int updated = 0;
        int peer_router_id = -1;
        
        printf("[DEBUG-CONN-INFO] R%d (client) sending CONNECTION_INFO for fd=%d\n", router_id, socket_fd);
        
        // 首先查找客户端自己的连接表，看是否已经有连接记录
        int existing_entry_idx = -1;
        for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
            if (router_states[router_id].connections[i].is_active &&
                router_states[router_id].connections[i].socket_fd == socket_fd) {
                existing_entry_idx = i;
                // 找到了这个连接，现在需要找到对端的 socket_fd
                peer_router_id = router_states[router_id].connections[i].peer_router_id;
                printf("[DEBUG-CONN-INFO] R%d found existing connection entry at index %d: peer_router_id=%d, peer_socket_fd=%d\n",
                       router_id, i, peer_router_id, router_states[router_id].connections[i].peer_socket_fd);
                
                if (peer_router_id != -1) {
                    // 在服务端的连接表中查找对应的连接
                    printf("[DEBUG-CONN-INFO] R%d searching in R%d connection table for matching server connection\n",
                           router_id, peer_router_id);
                    for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                        if (router_states[peer_router_id].connections[j].is_active &&
                            router_states[peer_router_id].connections[j].peer_router_id == router_id &&
                            router_states[peer_router_id].connections[j].peer_socket_fd == -1) {
                            // 找到了服务端的连接，它的 peer_socket_fd 还是 -1
                            // 更新双向映射
                            int peer_socket_fd = router_states[peer_router_id].connections[j].socket_fd;
                            printf("[DEBUG-CONN-INFO] R%d MATCHED! Updating bidirectional mapping with R%d fd=%d\n",
                                   router_id, peer_router_id, peer_socket_fd);
                            router_states[router_id].connections[i].peer_socket_fd = peer_socket_fd;
                            router_states[peer_router_id].connections[j].peer_socket_fd = socket_fd;
                            
                            printf("[DESD] R%d (client) updated connection: fd %d <-> R%d (server) fd %d.\n", 
                                   router_id, socket_fd, peer_router_id, peer_socket_fd);
                            updated = 1;
                            break;
                        }
                    }
                }
                break;
            }
        }
        
        if (!updated) {
            if (existing_entry_idx != -1) {
                printf("[DEBUG-CONN-INFO] R%d (client) connection entry exists at index %d but no server match yet (peer_router_id=%d)\n",
                       router_id, existing_entry_idx, peer_router_id);
                // 调试：dump 当前客户端及已知对端的连接表，便于分析未匹配原因
                dump_router_connections(router_id, "CONN_INFO_CLIENT_BEFORE");
                if (peer_router_id > 0 && peer_router_id <= MAX_ROUTERS) {
                    dump_router_connections(peer_router_id, "CONN_INFO_CLIENT_PEER");
                }
            } else {
                // 如果没有找到已有的连接，这通常不应该发生，因为 established 事件应该先到
                printf("[DEBUG-CONN-INFO] R%d (client) NO existing connection entry found for fd=%d. Registering new (incomplete) entry.\n",
                       router_id, socket_fd);
                register_connection(router_id, socket_fd, -1, -1);
                // 这里可能需要设置 connection_id，但如果 is_client，通常不需要在这一步设置
            }
        }
    } else {
        // 方案 P：服务端发送的 CONNECTION_INFO_EVENT，使用 real_peer_router_id 进行后验匹配
        // 不再依赖 connection_id 查找虚拟连接，而是根据真实 peer 匹配
        int real_peer_router_id = json_integer_value(json_object_get(payload_obj, "real_peer_router_id"));
        int client_router_id = -1;
        int peer_socket_fd = -1;
        int found_virtual_connection = 0;
        unsigned long matched_connection_id = 0;
        int matched_virtual_fd = 0;
        
        printf("[DEBUG-CONN-INFO] R%d (server) Plan P: CONNECTION_INFO for fd=%d, real_peer_router_id=%d\n", 
               router_id, socket_fd, real_peer_router_id);
        // 调试：在查找/替换虚拟连接前先 dump 当前 server 侧连接表
        dump_router_connections(router_id, "CONN_INFO_SERVER_BEFORE");
        
        // 方案 P：在 server 端查找虚拟连接，匹配条件：
        // - is_active == 1
        // - socket_fd < 0 (虚拟 fd)
        // - peer_router_id == real_peer_router_id
        for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
            if (router_states[router_id].connections[i].is_active &&
                router_states[router_id].connections[i].socket_fd < 0 &&
                router_states[router_id].connections[i].peer_router_id == real_peer_router_id) {
                
                // 找到了虚拟连接！
                matched_connection_id = router_states[router_id].connections[i].connection_id;
                matched_virtual_fd = router_states[router_id].connections[i].socket_fd;
                client_router_id = router_states[router_id].connections[i].peer_router_id;
                peer_socket_fd = router_states[router_id].connections[i].peer_socket_fd;
                
                printf("[DESD] R%d (server) Plan P matched virtual connection: virtual_fd=%d -> replacing with real_fd=%d (conn_id=%lu, real_peer=R%d)\n",
                       router_id, matched_virtual_fd, socket_fd, matched_connection_id, real_peer_router_id);
                
                // 替换虚拟 fd 为真实 fd
                router_states[router_id].connections[i].socket_fd = socket_fd;
                found_virtual_connection = 1;
                
                // 同时更新 client 端的 peer_socket_fd（从虚拟 fd 改为真实 fd）
                if (client_router_id > 0 && client_router_id <= MAX_ROUTERS && peer_socket_fd != -1) {
                    for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                        if (router_states[client_router_id].connections[j].is_active &&
                            router_states[client_router_id].connections[j].socket_fd == peer_socket_fd &&
                            router_states[client_router_id].connections[j].connection_id == matched_connection_id) {
                            
                            router_states[client_router_id].connections[j].peer_socket_fd = socket_fd;
                            printf("[DESD] R%d (client) peer_socket_fd updated: %d -> %d\n",
                                   client_router_id, matched_virtual_fd, socket_fd);
                            // 调试：成功更新后 dump server/client 双侧映射
                            dump_router_connections(router_id, "CONN_INFO_SERVER_AFTER");
                            dump_router_connections(client_router_id, "CONN_INFO_CLIENT_AFTER");
                            break;
                        }
                    }
                }
                
                // 关键修复：更新所有已缓冲的数据包的 socket_fd（从虚拟 fd 改为真实 fd）
                int updated_packets = 0;
                for (int j = 0; j < MAX_PENDING_PACKETS; j++) {
                    if (router_states[router_id].packet_buffers[j].is_used &&
                        router_states[router_id].packet_buffers[j].socket_fd == matched_virtual_fd) {
                        router_states[router_id].packet_buffers[j].socket_fd = socket_fd;
                        updated_packets++;
                    }
                }
                if (updated_packets > 0) {
                    printf("[DESD] R%d (server) updated %d buffered packet(s): socket_fd %d -> %d\n",
                           router_id, updated_packets, matched_virtual_fd, socket_fd);
                }
                
                printf("[DESD] R%d (server) connection updated: real_fd=%d <-> R%d (client) fd=%d (conn_id=%lu).\n",
                       router_id, socket_fd, client_router_id, peer_socket_fd, matched_connection_id);
                
                // 🔑 方案 P 新增：从 pending_connections 队列中删除该连接（按 connection_id 查找，非 FIFO）
                int removed_from_pending = 0;
                for (int p = 0; p < MAX_PENDING_PACKETS; p++) {
                    int idx = (router_states[router_id].pending_conn_head + p) % MAX_PENDING_PACKETS;
                    if (p >= router_states[router_id].pending_connections_count) break;
                    if (router_states[router_id].pending_connections[idx].connection_id == matched_connection_id) {
                        // 找到了，需要从队列中移除（通过将后面的元素前移）
                        for (int k = p; k < router_states[router_id].pending_connections_count - 1; k++) {
                            int curr_idx = (router_states[router_id].pending_conn_head + k) % MAX_PENDING_PACKETS;
                            int next_idx = (router_states[router_id].pending_conn_head + k + 1) % MAX_PENDING_PACKETS;
                            router_states[router_id].pending_connections[curr_idx] = router_states[router_id].pending_connections[next_idx];
                        }
                        router_states[router_id].pending_connections_count--;
                        // 调整 tail 指针
                        router_states[router_id].pending_conn_tail = (router_states[router_id].pending_conn_head + router_states[router_id].pending_connections_count) % MAX_PENDING_PACKETS;
                        removed_from_pending = 1;
                        printf("[DESD] R%d Plan P: removed conn_id=%lu from pending_connections, remaining=%d\n",
                               router_id, matched_connection_id, router_states[router_id].pending_connections_count);
                        break;
                    }
                }
                if (!removed_from_pending) {
                    printf("[DESD WARNING] R%d Plan P: conn_id=%lu not found in pending_connections queue\n",
                           router_id, matched_connection_id);
                }
                
                // 🔑 检查 client 是否已经先关闭了
                // 如果 client 在 server 完成 CONNECTION_INFO 前就 close 了，
                // 需要标记 server 侧的 peer_closed=1，这样后续发包时会返回 "Peer connection closed" 而不是 "Invalid connection mapping"
                if (client_router_id > 0 && check_client_already_closed(matched_connection_id, client_router_id)) {
                    router_states[router_id].connections[i].peer_closed = 1;
                    printf("[DESD-PEER-CLOSED] R%d (server) conn_id=%lu: client R%d already closed before CONNECTION_INFO completed. Marked peer_closed=1.\n",
                           router_id, matched_connection_id, client_router_id);
                }
                
                break;
            }
        }
        
        if (!found_virtual_connection) {
            // 方案 P：如果没找到虚拟连接，说明出现了异常情况
            // 可能是 real_peer_router_id 没有对应的虚拟连接
            printf("[DESD WARNING] R%d (server) Plan P: no virtual connection found for real_peer_router_id=%d. Attempting fallback...\n",
                   router_id, real_peer_router_id);

            // 调试：dump 当前 server 侧连接表，便于后续分析
            dump_router_connections(router_id, "CONN_INFO_SERVER_NO_VIRTUAL");
            
            // 尝试通过 real_peer_router_id 在 client 端连接表中查找
            if (real_peer_router_id > 0 && real_peer_router_id <= MAX_ROUTERS) {
                for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
                    if (router_states[real_peer_router_id].connections[i].is_active &&
                        router_states[real_peer_router_id].connections[i].peer_router_id == router_id) {
                        
                        client_router_id = real_peer_router_id;
                        peer_socket_fd = router_states[real_peer_router_id].connections[i].socket_fd;
                        matched_connection_id = router_states[real_peer_router_id].connections[i].connection_id;
                        router_states[real_peer_router_id].connections[i].peer_socket_fd = socket_fd;
                        printf("[PLAN-P-FALLBACK] R%d (server) fd=%d MATCHED with R%d (client) fd=%d using real_peer_router_id\n",
                               router_id, socket_fd, client_router_id, peer_socket_fd);
                        dump_router_connections(real_peer_router_id, "CONN_INFO_CLIENT_FALLBACK");
                        break;
                    }
                }
            }
            
            // 作为备用方案，注册新的服务器端连接
            register_connection(router_id, socket_fd, client_router_id, peer_socket_fd);
            for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
                if (router_states[router_id].connections[i].is_active &&
                    router_states[router_id].connections[i].socket_fd == socket_fd) {
                    router_states[router_id].connections[i].connection_id = matched_connection_id;
                    break;
                }
            }
            
            printf("[DESD] R%d (server) Plan P registered NEW connection (fallback): fd %d <-> R%d (client) fd %d.\n",
                   router_id, socket_fd, client_router_id, peer_socket_fd);
            if (client_router_id > 0 && client_router_id <= MAX_ROUTERS) {
                dump_router_connections(client_router_id, "CONN_INFO_CLIENT_FALLBACK_AFTER");
            }
        }
    }
    
    json_decref(payload_obj);
    send_success_response(router_id, event.thread_id, request_id, "CONNECTION_INFO", "Connection Info Recorded", NULL);
    
    // 回复发送后，将线程状态从 BLOCKED 改回 RUNNING
    pthread_mutex_lock(&router_states_mutex);
    ThreadInfo *ti = get_thread_info(router_id, event.thread_id);
    if (ti && ti->status == BLOCKED) {
        if (strncmp(ti->blocked_on_request_id, request_id, 63) == 0) {
            ti->status = RUNNING;
            ti->blocked_on_request_id[0] = '\0';
        }
    }
    pthread_mutex_unlock(&router_states_mutex);
}

void handle_close_socket_event(Event event) {
    int router_id = event.router_id;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_close_socket_event: Failed to parse payload JSON.\n");
        return;
    }
    
    int socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));
    const char *request_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    char request_id[64] = {0};
    if (request_id_ptr) {
        strncpy(request_id, request_id_ptr, 63);
    }
    
    printf("[DESD] R%d closed socket fd %d, performing single-sided cleanup.\n", router_id, socket_fd);
    
    // 清理该router的连接表中所有涉及该socket_fd的记录
    int cleaned_count = 0;
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (router_states[router_id].connections[i].is_active &&
            router_states[router_id].connections[i].socket_fd == socket_fd) {
            
            int peer_router_id = router_states[router_id].connections[i].peer_router_id;
            int peer_socket_fd = router_states[router_id].connections[i].peer_socket_fd;
            unsigned long conn_id = router_states[router_id].connections[i].connection_id;
            
            printf("[DESD] Removing local connection: R%d (fd:%d) <-> R%d (fd:%d) conn_id=%lu\n",
                   router_id, socket_fd, peer_router_id, peer_socket_fd, conn_id);
            
            // 🔑 新增：记录到 RecentClosedConn，用于检测"client 先关闭"的情况
            // 判断当前 router 是 client 还是 server：
            // - 如果 peer_socket_fd < 0（虚拟 fd）或 peer_socket_fd == -1，说明 server 侧还没完成 accept
            // - 此时当前 router 是 client 侧
            // 简化判断：如果当前 router 有一个 entry 指向 peer，且 peer 那边没有对应的 entry，
            // 或者 peer_socket_fd 是 -1，说明当前是 client 先关闭
            if (conn_id > 0 && peer_router_id > 0 && peer_router_id <= MAX_ROUTERS) {
                // 检查 peer 侧是否有这条连接的 entry
                int peer_has_entry = 0;
                for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                    if (router_states[peer_router_id].connections[j].is_active &&
                        router_states[peer_router_id].connections[j].connection_id == conn_id) {
                        peer_has_entry = 1;
                        break;
                    }
                }
                
                // 如果 peer_socket_fd == -1 或者 peer 侧还没有 entry，说明是 client 先关闭
                // 此时 router_id 是 client，peer_router_id 是 server
                if (peer_socket_fd == -1 || !peer_has_entry) {
                    record_closed_connection(conn_id, router_id, peer_router_id, 1);  // closed_side=1 表示 client
                } else {
                    // 否则记录为 server 关闭（或双方都已建立后的关闭）
                    // 这里仍然记录，但 closed_side=2
                    record_closed_connection(conn_id, peer_router_id, router_id, 2);  // closed_side=2 表示 server
                }
            }
            
            // 删除本router的连接记录
            router_states[router_id].connections[i].is_active = 0;
            router_states[router_id].connections[i].socket_fd = -1;
            router_states[router_id].connections[i].peer_router_id = -1;
            router_states[router_id].connections[i].peer_socket_fd = -1;
            router_states[router_id].connections[i].peer_closed = 0;
            cleaned_count++;
            
            // **增强清理：标记对端peer_closed并清除对端的peer_socket_fd**
            // 防止FD复用时出现映射混乱
            if (peer_router_id > 0 && peer_router_id <= MAX_ROUTERS && peer_socket_fd != -1) {
                for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                    if (router_states[peer_router_id].connections[j].is_active &&
                        router_states[peer_router_id].connections[j].socket_fd == peer_socket_fd &&
                        router_states[peer_router_id].connections[j].peer_router_id == router_id) {
                        
                        // 标记对端的peer已关闭
                        router_states[peer_router_id].connections[j].peer_closed = 1;
                        // 🔑 关键修复：清除对端记录的peer_socket_fd，防止FD复用后误匹配
                        router_states[peer_router_id].connections[j].peer_socket_fd = -1;
                        
                        printf("[DESD] Marked peer connection as closed: R%d (fd:%d) peer_closed=1, cleared peer_socket_fd to prevent FD reuse issues\n",
                               peer_router_id, peer_socket_fd);
                        break;
                    }
                }
            }
        }
    }
    
    // 清理该socket的pending数据包（本端已关闭，不会再读取这些数据）
    int cleared_buffers = 0;
    for (int i = 0; i < MAX_PENDING_PACKETS; i++) {
        if (router_states[router_id].packet_buffers[i].is_used &&
            router_states[router_id].packet_buffers[i].socket_fd == socket_fd) {
            printf("[DESD] Clearing pending packet buffer %d for R%d fd %d\n", i, router_id, socket_fd);
            router_states[router_id].packet_buffers[i].is_used = 0;
            router_states[router_id].packet_buffers[i].socket_fd = -1;
            memset(router_states[router_id].packet_buffers[i].data, 0, MAX_PACKET_SIZE);
            cleared_buffers++;
        }
    }
    
    // 同步压缩 pending_buffer_indices 队列，移除已经被清理的 buffer 引用
    if (cleared_buffers > 0) {
        int head = router_states[router_id].pending_buffer_head;
        int old_count = router_states[router_id].pending_packets_count;
        int write_pos = 0;
        
        // 遍历队列，只保留仍然有效的 buffer 索引
        for (int i = 0; i < old_count; i++) {
            int idx = (head + i) % MAX_PENDING_PACKETS;
            int buf_idx = router_states[router_id].pending_buffer_indices[idx];
            
            // 检查这个 buffer 是否仍然有效（is_used=1）
            if (buf_idx >= 0 && buf_idx < MAX_PENDING_PACKETS &&
                router_states[router_id].packet_buffers[buf_idx].is_used) {
                // 保留到压缩后的位置
                int new_pos = (head + write_pos) % MAX_PENDING_PACKETS;
                router_states[router_id].pending_buffer_indices[new_pos] = buf_idx;
                write_pos++;
            }
        }
        
        // 清理剩余位置
        for (int i = write_pos; i < old_count; i++) {
            int pos = (head + i) % MAX_PENDING_PACKETS;
            router_states[router_id].pending_buffer_indices[pos] = -1;
        }
        
        // 更新 count 和 tail
        router_states[router_id].pending_packets_count = write_pos;
        router_states[router_id].pending_buffer_tail = (head + write_pos) % MAX_PENDING_PACKETS;
        
        // 如果队列空了，重置 head 和 tail
        if (write_pos == 0) {
            router_states[router_id].pending_buffer_head = 0;
            router_states[router_id].pending_buffer_tail = 0;
        }
        
        printf("[DESD] R%d fd %d: cleared %d buffer(s), pending_packets_count: %d -> %d\n",
               router_id, socket_fd, cleared_buffers, old_count, write_pos);
    }
    
    printf("[DESD] R%d socket fd %d single-sided cleanup complete. Removed %d connection(s).\n", 
           router_id, socket_fd, cleaned_count);
    
    json_decref(payload_obj);
    
    // 发送SUCCESS响应，确保router在desd清理完成后才继续执行（DES框架要求）
    send_success_response(router_id, event.thread_id, request_id, "CLOSE_SOCKET", "Socket closed (single-sided)", NULL);
    
    // 回复发送后，将线程状态从 BLOCKED 改回 RUNNING
    pthread_mutex_lock(&router_states_mutex);
    ThreadInfo *ti = get_thread_info(router_id, event.thread_id);
    if (ti && ti->status == BLOCKED) {
        if (strncmp(ti->blocked_on_request_id, request_id, 63) == 0) {
            ti->status = RUNNING;
            ti->blocked_on_request_id[0] = '\0';
        }
    }
    pthread_mutex_unlock(&router_states_mutex);
}

void handle_router_block_request(Event event) {
    int router_id = event.router_id;
    int thread_id = event.thread_id;
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    
    // 极轻量统计 ROUTER_BLOCK_REQUEST 处理次数
    static unsigned long block_req_handle_count = 0;
    block_req_handle_count++;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_router_block_request: Failed to parse payload JSON.\n");
        return;
    }

    const char *req_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    if (req_id_ptr == NULL) {
        fprintf(stderr, "[DESD ERROR] handle_router_block_request: 'request_id' missing or not a string.\n");
        json_decref(payload_obj);
        return;
    }
    char request_id_local[64]; // Local buffer
    strncpy(request_id_local, req_id_ptr, 63);
    request_id_local[63] = '\0';

    const char *blocked_func_ptr = json_string_value(json_object_get(payload_obj, "blocked_function"));
    if (blocked_func_ptr == NULL) {
        fprintf(stderr, "[DESD ERROR] handle_router_block_request: 'blocked_function' missing or not a string.\n");
        json_decref(payload_obj);
        return;
    }
    char blocked_func_str_local[64]; // Local buffer
    strncpy(blocked_func_str_local, blocked_func_ptr, 63);
    blocked_func_str_local[63] = '\0';

    if (block_req_handle_count == 1 ||
        block_req_handle_count == 10 ||
        block_req_handle_count == 100 ||
        (block_req_handle_count % 1000) == 0) {
        printf("[DESD-BLOCK-REQ] R%d T%d handled ROUTER_BLOCK_REQUEST %lu time(s) (blocked_function=%s, ReqID=%s, current_VT=%.6f)\n",
               router_id, thread_id, block_req_handle_count,
               blocked_func_str_local, request_id_local, current_virtual_time);
    }

    // 解析 nonblocking 标志（用于 RECV_CALL）
    int is_nonblocking = 0;
    json_t *nonblocking_obj = json_object_get(payload_obj, "nonblocking");
    if (nonblocking_obj && json_is_boolean(nonblocking_obj)) {
        is_nonblocking = json_boolean_value(nonblocking_obj);
    }
    
    // 解析 socket_fd（用于检查 peer_closed 状态）
    int socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));

    json_decref(payload_obj);

    if (router_id > 0 && router_id <= MAX_ROUTERS && ti) {
        // 特殊处理 RECV_CALL：检查是否有 pending 数据包
        if (strcmp(blocked_func_str_local, "RECV_CALL") == 0) {
            // 🔧 修复：查找匹配socket_fd的pending packet，而非简单FIFO
            int found_buffer_index = -1;
            int found_queue_pos = -1;
            
            if (router_states[router_id].pending_packets_count > 0) {
                int head = router_states[router_id].pending_buffer_head;
                int count = router_states[router_id].pending_packets_count;
                
                // 遍历pending queue，查找匹配socket_fd的buffer
                for (int i = 0; i < count; i++) {
                    int queue_pos = (head + i) % MAX_PENDING_PACKETS;
                    int buf_idx = router_states[router_id].pending_buffer_indices[queue_pos];
                    
                    if (buf_idx >= 0 && buf_idx < MAX_PENDING_PACKETS &&
                        router_states[router_id].packet_buffers[buf_idx].is_used &&
                        router_states[router_id].packet_buffers[buf_idx].socket_fd == socket_fd) {
                        // 找到匹配的buffer
                        found_buffer_index = buf_idx;
                        found_queue_pos = i;  // 相对于head的位置
                        break;
                    }
                }
            }
            
            if (found_buffer_index >= 0) {
                // 找到了匹配socket_fd的数据，立即唤醒
                router_states[router_id].pending_packets_count--;
                // 只有当线程确实被这个 BLOCK_REQUEST 阻塞时才改 status
                if (ti->status == BLOCKED && strcmp(ti->blocked_on_request_id, request_id_local) == 0) {
                    ti->status = RUNNING;
                    ti->blocked_on_request_id[0] = '\0';
                }
                
                // 从队列中移除找到的buffer（压缩队列）
                int head = router_states[router_id].pending_buffer_head;
                int old_count = router_states[router_id].pending_packets_count + 1; // 移除前的数量
                
                // 将找到的元素之后的所有元素前移一位
                for (int i = found_queue_pos; i < old_count - 1; i++) {
                    int curr_pos = (head + i) % MAX_PENDING_PACKETS;
                    int next_pos = (head + i + 1) % MAX_PENDING_PACKETS;
                    router_states[router_id].pending_buffer_indices[curr_pos] = 
                        router_states[router_id].pending_buffer_indices[next_pos];
                }
                
                // 清掉最后一个位置（已经被左移覆盖，需要标记为无效）
                int last_pos = (head + old_count - 1) % MAX_PENDING_PACKETS;
                router_states[router_id].pending_buffer_indices[last_pos] = -1;
                
                // 更新 tail，保持 tail == (head + count) % MAX_PENDING_PACKETS 的不变量
                int new_count = router_states[router_id].pending_packets_count; // 已经减过1了
                router_states[router_id].pending_buffer_tail = (head + new_count) % MAX_PENDING_PACKETS;
                
                // 如果队列空了，重置 head 和 tail 到 0
                if (new_count == 0) {
                    router_states[router_id].pending_buffer_head = 0;
                    router_states[router_id].pending_buffer_tail = 0;
                }
                
                // 发送数据给路由器
                json_t *resp_payload = json_object();
                json_object_set_new(resp_payload, "status", json_string("SUCCESS"));
                json_object_set_new(resp_payload, "blocked_function", json_string("RECV"));
                json_object_set_new(resp_payload, "message", json_string("Packet Available"));
                json_object_set_new(resp_payload, "packet_data", 
                                  json_string(router_states[router_id].packet_buffers[found_buffer_index].data));
                
                char *payload_str = json_dumps(resp_payload, JSON_COMPACT);
                json_decref(resp_payload);
                
                Message response = {
                    .message_type = DESD_TO_HOOK,
                    .router_id = router_id,
                    .thread_id = thread_id,
                    .virtual_time = current_virtual_time
                };
                strncpy(response.request_id, request_id_local, 63);
                response.request_id[63] = '\0';
                strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
                response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                free(payload_str);
                send_message_to_router(router_id, &response);
                
                // 释放缓冲区：同时清 is_used 和 socket_fd，避免残留状态被 SELECT 误判
                router_states[router_id].packet_buffers[found_buffer_index].is_used = 0;
                router_states[router_id].packet_buffers[found_buffer_index].socket_fd = -1;
                
                printf("[DESD] R%d recv(fd:%d) immediately unblocked with matching packet (had %d pending packet(s)).\n", 
                       router_id, socket_fd, router_states[router_id].pending_packets_count + 1);
            } else {
                // 没有找到匹配socket_fd的数据（可能有其他fd的pending packets）
                if (router_states[router_id].pending_packets_count > 0) {
                    printf("[DESD] R%d recv(fd:%d) blocked: has %d pending packet(s) but none match this socket_fd.\n",
                           router_id, socket_fd, router_states[router_id].pending_packets_count);
                }
                
                // 检查对端是否已关闭连接
                int peer_has_closed = 0;
                for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
                    if (router_states[router_id].connections[i].is_active &&
                        router_states[router_id].connections[i].socket_fd == socket_fd) {
                        peer_has_closed = router_states[router_id].connections[i].peer_closed;
                        break;
                    }
                }
                
                if (peer_has_closed) {
                    // 对端已关闭且无数据，返回EOF（0字节）
                    // 只有当线程确实被这个 BLOCK_REQUEST 阻塞时才改 status
                    if (ti->status == BLOCKED && strcmp(ti->blocked_on_request_id, request_id_local) == 0) {
                        ti->status = RUNNING;
                        ti->blocked_on_request_id[0] = '\0';
                    }
                    
                    json_t *resp_payload = json_object();
                    json_object_set_new(resp_payload, "status", json_string("EOF"));
                    json_object_set_new(resp_payload, "blocked_function", json_string("RECV"));
                    json_object_set_new(resp_payload, "message", json_string("Connection closed by peer"));
                    json_object_set_new(resp_payload, "packet_data", json_string(""));  // 空数据表示EOF
                    
                    char *payload_str = json_dumps(resp_payload, JSON_COMPACT);
                    json_decref(resp_payload);
                    
                    Message response = {
                        .message_type = DESD_TO_HOOK,
                        .router_id = router_id,
                        .thread_id = thread_id,
                        .virtual_time = current_virtual_time
                    };
                    strncpy(response.request_id, request_id_local, 63);
                    response.request_id[63] = '\0';
                    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
                    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                    free(payload_str);
                    send_message_to_router(router_id, &response);
                    
                    printf("[DESD] R%d recv() returning EOF - peer has closed connection (fd:%d).\n", 
                           router_id, socket_fd);
                } else if (is_nonblocking) {
                    // 非阻塞模式：立即返回 EAGAIN
                    // 只有当线程确实被这个 BLOCK_REQUEST 阻塞时才改 status
                    if (ti->status == BLOCKED && strcmp(ti->blocked_on_request_id, request_id_local) == 0) {
                        ti->status = RUNNING;
                        ti->blocked_on_request_id[0] = '\0';
                    }
                    send_eagain_response(router_id, thread_id, request_id_local);
                    printf("[DESD] R%d recv() on non-blocking socket, no data available, returning EAGAIN.\n", router_id);
                } else {
                    // 阻塞模式：保持阻塞
                    ti->status = BLOCKED;
                    strncpy(ti->blocked_on_request_id, request_id_local, 63);
                    ti->blocked_on_request_id[63] = '\0';
                    strncpy(ti->blocked_on_function, blocked_func_str_local, 63);
                    ti->blocked_on_function[63] = '\0';
                    printf("[DESD] R%d blocked on %s (ReqID: %s) - no pending packets.\n",
                           router_id, blocked_func_str_local, request_id_local);
                }
            }
        } else if (strcmp(blocked_func_str_local, "ACCEPT_CALL") == 0) {
            // 方案 P：ACCEPT_CALL 只检查是否有 pending 连接，不出队/不绑定具体 connection_id
            // 真正的绑定在 CONNECTION_INFO_EVENT 中根据 real_peer_router_id 后验匹配完成
            if (router_states[router_id].pending_connections_count > 0) {
                // 只有当线程确实被这个 BLOCK_REQUEST 阻塞时才改 status
                if (ti->status == BLOCKED && strcmp(ti->blocked_on_request_id, request_id_local) == 0) {
                    ti->status = RUNNING;
                    ti->blocked_on_request_id[0] = '\0';
                }
                
                // 返回简单的 SUCCESS 响应，不包含具体连接信息
                // libdeshook 将在 real_accept() 成功后通过 CONNECTION_INFO_EVENT 报告真实 peer
                Message accept_resp;
                memset(&accept_resp, 0, sizeof(Message));
                accept_resp.message_type = DESD_TO_HOOK;
                accept_resp.router_id = router_id;
                accept_resp.thread_id = thread_id;
                strncpy(accept_resp.request_id, request_id_local, 63);
                accept_resp.request_id[63] = '\0';
                
                json_t *resp_payload = json_object();
                json_object_set_new(resp_payload, "status", json_string("SUCCESS"));
                json_object_set_new(resp_payload, "function", json_string("ACCEPT"));
                json_object_set_new(resp_payload, "message", json_string("Connection Available"));
                json_object_set_new(resp_payload, "pending_count", json_integer(router_states[router_id].pending_connections_count));
                char *resp_str = json_dumps(resp_payload, JSON_COMPACT);
                strncpy(accept_resp.payload.json_str, resp_str, MAX_MSG_SIZE - 1);
                accept_resp.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                free(resp_str);
                json_decref(resp_payload);
                
                send_message_to_router(router_id, &accept_resp);
                
                printf("[DESD] R%d accept() unblocked (Plan P): pending_count=%d, will match by real_peer in CONNECTION_INFO_EVENT\n", 
                       router_id, router_states[router_id].pending_connections_count);
            } else {
                // 连接还没建立，保持阻塞
                ti->status = BLOCKED;
                strncpy(ti->blocked_on_request_id, request_id_local, 63);
                ti->blocked_on_request_id[63] = '\0';
                strncpy(ti->blocked_on_function, blocked_func_str_local, 63);
                ti->blocked_on_function[63] = '\0';
                printf("[DESD] R%d blocked on %s (ReqID: %s) - no pending connections.\n",
                       router_id, blocked_func_str_local, request_id_local);
            }
        } else if (strcmp(blocked_func_str_local, "SELECT_CALL") == 0) {
            // 处理 SELECT_CALL：支持POLLIN/POLLOUT/POLLERR/POLLHUP等事件
            json_error_t error2;
            json_t *payload_obj2 = json_loads(event.payload.json_str, 0, &error2);
            int timeout_ms = -1;
            json_t *monitored_fds_array = NULL;
            
            if (payload_obj2) {
                timeout_ms = json_integer_value(json_object_get(payload_obj2, "timeout_ms"));
                monitored_fds_array = json_object_get(payload_obj2, "monitored_fds");
            }
            
            // 构建就绪的 FD 列表及其 revents
            json_t *ready_fds_array = json_array();
            int has_ready_fds = 0;
            
            // [DEBUG] 记录SELECT_CALL的详细信息
            size_t monitored_fds_count = monitored_fds_array && json_is_array(monitored_fds_array) ? 
                                         json_array_size(monitored_fds_array) : 0;
            printf("[DEBUG-SELECT] R%d SELECT_CALL: timeout=%dms, monitoring %zu fd(s), pending_connections=%d\n",
                   router_id, timeout_ms, monitored_fds_count, router_states[router_id].pending_connections_count);
            
            // 遍历所有监听的 FD，检查各种事件
            if (monitored_fds_array && json_is_array(monitored_fds_array)) {
                size_t array_size = json_array_size(monitored_fds_array);
                
                for (size_t i = 0; i < array_size; i++) {
                    json_t *fd_info = json_array_get(monitored_fds_array, i);
                    
                    // 解析 {fd, events} 对象
                    if (!json_is_object(fd_info)) continue;
                    
                    int fd = json_integer_value(json_object_get(fd_info, "fd"));
                    int events = json_integer_value(json_object_get(fd_info, "events"));
                    
                    if (fd < 0) continue;
                    
                    int revents = 0;
                    
                    // 检查 POLLIN：是否有数据可读或有新连接待accept
                    if (events & 0x001) {  // POLLIN = 0x001
                        // 1. 检查是否有数据包可读
                        int head = router_states[router_id].pending_buffer_head;
                        int count = router_states[router_id].pending_packets_count;
                        
                        for (int j = 0; j < count; j++) {
                            int idx = (head + j) % MAX_PENDING_PACKETS;
                            int buf_idx = router_states[router_id].pending_buffer_indices[idx];
                            if (buf_idx >= 0 && buf_idx < MAX_PENDING_PACKETS &&
                                router_states[router_id].packet_buffers[buf_idx].is_used &&
                                router_states[router_id].packet_buffers[buf_idx].socket_fd == fd) {
                                revents |= 0x001;  // POLLIN - 数据可读
                                break;
                            }
                        }
                        
                        // 2. 检查是否是listening socket且有pending连接
                        if (!(revents & 0x001)) {  // 如果还没有设置POLLIN
                            for (int j = 0; j < router_states[router_id].listen_count; j++) {
                                if (router_states[router_id].listening_socket_fds[j] == fd) {
                                    // 这是一个listening socket
                                    printf("[DEBUG-LISTEN] R%d fd=%d is listening socket (listen_idx=%d), pending=%d\n",
                                           router_id, fd, j, router_states[router_id].pending_connections_count);
                                    if (router_states[router_id].pending_connections_count > 0) {
                                        revents |= 0x001;  // POLLIN - 新连接待accept
                                        printf("[DEBUG-LISTEN] R%d fd=%d set POLLIN due to %d pending connection(s)\n",
                                               router_id, fd, router_states[router_id].pending_connections_count);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    
                    // 检查 POLLOUT：socket 是否可写（仅对已知的活动连接fd生效）
                    if (events & 0x004) {  // POLLOUT = 0x004
                        // 查找该 fd 的连接信息
                        for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                            if (router_states[router_id].connections[j].is_active &&
                                router_states[router_id].connections[j].socket_fd == fd) {
                                // 连接存在且活跃，socket 可写
                                revents |= 0x004;  // POLLOUT
                                break;
                            }
                        }
                        // 未找到的fd（非DES管理的fd）不设置任何就绪位，避免误唤醒
                    }
                    
                    // 检查 POLLERR/POLLHUP：仅对已知连接fd设置
                    for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                        if (router_states[router_id].connections[j].socket_fd == fd) {
                            if (!router_states[router_id].connections[j].is_active) {
                                revents |= 0x010;  // POLLHUP - 连接已断开
                            }
                            break;
                        }
                    }
                    
                    // 如果有任何事件就绪，按更严格的条件将其添加到结果列表
                    if (revents != 0) {
                        // 关键过滤：
                        // 1) 仅当是监听socket且pending_connections>0的POLLIN才算就绪
                        // 2) 或者是数据socket：
                        //    - 有对应pending_packets的POLLIN
                        //    - 已知活跃连接fd的POLLOUT
                        // 3) 不因未知fd的HUP/ERR而唤醒
                        int eligible = 0;
                        // 判断是否监听socket
                        int is_listen_fd = 0;
                        for (int j = 0; j < router_states[router_id].listen_count; j++) {
                            if (router_states[router_id].listening_socket_fds[j] == fd) {
                                is_listen_fd = 1; break;
                            }
                        }

                        if ((revents & 0x001) && (events & 0x001) && is_listen_fd && router_states[router_id].pending_connections_count > 0) {
                            // 监听socket有待accept连接
                            eligible = 1;
                        } else {
                            // 非监听：检查是否数据socket
                            int is_known_conn_fd = 0;
                            for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                                if (router_states[router_id].connections[j].is_active &&
                                    router_states[router_id].connections[j].socket_fd == fd) {
                                    is_known_conn_fd = 1; break;
                                }
                            }
                            if (is_known_conn_fd) {
                                if ((revents & 0x004) && (events & 0x004)) {
                                    // 已知连接上的POLLOUT
                                    eligible = 1;
                                } else if ((revents & 0x001) && (events & 0x001)) {
                                    // 已知连接上的POLLIN需确认确有数据（pending_packets匹配该fd，且is_used=1）
                                    int head = router_states[router_id].pending_buffer_head;
                                    int count = router_states[router_id].pending_packets_count;
                                    for (int j = 0; j < count; j++) {
                                        int idx = (head + j) % MAX_PENDING_PACKETS;
                                        int buf_idx = router_states[router_id].pending_buffer_indices[idx];
                                        if (buf_idx >= 0 && buf_idx < MAX_PENDING_PACKETS &&
                                            router_states[router_id].packet_buffers[buf_idx].is_used &&
                                            router_states[router_id].packet_buffers[buf_idx].socket_fd == fd) {
                                            eligible = 1; break;
                                        }
                                    }
                                }
                            }
                        }

                        if (eligible) {
                            json_t *ready_fd_info = json_object();
                            json_object_set_new(ready_fd_info, "fd", json_integer(fd));
                            json_object_set_new(ready_fd_info, "revents", json_integer(revents));
                            json_array_append_new(ready_fds_array, ready_fd_info);
                            has_ready_fds = 1;
                            printf("[DEBUG-SELECT-READY] R%d add ready fd=%d revents=0x%x (listen=%d, pending_conn=%d)\n",
                                   router_id, fd, revents, is_listen_fd, router_states[router_id].pending_connections_count);
                        }
                    }
                }
            }
            
            if (payload_obj2) {
                json_decref(payload_obj2);
            }
            
            // 如果有就绪的 FD，立即唤醒
            if (has_ready_fds) {
                // 只有当线程确实被这个 BLOCK_REQUEST 阻塞时才改 status
                if (ti->status == BLOCKED && strcmp(ti->blocked_on_request_id, request_id_local) == 0) {
                    ti->status = RUNNING;
                    ti->blocked_on_request_id[0] = '\0';
                }
                // 清除保存的monitored_fds信息（因为已经返回，不再需要）
                ti->monitored_fds_count = 0;
                
                size_t ready_fds_count = json_array_size(ready_fds_array);
                json_t *response_payload = json_object();
                json_object_set_new(response_payload, "status", json_string("SUCCESS"));
                json_object_set_new(response_payload, "blocked_function", json_string("SELECT"));
                json_object_set_new(response_payload, "message", json_string("FDs Ready"));
                json_object_set_new(response_payload, "ready_fds", ready_fds_array);
                
                char *response_payload_str = json_dumps(response_payload, JSON_COMPACT);
                json_decref(response_payload);
                
                Message response = {
                    .message_type = DESD_TO_HOOK,
                    .router_id = router_id,
                    .thread_id = thread_id,
                    .virtual_time = current_virtual_time
                };
                strncpy(response.request_id, request_id_local, 63);
                response.request_id[63] = '\0';
                strncpy(response.payload.json_str, response_payload_str, MAX_MSG_SIZE - 1);
                response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                free(response_payload_str);
                send_message_to_router(router_id, &response);
                
                printf("[DESD] R%d select/poll() immediately unblocked (%zu ready FD(s)).\n", 
                       router_id, ready_fds_count);
            } else if (timeout_ms == 0) {
                // 特殊处理：timeout=0 且无数据就绪，立即返回超时
                json_decref(ready_fds_array);
                // 只有当线程确实被这个 BLOCK_REQUEST 阻塞时才改 status
                if (ti->status == BLOCKED && strcmp(ti->blocked_on_request_id, request_id_local) == 0) {
                    ti->status = RUNNING;
                    ti->blocked_on_request_id[0] = '\0';
                }
                send_timeout_response(router_id, thread_id, request_id_local, "SELECT_CALL");
                printf("[DESD] R%d select/poll() immediate timeout (timeout=0).\n", router_id);
            } else {
                // 释放空的 ready_fds_array
                json_decref(ready_fds_array);
                // 数据未就绪，阻塞并可能注册超时事件
                ti->status = BLOCKED;
                strncpy(ti->blocked_on_request_id, request_id_local, 63);
                ti->blocked_on_request_id[63] = '\0';
                strncpy(ti->blocked_on_function, blocked_func_str_local, 63);
                ti->blocked_on_function[63] = '\0';
                
                // 保存select()监听的fd信息，用于PACKET_RECEIVE_EVENT时判断是否需要唤醒
                ti->monitored_fds_count = 0;
                if (monitored_fds_array && json_is_array(monitored_fds_array)) {
                    size_t array_size = json_array_size(monitored_fds_array);
                    for (size_t i = 0; i < array_size && i < 64; i++) {
                        json_t *fd_info = json_array_get(monitored_fds_array, i);
                        if (json_is_object(fd_info)) {
                            int fd = json_integer_value(json_object_get(fd_info, "fd"));
                            int events = json_integer_value(json_object_get(fd_info, "events"));
                            if (fd >= 0) {
                                ti->monitored_fds[ti->monitored_fds_count] = fd;
                                ti->monitored_fds_events[ti->monitored_fds_count] = events;
                                ti->monitored_fds_count++;
                            }
                        }
                    }
                }
                
                if (timeout_ms > 0) {
                    // 注册超时事件
                    double timeout_seconds = timeout_ms / 1000.0;
                    double timeout_time = current_virtual_time + timeout_seconds;
                    
                    Event timeout_event = {
                        .timestamp = timeout_time,
                        .router_id = router_id,
                        .thread_id = thread_id,  // 修复：正确设置 thread_id
                        .event_type = TIMEOUT_EVENT,
                        .event_id = generate_event_id()
                    };
                    
                    json_t *timeout_payload_obj = json_object();
                    json_object_set_new(timeout_payload_obj, "original_block_request_id", json_string(request_id_local));
                    json_object_set_new(timeout_payload_obj, "timeout_type", json_string("SELECT_CALL"));
                    char *timeout_payload_str = json_dumps(timeout_payload_obj, JSON_COMPACT);
                    strncpy(timeout_event.payload.json_str, timeout_payload_str, MAX_MSG_SIZE - 1);
                    timeout_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                    free(timeout_payload_str);
                    json_decref(timeout_payload_obj);
                    
                    push_event(timeout_event);
                    ti->pending_timeout_event_id = timeout_event.event_id;
                    
                    printf("[DESD] R%d blocked on %s (ReqID: %s) with %dms timeout. Registered TIMEOUT_EVENT (ID: %lu) at VT=%.3f.\n",
                           router_id, blocked_func_str_local, request_id_local, timeout_ms, timeout_event.event_id, timeout_time);
                } else {
                    printf("[DESD] R%d blocked on %s (ReqID: %s) - infinite timeout.\n",
                           router_id, blocked_func_str_local, request_id_local);
                }
            }
        } else if (strcmp(blocked_func_str_local, "SLEEP_CALL") == 0) {
            // 处理 SLEEP_CALL：直接注册一个唤醒事件
            json_error_t error2;
            json_t *payload_obj2 = json_loads(event.payload.json_str, 0, &error2);
            int sleep_seconds = 0;
            if (payload_obj2) {
                sleep_seconds = json_integer_value(json_object_get(payload_obj2, "sleep_seconds"));
                json_decref(payload_obj2);
            }
            
            ti->status = BLOCKED;
            strncpy(ti->blocked_on_request_id, request_id_local, 63);
            ti->blocked_on_request_id[63] = '\0';
            strncpy(ti->blocked_on_function, blocked_func_str_local, 63);
            ti->blocked_on_function[63] = '\0';
            
            // 注册唤醒事件（类似超时事件）
            double wakeup_time = current_virtual_time + sleep_seconds;
            
            Event wakeup_event = {
                .timestamp = wakeup_time,
                .router_id = router_id,
                .thread_id = thread_id,  // 修复：正确设置 thread_id
                .event_type = TIMEOUT_EVENT,  // 复用 TIMEOUT_EVENT
                .event_id = generate_event_id()
            };
            
            json_t *wakeup_payload_obj = json_object();
            json_object_set_new(wakeup_payload_obj, "original_block_request_id", json_string(request_id_local));
            json_object_set_new(wakeup_payload_obj, "timeout_type", json_string("SLEEP_CALL"));
            char *wakeup_payload_str = json_dumps(wakeup_payload_obj, JSON_COMPACT);
            strncpy(wakeup_event.payload.json_str, wakeup_payload_str, MAX_MSG_SIZE - 1);
            wakeup_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
            free(wakeup_payload_str);
            json_decref(wakeup_payload_obj);
            
            push_event(wakeup_event);
            
            printf("[DESD] R%d blocked on %s (ReqID: %s) for %d seconds. Registered wakeup event (ID: %lu) at VT=%.3f.\n",
                   router_id, blocked_func_str_local, request_id_local, sleep_seconds, wakeup_event.event_id, wakeup_time);
        } else {
            // 其他类型的阻塞请求
            ti->status = BLOCKED;
            strncpy(ti->blocked_on_request_id, request_id_local, 63);
            ti->blocked_on_request_id[63] = '\0';
            strncpy(ti->blocked_on_function, blocked_func_str_local, 63);
            ti->blocked_on_function[63] = '\0';
            printf("[DESD] R%d blocked on %s (ReqID: %s).\n",
                   router_id, blocked_func_str_local, request_id_local);
        }
    }
}

void handle_packet_send_event(Event event) {
    int source_router_id = event.router_id;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_packet_send_event: Failed to parse payload JSON.\n");
        return;
    }
    const char *request_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    const char *destination_abstract_address_ptr = json_string_value(json_object_get(payload_obj, "destination_abstract_address"));
    const char *packet_data_ptr = json_string_value(json_object_get(payload_obj, "packet_data"));
    
    // 复制到本地缓冲区，防止 json_decref 后访问无效内存
    char request_id[64] = {0};
    char destination_abstract_address[256] = {0};
    char packet_data[MAX_PACKET_SIZE] = {0};
    
    if (request_id_ptr) {
        strncpy(request_id, request_id_ptr, sizeof(request_id) - 1);
    }
    if (destination_abstract_address_ptr) {
        strncpy(destination_abstract_address, destination_abstract_address_ptr, sizeof(destination_abstract_address) - 1);
    }
    if (packet_data_ptr) {
        strncpy(packet_data, packet_data_ptr, MAX_PACKET_SIZE - 1);
    }
    
    // 解析 socket_fd
    int socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));
    
    json_decref(payload_obj);

    // send()操作本身是瞬时的，发送方不会阻塞
    int thread_id = event.thread_id;
    ThreadInfo *ti = get_thread_info(source_router_id, thread_id);
    if (source_router_id > 0 && source_router_id <= MAX_ROUTERS && ti) {
        ti->status = RUNNING;
        memset(ti->blocked_on_request_id, 0, 64);
        memset(ti->blocked_on_function, 0, 64);
    }

    double transmission_delay = 0.1; // 0.1s (100ms) - 数据包传输延迟
    double receive_time = current_virtual_time + transmission_delay;

    // 使用连接表查找目标路由器
    int target_router_id = find_peer_router(source_router_id, socket_fd);
    
    if (target_router_id == -1) {
        fprintf(stderr, "[DESD ERROR] R%d: No peer found for socket fd %d. Cannot send packet.\n", 
                source_router_id, socket_fd);
        // 调试：在判定 Invalid connection 前 dump 源/目的路由器的连接映射
        dump_router_connections(source_router_id, "SEND_SRC_ON_INVALID_MAP");
        dump_router_connections(target_router_id, "SEND_DST_ON_INVALID_MAP");
        send_error_response(source_router_id, event.thread_id, request_id, "Invalid connection");
        return;
    }
    
    // 检查对端是否已关闭连接
    int peer_has_closed = 0;
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (router_states[source_router_id].connections[i].is_active &&
            router_states[source_router_id].connections[i].socket_fd == socket_fd) {
            peer_has_closed = router_states[source_router_id].connections[i].peer_closed;
            break;
        }
    }
    
    if (peer_has_closed) {
        fprintf(stderr, "[DESD ERROR] R%d: Cannot send on fd %d - peer has closed connection.\n",
                source_router_id, socket_fd);
        send_error_response(source_router_id, event.thread_id, request_id, "Peer connection closed");
        return;
    }

    // 查找目标路由器对应这个连接的 socket_fd（使用源路由器的 socket_fd 进行精确匹配）
    int target_socket_fd = find_socket_fd_for_peer(target_router_id, source_router_id, socket_fd);
    if (target_socket_fd == -1) {
        fprintf(stderr, "[DESD ERROR] R%d: Cannot find socket_fd for connection from R%d (source fd=%d).\n",
                target_router_id, source_router_id, socket_fd);
        // 调试：Invalid connection mapping 专用 dump，方便分析双向连接表不一致
        dump_router_connections(source_router_id, "SEND_SRC_ON_INVALID_MAPPING");
        dump_router_connections(target_router_id, "SEND_DST_ON_INVALID_MAPPING");
        send_error_response(source_router_id, event.thread_id, request_id, "Invalid connection mapping");
        return;
    }

    // 调试：在发送时对比源和目标两侧的 connection_id / peer_closed，检查是否误用了旧连接映射
    unsigned long src_conn_id = 0;
    int src_peer_router = -1;
    int src_peer_fd = -1;
    int src_peer_closed = 0;
    int found_src = 0;
    if (source_router_id > 0 && source_router_id <= MAX_ROUTERS) {
        for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
            if (router_states[source_router_id].connections[i].is_active &&
                router_states[source_router_id].connections[i].socket_fd == socket_fd) {
                src_conn_id = router_states[source_router_id].connections[i].connection_id;
                src_peer_router = router_states[source_router_id].connections[i].peer_router_id;
                src_peer_fd = router_states[source_router_id].connections[i].peer_socket_fd;
                src_peer_closed = router_states[source_router_id].connections[i].peer_closed;
                found_src = 1;
                break;
            }
        }
    }

    unsigned long dst_conn_id = 0;
    int dst_peer_router = -1;
    int dst_peer_fd = -1;
    int dst_peer_closed = 0;
    int found_dst = 0;
    if (target_router_id > 0 && target_router_id <= MAX_ROUTERS) {
        for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
            if (router_states[target_router_id].connections[i].is_active &&
                router_states[target_router_id].connections[i].socket_fd == target_socket_fd) {
                dst_conn_id = router_states[target_router_id].connections[i].connection_id;
                dst_peer_router = router_states[target_router_id].connections[i].peer_router_id;
                dst_peer_fd = router_states[target_router_id].connections[i].peer_socket_fd;
                dst_peer_closed = router_states[target_router_id].connections[i].peer_closed;
                found_dst = 1;
                break;
            }
        }
    }

    printf("[DESD-SEND-MAP] R%d send(fd:%d) conn_id=%lu peer_router=%d peer_fd=%d peer_closed=%d  ->  R%d recv(fd:%d) conn_id=%lu peer_router=%d peer_fd=%d peer_closed=%d (found_src=%d found_dst=%d)\n",
           source_router_id, socket_fd, src_conn_id, src_peer_router, src_peer_fd, src_peer_closed,
           target_router_id, target_socket_fd, dst_conn_id, dst_peer_router, dst_peer_fd, dst_peer_closed,
           found_src, found_dst);

    if (found_src && found_dst) {
        if (src_conn_id != 0 && dst_conn_id != 0 && src_conn_id != dst_conn_id) {
            printf("[DESD-SEND-MAP WARNING] connection_id mismatch when sending: src_conn_id=%lu dst_conn_id=%lu (R%d fd:%d -> R%d fd:%d)\n",
                   src_conn_id, dst_conn_id, source_router_id, socket_fd, target_router_id, target_socket_fd);
        }
        if (dst_peer_closed) {
            printf("[DESD-SEND-MAP WARNING] target connection appears peer_closed=1 when sending: R%d fd:%d (conn_id=%lu)\n",
                   target_router_id, target_socket_fd, dst_conn_id);
        }
    }

    // 将数据存储到目标路由器的缓冲区
    int buffer_index = -1;
    for (int i = 0; i < MAX_PENDING_PACKETS; i++) {
        if (!router_states[target_router_id].packet_buffers[i].is_used) {
            buffer_index = i;
            break;
        }
    }
    
    if (buffer_index == -1) {
        fprintf(stderr, "[DESD ERROR] R%d: No free packet buffer for R%d. Dropping packet.\n",
                source_router_id, target_router_id);
        send_error_response(source_router_id, event.thread_id, request_id, "Packet buffer full");
        return;
    }
    
    // 缓冲数据，并记录对应的 socket_fd
    strncpy(router_states[target_router_id].packet_buffers[buffer_index].data, packet_data, MAX_PACKET_SIZE - 1);
    router_states[target_router_id].packet_buffers[buffer_index].data_len = strlen(packet_data);
    router_states[target_router_id].packet_buffers[buffer_index].socket_fd = target_socket_fd;
    router_states[target_router_id].packet_buffers[buffer_index].is_used = 1;

    // 构造更清晰的描述信息，使用路由器ID和socket fd而不是旧的destination_abstract_address
    char connection_desc[256];
    snprintf(connection_desc, sizeof(connection_desc), "R%d(fd:%d) -> R%d(fd:%d)", 
             source_router_id, socket_fd, target_router_id, target_socket_fd);
    
    json_t *recv_payload_obj = json_object();
    json_object_set_new(recv_payload_obj, "source_router_id", json_integer(source_router_id));
    json_object_set_new(recv_payload_obj, "destination_abstract_address", json_string(connection_desc));
    json_object_set_new(recv_payload_obj, "buffer_index", json_integer(buffer_index));  // 传递缓冲区索引
    char *recv_payload_str = json_dumps(recv_payload_obj, JSON_COMPACT);
    json_decref(recv_payload_obj);

    Event receive_event = {
        .timestamp = receive_time,
        .router_id = target_router_id,
        .event_type = PACKET_RECEIVE_EVENT,
        .event_id = generate_event_id()
    };
    strncpy(receive_event.payload.json_str, recv_payload_str, MAX_MSG_SIZE - 1);
    receive_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(recv_payload_str);
    push_event(receive_event);
    printf("[DESD] R%d (fd:%d) sent packet to R%d. Scheduled PACKET_RECEIVE_EVENT at VT=%.3f (EventID: %lu).\n",
           source_router_id, socket_fd, target_router_id, receive_time, receive_event.event_id);
    
    send_success_response(source_router_id, event.thread_id, request_id, "SEND", "Packet Sent", NULL); // 发送方send()成功返回
}

void handle_packet_receive_event(Event event) {
    int target_router_id = event.router_id;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_packet_receive_event: Failed to parse payload JSON.\n");
        return;
    }
    const char *destination_abstract_address_ptr = json_string_value(json_object_get(payload_obj, "destination_abstract_address"));
    int buffer_index = json_integer_value(json_object_get(payload_obj, "buffer_index"));
    
    // 复制到本地缓冲区，防止 json_decref 后访问无效内存
    char destination_abstract_address[256] = {0};
    if (destination_abstract_address_ptr) {
        strncpy(destination_abstract_address, destination_abstract_address_ptr, sizeof(destination_abstract_address) - 1);
    }
    
    json_decref(payload_obj);

    if (target_router_id > 0 && target_router_id <= MAX_ROUTERS) {
        printf("[DESD] R%d received PACKET_RECEIVE_EVENT for %s.\n",
               target_router_id, destination_abstract_address);

        // 查找阻塞在 RECV_CALL 或 SELECT_CALL 上的线程
        ThreadInfo *blocked_ti = find_blocked_thread(target_router_id, "RECV_CALL");
        if (!blocked_ti) {
            blocked_ti = find_blocked_thread(target_router_id, "SELECT_CALL");
        }
        
        if (blocked_ti && blocked_ti->blocked_on_request_id[0] != '\0') {
            
            const char *blocked_func = blocked_ti->blocked_on_function;
            
            if (strcmp(blocked_func, "RECV_CALL") == 0) {
                // 情况1a：接收方已经在 recv 上等待，立即唤醒
                char request_id[64];
                strncpy(request_id, blocked_ti->blocked_on_request_id, 63);
                request_id[63] = '\0';

                blocked_ti->status = RUNNING;
                memset(blocked_ti->blocked_on_request_id, 0, 64);
                memset(blocked_ti->blocked_on_function, 0, 64);
                
                // 从缓冲区读取数据并发送给路由器
                if (buffer_index >= 0 && buffer_index < MAX_PENDING_PACKETS &&
                    router_states[target_router_id].packet_buffers[buffer_index].is_used) {
                    
                    json_t *resp_payload = json_object();
                    json_object_set_new(resp_payload, "status", json_string("SUCCESS"));
                    json_object_set_new(resp_payload, "blocked_function", json_string("RECV"));
                    json_object_set_new(resp_payload, "message", json_string("Packet Available"));
                    json_object_set_new(resp_payload, "packet_data", 
                                      json_string(router_states[target_router_id].packet_buffers[buffer_index].data));
                    
                    char *payload_str = json_dumps(resp_payload, JSON_COMPACT);
                    json_decref(resp_payload);
                    
                    Message response = {
                        .message_type = DESD_TO_HOOK,
                        .router_id = target_router_id,
                        .thread_id = blocked_ti->thread_id,  // 修复：使用被唤醒线程的 thread_id
                        .virtual_time = current_virtual_time
                    };
                    strncpy(response.request_id, request_id, 63);
                    response.request_id[63] = '\0';
                    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
                    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                    free(payload_str);
                    send_message_to_router(target_router_id, &response);
                    
                    // 释放缓冲区
                    router_states[target_router_id].packet_buffers[buffer_index].is_used = 0;
                }
                
                printf("[DESD] R%d T%d was blocked on recv and now awakened by PACKET_RECEIVE_EVENT for %s.\n", target_router_id, blocked_ti->thread_id, destination_abstract_address);
            } else if (strcmp(blocked_func, "SELECT_CALL") == 0) {
                // 情况1b：接收方在 select 上等待，需要检查数据包是否匹配监听的fd
                char request_id[64];
                strncpy(request_id, blocked_ti->blocked_on_request_id, 63);
                request_id[63] = '\0';
                
                // 获取新到达数据包对应的socket_fd
                int packet_socket_fd = -1;
                if (buffer_index >= 0 && buffer_index < MAX_PENDING_PACKETS &&
                    router_states[target_router_id].packet_buffers[buffer_index].is_used) {
                    packet_socket_fd = router_states[target_router_id].packet_buffers[buffer_index].socket_fd;
                }
                
                // 检查这个socket_fd是否在select()监听的fd集合中，且监听了POLLIN事件
                int should_wakeup = 0;
                int wakeup_listen_fd = -1;  // 如果通过监听fd唤醒，记录该fd
                
                if (packet_socket_fd >= 0) {
                    // 情况A：数据到达已accept的正常fd
                    for (int i = 0; i < blocked_ti->monitored_fds_count; i++) {
                        if (blocked_ti->monitored_fds[i] == packet_socket_fd &&
                            (blocked_ti->monitored_fds_events[i] & 0x001)) { // POLLIN
                            should_wakeup = 1;
                            break;
                        }
                    }
                } else {
                    // 情况B：数据到达虚拟fd（server端还没accept的连接）
                    // 按照真实内核语义，此时应该把监听fd标为ready（因为有pending connection）
                    // 这样router被唤醒后会先accept，拿到真实fd，然后再recv数据
                    if (router_states[target_router_id].pending_connections_count > 0) {
                        // 检查monitored_fds中是否有监听fd且监听了POLLIN
                        for (int i = 0; i < blocked_ti->monitored_fds_count; i++) {
                            int mon_fd = blocked_ti->monitored_fds[i];
                            int mon_events = blocked_ti->monitored_fds_events[i];
                            
                            // 检查这个fd是否是监听socket
                            for (int j = 0; j < router_states[target_router_id].listen_count; j++) {
                                if (router_states[target_router_id].listening_socket_fds[j] == mon_fd &&
                                    (mon_events & 0x001)) { // POLLIN
                                    should_wakeup = 1;
                                    wakeup_listen_fd = mon_fd;
                                    printf("[DESD] R%d: data arrived on virtual fd %d, mapping to listening fd %d (pending_connections=%d)\n",
                                           target_router_id, packet_socket_fd, mon_fd, 
                                           router_states[target_router_id].pending_connections_count);
                                    break;
                                }
                            }
                            if (should_wakeup) break;
                        }
                    }
                }
                
                if (!should_wakeup) {
                    // 数据包不匹配监听的fd，只加入pending队列，不唤醒路由器
                    router_states[target_router_id].pending_packets_count++;
                    int tail = router_states[target_router_id].pending_buffer_tail;
                    router_states[target_router_id].pending_buffer_indices[tail] = buffer_index;
                    router_states[target_router_id].pending_buffer_tail = (tail + 1) % MAX_PENDING_PACKETS;
                    printf("[DESD] R%d has %d pending packet(s) (data arrived on fd:%d but not monitored by select, pending_connections=%d, currently blocked on SELECT_CALL).\n", 
                           target_router_id, router_states[target_router_id].pending_packets_count, packet_socket_fd,
                           router_states[target_router_id].pending_connections_count);
                    return; // 不唤醒，直接返回
                }
                
                // 数据包匹配监听的fd，取消超时事件并唤醒
                if (blocked_ti->pending_timeout_event_id > 0) {
                    cancel_event(blocked_ti->pending_timeout_event_id);
                    printf("[DESD] Canceled timeout event %lu for R%d (data arrived before timeout).\n",
                           blocked_ti->pending_timeout_event_id, target_router_id);
                    blocked_ti->pending_timeout_event_id = 0;
                }

                blocked_ti->status = RUNNING;
                memset(blocked_ti->blocked_on_request_id, 0, 64);
                memset(blocked_ti->blocked_on_function, 0, 64);
                // 清除保存的monitored_fds信息
                blocked_ti->monitored_fds_count = 0;
                
                // 将 buffer_index 加入 pending 队列，后续的 recv() 才能读取数据
                router_states[target_router_id].pending_packets_count++;
                int tail = router_states[target_router_id].pending_buffer_tail;
                router_states[target_router_id].pending_buffer_indices[tail] = buffer_index;
                router_states[target_router_id].pending_buffer_tail = (tail + 1) % MAX_PENDING_PACKETS;
                
                // 构建就绪 FD 列表
                json_t *ready_fds_array = json_array();
                
                // 如果是通过监听fd唤醒的（数据到达虚拟fd的情况），把监听fd加入ready列表
                if (wakeup_listen_fd >= 0) {
                    json_t *listen_fd_info = json_object();
                    json_object_set_new(listen_fd_info, "fd", json_integer(wakeup_listen_fd));
                    json_object_set_new(listen_fd_info, "revents", json_integer(0x001));  // POLLIN - 新连接待accept
                    json_array_append_new(ready_fds_array, listen_fd_info);
                }
                
                // 同时检查pending packets中是否有其他已accept的fd也有数据
                int head = router_states[target_router_id].pending_buffer_head;
                int count = router_states[target_router_id].pending_packets_count;
                
                for (int i = 0; i < count; i++) {
                    int idx = (head + i) % MAX_PENDING_PACKETS;
                    int buf_idx = router_states[target_router_id].pending_buffer_indices[idx];
                    if (buf_idx >= 0 && buf_idx < MAX_PENDING_PACKETS) {
                        int sock_fd = router_states[target_router_id].packet_buffers[buf_idx].socket_fd;
                        if (sock_fd >= 0) {
                            // 检查是否已添加（包括监听fd）
                            int already_added = 0;
                            size_t array_size = json_array_size(ready_fds_array);
                            for (size_t j = 0; j < array_size; j++) {
                                json_t *existing = json_array_get(ready_fds_array, j);
                                if (json_is_object(existing)) {
                                    int existing_fd = json_integer_value(json_object_get(existing, "fd"));
                                    if (existing_fd == sock_fd) {
                                        already_added = 1;
                                        break;
                                    }
                                }
                            }
                            if (!already_added) {
                                // 使用新格式：{fd, revents}对象
                                json_t *ready_fd_info = json_object();
                                json_object_set_new(ready_fd_info, "fd", json_integer(sock_fd));
                                json_object_set_new(ready_fd_info, "revents", json_integer(0x001));  // POLLIN
                                json_array_append_new(ready_fds_array, ready_fd_info);
                            }
                        }
                    }
                }
                
                // 构建包含就绪 FD 列表的响应
                size_t ready_fds_count = json_array_size(ready_fds_array);
                json_t *response_payload = json_object();
                json_object_set_new(response_payload, "status", json_string("SUCCESS"));
                json_object_set_new(response_payload, "blocked_function", json_string("SELECT"));
                json_object_set_new(response_payload, "message", json_string("Data Available"));
                json_object_set_new(response_payload, "ready_fds", ready_fds_array);
                
                char *response_payload_str = json_dumps(response_payload, JSON_COMPACT);
                json_decref(response_payload);
                
                Message response = {
                    .message_type = DESD_TO_HOOK,
                    .router_id = target_router_id,
                    .thread_id = blocked_ti->thread_id,  // 修复：使用被唤醒线程的 thread_id
                    .virtual_time = current_virtual_time
                };
                strncpy(response.request_id, request_id, 63);
                response.request_id[63] = '\0';
                strncpy(response.payload.json_str, response_payload_str, MAX_MSG_SIZE - 1);
                response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                free(response_payload_str);
                send_message_to_router(target_router_id, &response);
                
                printf("[DESD] R%d T%d was blocked on select and now awakened by PACKET_RECEIVE_EVENT for %s (buffer_index=%d added to pending queue, %zu unique FDs).\n", 
                       target_router_id, blocked_ti->thread_id, destination_abstract_address, buffer_index, ready_fds_count);
            } else {
                // 其他阻塞类型，记录为 pending
                router_states[target_router_id].pending_packets_count++;
                // 将 buffer_index 加入 pending 队列
                int tail = router_states[target_router_id].pending_buffer_tail;
                router_states[target_router_id].pending_buffer_indices[tail] = buffer_index;
                router_states[target_router_id].pending_buffer_tail = (tail + 1) % MAX_PENDING_PACKETS;
                printf("[DESD] R%d has %d pending packet(s) (data arrived but currently blocked on %s).\n", 
                       target_router_id, router_states[target_router_id].pending_packets_count, blocked_func);
            }
        } else {
            // 情况2：接收方还没调用recv()或select()，记录数据已到达
            router_states[target_router_id].pending_packets_count++;
            // 将 buffer_index 加入 pending 队列
            int tail = router_states[target_router_id].pending_buffer_tail;
            router_states[target_router_id].pending_buffer_indices[tail] = buffer_index;
            router_states[target_router_id].pending_buffer_tail = (tail + 1) % MAX_PENDING_PACKETS;
            printf("[DESD] R%d has %d pending packet(s) (data arrived but recv/select not called yet).\n", 
                   target_router_id, router_states[target_router_id].pending_packets_count);
        }
    }
}

void handle_timeout_event(Event event) {
    int router_id = event.router_id;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_timeout_event: Failed to parse payload JSON.\n");
        return;
    }
    
    // 复制到本地缓冲区，防止 json_decref 后访问无效内存
    const char *original_block_request_id_ptr = json_string_value(json_object_get(payload_obj, "original_block_request_id"));
    const char *timeout_type_ptr = json_string_value(json_object_get(payload_obj, "timeout_type"));
    
    char original_block_request_id[64] = {0};
    char timeout_type[64] = {0};
    
    if (original_block_request_id_ptr) {
        strncpy(original_block_request_id, original_block_request_id_ptr, sizeof(original_block_request_id) - 1);
    }
    if (timeout_type_ptr) {
        strncpy(timeout_type, timeout_type_ptr, sizeof(timeout_type) - 1);
    }
    
    json_decref(payload_obj);  // 现在可以安全地释放了

    // 查找匹配 request_id 的阻塞线程
    ThreadInfo *timeout_ti = NULL;
    for (int t = 0; t < MAX_THREADS_PER_ROUTER; t++) {
        ThreadInfo *ti = get_thread_info(router_id, t);
        if (ti && ti->is_active && ti->status == BLOCKED &&
            strcmp(ti->blocked_on_request_id, original_block_request_id) == 0) {
            timeout_ti = ti;
            break;
        }
    }
    
    if (router_id > 0 && router_id <= MAX_ROUTERS && timeout_ti) {
        
        timeout_ti->status = RUNNING;
        memset(timeout_ti->blocked_on_request_id, 0, 64);
        memset(timeout_ti->blocked_on_function, 0, 64);
        
        // 清除超时事件ID
        if (timeout_ti->pending_timeout_event_id == event.event_id) {
            timeout_ti->pending_timeout_event_id = 0;
        }
        
        // 清除保存的monitored_fds信息（超时返回，不再需要）
        timeout_ti->monitored_fds_count = 0;
        
        // 对于 SLEEP_CALL，发送 SUCCESS 响应；对于其他超时，发送 TIMEOUT 响应
        if (timeout_type[0] != '\0' && strcmp(timeout_type, "SLEEP_CALL") == 0) {
            send_success_response(router_id, timeout_ti->thread_id, original_block_request_id, "SLEEP", "Sleep Completed", NULL);
            printf("[DESD-Sleep] R%d sleep completed for request %s at VT=%.3f.\n", router_id, original_block_request_id, current_virtual_time);
        } else {
            send_timeout_response(router_id, timeout_ti->thread_id, original_block_request_id, timeout_type[0] != '\0' ? timeout_type : "UNKNOWN");
            printf("[DESD-Timeout] R%d timed out for request %s (Type: %s) at VT=%.3f.\n", router_id, original_block_request_id, timeout_type[0] != '\0' ? timeout_type : "UNKNOWN", current_virtual_time);
        }
    } else {
        printf("[DESD-Timeout] TIMEOUT_EVENT %lu for R%d ignored (router not blocked on this request %s anymore or already handled).\n",
               event.event_id, router_id, original_block_request_id[0] != '\0' ? original_block_request_id : "UNKNOWN");
    }
}

// Handle CANCEL_BLOCK_REQUEST - 取消之前的阻塞请求（本地fd提前ready时使用）
//
// 【协议说明】CANCEL_BLOCK_REQUEST 是一个 one-way 通知：
//   - libdeshook 端使用 fire-and-forget 方式发送（send_cancel_block_request），不等待响应
//   - DESD 端只做本地状态更新（取消 timeout、清除阻塞状态），不发送 DESD_TO_HOOK 响应
//   - 这样设计是因为：libdeshook 发送 CANCEL 时，应用线程已经因为本地 fd ready 而继续执行了，
//     不会再阻塞等待 DESD 的任何响应；发送响应只会产生"陈旧响应"被后续 RPC 丢弃。
//
// 新架构下，libdeshook可以同时监听desd_fd和本地fd，如果本地fd先ready，
// 需要发送CANCEL_BLOCK_REQUEST取消之前发送给DESD的阻塞请求
void handle_cancel_block_request(Event event) {
    int router_id = event.router_id;
    int thread_id = event.thread_id;
    
    // 从payload中提取request_id
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_cancel_block_request: Failed to parse payload JSON.\n");
        return;
    }
    
    const char *request_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    if (!request_id_ptr) {
        fprintf(stderr, "[DESD ERROR] handle_cancel_block_request: No request_id in payload.\n");
        json_decref(payload_obj);
        return;
    }
    
    char request_id_local[64];
    strncpy(request_id_local, request_id_ptr, 63);
    request_id_local[63] = '\0';
    json_decref(payload_obj);
    
    printf("[DESD-CANCEL] R%d T%d requesting to cancel block request %s at VT=%.6f\n",
           router_id, thread_id, request_id_local, current_virtual_time);
    
    // 获取线程信息
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    if (!ti) {
        fprintf(stderr, "[DESD ERROR] handle_cancel_block_request: Thread R%d T%d not found.\n", router_id, thread_id);
        return;
    }
    
    // 检查线程是否被阻塞在这个request_id上
    if (ti->status == BLOCKED && strcmp(ti->blocked_on_request_id, request_id_local) == 0) {
        // 取消pending的TIMEOUT_EVENT（这会在 cancel_event 中减少 pending_event_count）
        if (ti->pending_timeout_event_id > 0) {
            cancel_event(ti->pending_timeout_event_id);
            printf("[DESD-CANCEL] Canceled pending timeout event %lu for R%d T%d.\n",
                   ti->pending_timeout_event_id, router_id, thread_id);
            ti->pending_timeout_event_id = 0;
        }
        
        // 清理阻塞相关状态
        memset(ti->blocked_on_request_id, 0, 64);
        memset(ti->blocked_on_function, 0, 64);
        ti->monitored_fds_count = 0;
        
        // ===== 方案二核心判断：根据 pending_event_count 决定是否进入 DETACHED_WAITING =====
        // 此时 pending_event_count 的含义：
        //   - 这次 CANCEL 事件本身已在 pop_event 后减过一次
        //   - 刚才 cancel 掉的 TIMEOUT_EVENT 也在 cancel_event 中减过一次
        //   - 所以剩余的 pending_event_count 只包含"除了这次 BLOCK/TIMEOUT 之外的其它事件"
        //     比如：LISTEN, CONNECT_REQUEST, 新的 ROUTER_BLOCK_REQUEST 等
        
        if (ti->pending_event_count == 0) {
            // 没有其它未来事件：线程真的要"脱离 DES 控制"了
            ti->status = RUNNING_DETACHED;
            if (!ti->detached_waiting) {
                ti->detached_waiting = 1;
                ti->detached_since_vt = current_virtual_time;
                num_detached_waiting++;
                if (num_detached_waiting == 1) {
                    first_detached_vt = current_virtual_time;
                }
                printf("[DESD-DETACH] R%d T%d entered DETACHED_WAITING at VT=%.3f (pending_event_count=0, total detached: %d)\n",
                       router_id, thread_id, current_virtual_time, num_detached_waiting);
            }
            printf("[DESD-CANCEL] R%d T%d block request %s cancelled, now RUNNING_DETACHED (no future events).\n",
                   router_id, thread_id, request_id_local);
        } else {
            // 仍有其它事件排队（例如 LISTEN）
            // 说明线程已经/即将通过新的 RPC/阻塞请求重新受 DES 管理
            // 这次 CANCEL 只是"废弃旧的 block + timeout"，不应进入 DETACHED_WAITING
            ti->status = RUNNING;
            printf("[DESD-CANCEL] R%d T%d block request %s cancelled, now RUNNING (pending_event_count=%d, has future events).\n",
                   router_id, thread_id, request_id_local, ti->pending_event_count);
        }
    } else {
        // 线程不在阻塞状态，或者阻塞在其他request_id上
        printf("[DESD-CANCEL] R%d T%d cancel request ignored (status=%d, blocked_on=%s, cancel_req=%s).\n",
               router_id, thread_id, ti->status, ti->blocked_on_request_id, request_id_local);
    }
    
    // 【重要】不发送 DESD_TO_HOOK 响应：
    // CANCEL 是 one-way 通知，libdeshook 端不会等待响应。
    // 如果发送响应，只会在后续 RPC 中被当作"陈旧响应"丢弃，增加不必要的噪音。
}

// --- Helper Functions for sending specific responses ---
void send_success_response(int router_id, int thread_id, const char* request_id, const char* blocked_func, const char* message, const char* connection_id_str) {
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "status", json_string("SUCCESS"));
    if (blocked_func) json_object_set_new(payload_obj, "blocked_function", json_string(blocked_func));
    if (message) json_object_set_new(payload_obj, "message", json_string(message));
    if (connection_id_str) json_object_set_new(payload_obj, "connection_id", json_string(connection_id_str));
    // bytes_transferred不再由desd返回
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    json_decref(payload_obj);

    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
        .thread_id = thread_id,
        .request_id = "",
        .virtual_time = current_virtual_time
    };
    strncpy(response.request_id, request_id, 63);
    response.request_id[63] = '\0';
    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    send_message_to_router(router_id, &response);
}

void send_error_response(int router_id, int thread_id, const char* request_id, const char* error_msg) {
    printf("[DESD-ERROR-RESPONSE] Sending error to R%d T%d (ReqID: %s): %s\n", router_id, thread_id, request_id, error_msg);
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "status", json_string("ERROR"));
    json_object_set_new(payload_obj, "error_message", json_string(error_msg));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    json_decref(payload_obj);

    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
        .thread_id = thread_id,
        .request_id = "",
        .virtual_time = current_virtual_time
    };
    strncpy(response.request_id, request_id, 63);
    response.request_id[63] = '\0';
    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    send_message_to_router(router_id, &response);
}

void send_eagain_response(int router_id, int thread_id, const char* request_id) {
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "status", json_string("EAGAIN"));
    json_object_set_new(payload_obj, "blocked_function", json_string("RECV"));
    json_object_set_new(payload_obj, "message", json_string("No data available (non-blocking)"));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    json_decref(payload_obj);

    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
        .thread_id = thread_id,
        .request_id = "",
        .virtual_time = current_virtual_time
    };
    strncpy(response.request_id, request_id, 63);
    response.request_id[63] = '\0';
    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    send_message_to_router(router_id, &response);
}

void send_timeout_response(int router_id, int thread_id, const char* request_id, const char* timeout_type) {
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "status", json_string("TIMEOUT"));
    json_object_set_new(payload_obj, "blocked_function", json_string(timeout_type));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    json_decref(payload_obj);

    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
        .thread_id = thread_id,
        .request_id = "",
        .virtual_time = current_virtual_time
    };
    strncpy(response.request_id, request_id, 63);
    response.request_id[63] = '\0';
    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    send_message_to_router(router_id, &response);
}

// Handle GET_VIRTUAL_TIME_EVENT - 返回当前虚拟时间
// 注意：此函数现在统一通过主事件循环调用（所有 GETVT 都先入队再处理）
void handle_get_virtual_time_event(Event event) {
    int router_id = event.router_id;
    int thread_id = event.thread_id;
    
    // 从payload中提取request_id
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_get_virtual_time_event: R%d T%d Failed to parse payload JSON.\n",
                router_id, thread_id);
        return;
    }
    
    const char* request_id = json_string_value(json_object_get(payload_obj, "request_id"));
    if (!request_id) {
        fprintf(stderr, "[DESD ERROR] handle_get_virtual_time_event: R%d T%d No request_id in payload.\n",
                router_id, thread_id);
        json_decref(payload_obj);
        return;
    }
    
    char request_id_local[64];
    strncpy(request_id_local, request_id, 63);
    request_id_local[63] = '\0';
    json_decref(payload_obj);

    // 轻量统计 GET_VIRTUAL_TIME_EVENT 处理次数（包含 thread_id 便于调试）
    static unsigned long get_vtime_handle_count = 0;
    get_vtime_handle_count++;
    if (get_vtime_handle_count == 1 ||
        get_vtime_handle_count == 10 ||
        get_vtime_handle_count == 100 ||
        (get_vtime_handle_count % 1000) == 0) {
        printf("[DESD-GETVT-HANDLER] R%d T%d handled GET_VIRTUAL_TIME_EVENT %lu time(s) (ReqID: %s, VT=%.6f, EventID: %lu)\n",
               router_id, thread_id, get_vtime_handle_count, request_id_local, current_virtual_time, event.event_id);
    }
    
    // 构建响应，返回当前虚拟时间
    json_t *resp_payload_obj = json_object();
    json_object_set_new(resp_payload_obj, "status", json_string("SUCCESS"));
    json_object_set_new(resp_payload_obj, "current_virtual_time", json_real(current_virtual_time));
    char *payload_str = json_dumps(resp_payload_obj, JSON_COMPACT);
    json_decref(resp_payload_obj);
    
    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
        .thread_id = event.thread_id,  // 使用 event 中的 thread_id，确保回复给正确的线程
        .virtual_time = current_virtual_time  // 响应消息也携带虚拟时间
    };
    strncpy(response.request_id, request_id_local, 63);
    response.request_id[63] = '\0';
    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    
    send_message_to_router(router_id, &response);
    
    // 回复发送后，将线程状态从 BLOCKED 改回 RUNNING
    // 这样线程可以继续执行，也允许 DESD 在后续事件中与其交互
    pthread_mutex_lock(&router_states_mutex);
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    if (ti && ti->status == BLOCKED) {
        // 只有当线程确实在等这个 GETVT 回复时才改状态
        // 检查 blocked_on_request_id 是否匹配（防止误改其他阻塞原因）
        if (strncmp(ti->blocked_on_request_id, request_id_local, 63) == 0) {
            ti->status = RUNNING;
            ti->blocked_on_request_id[0] = '\0';
        }
    }
    pthread_mutex_unlock(&router_states_mutex);
    
    //GET_VIRTUAL_TIME_EVENT 不打印
    // printf("[DESD] R%d GET_VIRTUAL_TIME_EVENT responded with VT=%.6f (ReqID: %s).\n", 
    //        router_id, current_virtual_time, request_id_local);
}

// === Mutex Hook 相关函数实现 ===

// 查找或创建 mutex 表项
static MutexInfo* find_or_create_mutex(uintptr_t mutex_addr) {
    // 先查找现有的
    for (int i = 0; i < MAX_TRACKED_MUTEXES; i++) {
        if (mutex_table[i].is_active && mutex_table[i].mutex_addr == mutex_addr) {
            return &mutex_table[i];
        }
    }
    
    // 没找到，创建新的
    for (int i = 0; i < MAX_TRACKED_MUTEXES; i++) {
        if (!mutex_table[i].is_active) {
            memset(&mutex_table[i], 0, sizeof(MutexInfo));
            mutex_table[i].mutex_addr = mutex_addr;
            mutex_table[i].is_active = 1;
            mutex_table_count++;
            // printf("[DESD-MUTEX] Created new mutex entry for addr=0x%lx (total tracked: %d)\n",
            //        (unsigned long)mutex_addr, mutex_table_count);
            return &mutex_table[i];
        }
    }
    
    // fprintf(stderr, "[DESD-MUTEX ERROR] Mutex table full! Cannot track mutex 0x%lx\n",
    //         (unsigned long)mutex_addr);
    return NULL;
}

// 处理 MUTEX_LOCK_ACQUIRED：线程成功获取 mutex（单向通知）
static void handle_mutex_lock_acquired(int router_id, int thread_id, uintptr_t mutex_addr) {
    MutexInfo *mi = find_or_create_mutex(mutex_addr);
    if (!mi) return;
    
    mi->owner_router_id = router_id;
    mi->owner_thread_id = thread_id;
    
    // printf("[DESD-MUTEX] R%d T%d acquired mutex 0x%lx\n",
    //        router_id, thread_id, (unsigned long)mutex_addr);
}

// 处理 MUTEX_WAIT_START：线程尝试加锁失败，进入等待状态
static void handle_mutex_wait_start(int router_id, int thread_id, uintptr_t mutex_addr) {
    MutexInfo *mi = find_or_create_mutex(mutex_addr);
    if (!mi) return;
    
    // 将线程加入等待队列
    if (mi->waiter_count >= MAX_MUTEX_WAITERS) {
        // fprintf(stderr, "[DESD-MUTEX ERROR] Waiter queue full for mutex 0x%lx\n",
        //         (unsigned long)mutex_addr);
        return;
    }
    
    mi->waiters[mi->waiter_count].router_id = router_id;
    mi->waiters[mi->waiter_count].thread_id = thread_id;
    mi->waiter_count++;
    
    // 将线程标记为 BLOCKED（等待 mutex）
    pthread_mutex_lock(&router_states_mutex);
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    if (ti) {
        ti->status = BLOCKED;
        // 使用 blocked_on_function 来标记是在等 mutex
        snprintf(ti->blocked_on_function, sizeof(ti->blocked_on_function),
                 "MUTEX_0x%lx", (unsigned long)mutex_addr);
    }
    pthread_mutex_unlock(&router_states_mutex);
    
    // printf("[DESD-MUTEX] R%d T%d waiting on mutex 0x%lx (owner: R%d T%d, waiters: %d)\n",
    //        router_id, thread_id, (unsigned long)mutex_addr,
    //        mi->owner_router_id, mi->owner_thread_id, mi->waiter_count);
}

// 处理 MUTEX_UNLOCK：线程释放 mutex
static void handle_mutex_unlock_msg(int router_id, int thread_id, uintptr_t mutex_addr) {
    MutexInfo *mi = find_or_create_mutex(mutex_addr);
    if (!mi) return;
    
    // 清空 owner
    mi->owner_router_id = 0;
    mi->owner_thread_id = 0;
    
    // printf("[DESD-MUTEX] R%d T%d released mutex 0x%lx (waiters: %d)\n",
    //        router_id, thread_id, (unsigned long)mutex_addr, mi->waiter_count);

    // 如果没有等待者，可以回收该 mutex 表项，避免长时间占用 slot
    if (mi->waiter_count == 0) {
        mi->is_active = 0;
        mi->mutex_addr = 0;
        mi->owner_router_id = 0;
        mi->owner_thread_id = 0;
        if (mutex_table_count > 0) {
            mutex_table_count--;
        }
        return;
    }

    // 如果有等待者，生成 MUTEX_RETRY_EVENT
    if (mi->waiter_count > 0) {
        // 取出队首的等待者（FIFO）
        int waiter_router = mi->waiters[0].router_id;
        int waiter_thread = mi->waiters[0].thread_id;
        
        // 移除队首（左移）
        for (int i = 0; i < mi->waiter_count - 1; i++) {
            mi->waiters[i] = mi->waiters[i + 1];
        }
        mi->waiter_count--;
        
        // 生成 MUTEX_RETRY_EVENT
        Event retry_event = {
            .timestamp = current_virtual_time,  // 方案 A：不推进虚拟时间
            .router_id = waiter_router,
            .thread_id = waiter_thread,
            .event_type = MUTEX_RETRY_EVENT,
            .event_id = generate_event_id()
        };
        
        // 在 payload 中包含 mutex_addr
        json_t *payload_obj = json_object();
        json_object_set_new(payload_obj, "mutex_addr", json_integer((json_int_t)mutex_addr));
        char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
        strncpy(retry_event.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
        retry_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
        free(payload_str);
        json_decref(payload_obj);
        
        push_event(retry_event);
        
        // printf("[DESD-MUTEX] Generated MUTEX_RETRY_EVENT for R%d T%d on mutex 0x%lx at VT=%.3f (EventID: %lu)\n",
        //        waiter_router, waiter_thread, (unsigned long)mutex_addr,
        //        current_virtual_time, retry_event.event_id);
    }
}

// 处理 MUTEX_RETRY_EVENT：唤醒等待的线程
void handle_mutex_retry_event(Event event) {
    int router_id = event.router_id;
    int thread_id = event.thread_id;
    
    // 从 payload 解析 mutex_addr
    uintptr_t mutex_addr = 0;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (payload_obj) {
        json_t *addr_json = json_object_get(payload_obj, "mutex_addr");
        if (addr_json && json_is_integer(addr_json)) {
            mutex_addr = (uintptr_t)json_integer_value(addr_json);
        }
        json_decref(payload_obj);
    }
    
    // printf("[DESD-MUTEX] Processing MUTEX_RETRY_EVENT for R%d T%d on mutex 0x%lx at VT=%.3f\n",
    //        router_id, thread_id, (unsigned long)mutex_addr, current_virtual_time);
    
    // 更新线程状态为 RUNNING
    pthread_mutex_lock(&router_states_mutex);
    ThreadInfo *ti = get_thread_info(router_id, thread_id);
    if (ti) {
        ti->status = RUNNING;
        memset(ti->blocked_on_function, 0, sizeof(ti->blocked_on_function));
    }
    pthread_mutex_unlock(&router_states_mutex);
    
    // 发送 MUTEX_RESUME 给 BIRD
    json_t *resp_payload_obj = json_object();
    json_object_set_new(resp_payload_obj, "status", json_string("MUTEX_RESUME"));
    json_object_set_new(resp_payload_obj, "mutex_addr", json_integer((json_int_t)mutex_addr));
    char *payload_str = json_dumps(resp_payload_obj, JSON_COMPACT);
    json_decref(resp_payload_obj);
    
    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
        .thread_id = thread_id,
        .event_type = MUTEX_RETRY_EVENT,
        .virtual_time = current_virtual_time
    };
    snprintf(response.request_id, sizeof(response.request_id), "mutex_retry_%lu", event.event_id);
    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    
    send_message_to_thread(router_id, thread_id, &response);
    
    // printf("[DESD-MUTEX] Sent MUTEX_RESUME to R%d T%d for mutex 0x%lx\n",
    //        router_id, thread_id, (unsigned long)mutex_addr);
}
