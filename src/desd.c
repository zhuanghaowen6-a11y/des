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
// #include <poll.h> // For poll() function

// --- Global DESD State ---
// 尽管是单线程，但事件队列的访问和条件变量的等待仍需要互斥锁。
//pthread_mutex_t desd_state_mutex = PTHREAD_MUTEX_INITIALIZER;
//pthread_cond_t event_queue_cond = PTHREAD_COND_INITIALIZER; // 用于pop_event在队列为空时等待事件

// Event Queue (min-heap)
#define MAX_EVENTS 1000
Event event_queue[MAX_EVENTS];
int event_queue_size = 0;

// Active Events (for quick lookup and cancellation)
// 现在不再保存实际事件的指针，因为事件是值传递的。用于标记事件是否已被取消。
#define MAX_ACTIVE_EVENTS 100000  // 增大到100000，足够应对长时间运行
// Event* active_events[MAX_ACTIVE_EVENTS]; // 不再需要，因为不再存储事件引用
int event_active_status[MAX_ACTIVE_EVENTS]; // 0: inactive, 1: active (简化)

// Router States
#define MAX_ROUTERS 2 // For r1 and r2
#define MAX_CONNECTIONS_PER_ROUTER 10 // 每个路由器最多支持的连接数

// 连接信息结构
typedef struct {
    int socket_fd;          // 本地 socket fd
    int peer_router_id;     // 对端路由器ID
    int peer_socket_fd;     // 对端 socket fd（用于多连接场景的精确匹配）
    int is_active;          // 连接是否活跃
} ConnectionInfo;

// 数据包缓冲区（用于暂存发送的数据，直到虚拟时间到达）
#define MAX_PENDING_PACKETS 100
#define MAX_PACKET_SIZE 8192

typedef struct {
    char data[MAX_PACKET_SIZE];  // 数据内容（hex编码）
    size_t data_len;             // 数据长度
    int socket_fd;               // 对应的 socket 文件描述符
    int is_used;                 // 是否被使用
} PacketBuffer;

typedef struct {
    int router_id;
    RouterStatus status;
    char blocked_on_request_id[64]; // 路由器正在等待的请求ID
    char blocked_on_function[64]; // 路由器正在等待的函数类型 (RECV_CALL, ACCEPT_CALL, SELECT_CALL等)
    int comm_socket_fd; // 与该路由器libdeshook.so通信的socket FD
    char initial_register_request_id[64]; // 存储初始注册ROUTER_START事件的请求ID
    int pending_packets_count; // 记录有多少个数据包已经在虚拟时间上到达但未被读取
    int pending_connections_count; // 记录有多少个连接已经在虚拟时间上建立但未被accept
    int listen_count; // 监听地址数量
    char listen_addresses[10][256]; // 支持最多 10 个监听地址
    ConnectionInfo connections[MAX_CONNECTIONS_PER_ROUTER]; // 连接表
    unsigned long pending_timeout_event_id; // 记录正在等待的超时事件ID（用于取消）
    PacketBuffer packet_buffers[MAX_PENDING_PACKETS]; // 数据包缓冲区
    int pending_buffer_indices[MAX_PENDING_PACKETS]; // pending packet 的 buffer_index 队列
    int pending_buffer_head; // 队列头
    int pending_buffer_tail; // 队列尾
} RouterInfo;

RouterInfo router_states[MAX_ROUTERS + 1]; // router_id 从 1 开始

// Global control socket for accepting initial libdeshook.so connections
static int desd_listen_fd = -1;

// --- Function Prototypes ---
void init_desd();
void cleanup_desd();

// Event Queue management
void push_event(Event new_event);
Event pop_event();
void cancel_event(unsigned long event_id);
int is_event_queue_empty();

// Router communication
void send_message_to_router(int router_id, const Message* msg);
// 路由器现在是直接通信，这个函数仅用于desd与libdeshook之间的控制消息
int receive_blocking_from_router(int router_id, Message* msg_out);

// Helper to convert EventType to string for logging
const char* event_type_to_string(EventType type);

// DESD event loop
void desd_event_loop();
void handle_event(Event event);

// Event Handlers
void handle_router_start(Event event);
void handle_listen_event(Event event);
void handle_connect_request_event(Event event);
void handle_connection_established_event(Event event);
void handle_connection_info_event(Event event);
void handle_router_block_request(Event event);
void handle_packet_send_event(Event event);
void handle_packet_receive_event(Event event);
void handle_timeout_event(Event event);
void handle_get_virtual_time_event(Event event);

// Helper functions for sending specific responses
void send_success_response(int router_id, const char* request_id, const char* blocked_func, const char* message, const char* connection_id_str);
void send_error_response(int router_id, const char* request_id, const char* error_msg);
void send_timeout_response(int router_id, const char* request_id, const char* timeout_type);
void send_eagain_response(int router_id, const char* request_id);

// Connection management helper functions
int find_router_by_listen_address(const char *address, int caller_router_id);
void register_connection(int router_id, int socket_fd, int peer_router_id, int peer_socket_fd);
int find_peer_router(int router_id, int socket_fd);

// New helper for reading router messages and enqueuing events
// void read_and_enqueue_router_message(int router_id, int comm_fd);


// --- Main ---
int main() {
    // 设置stdout和stderr为无缓冲，确保日志立即输出
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    init_desd();

    // Setup control socket for initial connections
    if (access(DESD_CONTROL_SOCKET_PATH, F_OK) == 0) {
        unlink(DESD_CONTROL_SOCKET_PATH);
    }
    desd_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (desd_listen_fd < 0) {
        perror("[DESD ERROR] socket for listening");
        exit(1);
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DESD_CONTROL_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (bind(desd_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[DESD ERROR] bind for listening");
        close(desd_listen_fd);
        exit(1);
    }
    if (listen(desd_listen_fd, 5) < 0) {
        perror("[DESD ERROR] listen for connections");
        close(desd_listen_fd);
        exit(1);
    }
    printf("[DESD] Listening for initial libdeshook.so connections on %s\n", DESD_CONTROL_SOCKET_PATH);

    // Accept initial router connections and process REGISTER_ROUTER
    int connected_routers = 0;
    while (connected_routers < MAX_ROUTERS) {
        printf("[DESD] Waiting for R%d to connect...\n", connected_routers + 1);
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

        if (register_msg.message_type == HOOK_TO_DESD && register_msg.event_type == ROUTER_START) { // 注册时发送ROUTER_START事件
            int router_id = register_msg.router_id;
            if (router_id > 0 && router_id <= MAX_ROUTERS && router_states[router_id].comm_socket_fd == -1) {
                router_states[router_id].router_id = router_id;
                router_states[router_id].comm_socket_fd = conn_fd;
                router_states[router_id].status = IDLE; // 初始为IDLE
                strncpy(router_states[router_id].initial_register_request_id, register_msg.request_id, 63);
                router_states[router_id].initial_register_request_id[63] = '\0';
                connected_routers++;
                printf("[DESD] Router %d registered with fd %d. (%d/%d).\n", router_id, conn_fd, connected_routers, MAX_ROUTERS);
                // ！！！不再立即发送SUCCESS响应，而是等待事件循环处理ROUTER_START事件
                // send_success_response(router_id, register_msg.request_id, NULL, "Registered", NULL);

                // 调度初始 ROUTER_START 事件 (timestamp = 0)
                Event start_event = {
                    .timestamp = 0.0, // 初始事件在虚拟时间0时发生
                    .router_id = router_id,
                    .event_type = ROUTER_START,
                    .event_id = generate_event_id(),
                    .payload = {"{}"}
                };
                push_event(start_event);

            } else {
                fprintf(stderr, "[DESD ERROR] Invalid or already registered router ID: %d (fd: %d).\n", router_id, conn_fd);
                send_error_response(router_id, register_msg.request_id, "Invalid or Duplicate Router ID");
                close(conn_fd);
            }
        } else {
            fprintf(stderr, "[DESD ERROR] Expected ROUTER_START message for registration, got type %d event %d from fd %d.\n",
                    register_msg.message_type, register_msg.event_type, conn_fd);
            send_error_response(0, "unknown_req", "Invalid Registration Message");
            close(conn_fd);
        }
    }
    close(desd_listen_fd); // Close listening socket after all routers are connected

    printf("[DESD] All routers connected and registered. Starting event loop...\n");
    desd_event_loop(); // Start the single-threaded event loop

    cleanup_desd();
    return 0;
}

// --- DESD Initialization and Cleanup ---
void init_desd() {
    for (int i = 0; i <= MAX_ROUTERS; ++i) {
        router_states[i].router_id = i;
        router_states[i].status = IDLE;
        router_states[i].comm_socket_fd = -1;
        router_states[i].pending_packets_count = 0;
        router_states[i].pending_connections_count = 0;
        router_states[i].listen_count = 0;
        router_states[i].pending_timeout_event_id = 0;
        memset(router_states[i].blocked_on_request_id, 0, sizeof(router_states[i].blocked_on_request_id));
        memset(router_states[i].blocked_on_function, 0, sizeof(router_states[i].blocked_on_function));
        memset(router_states[i].initial_register_request_id, 0, sizeof(router_states[i].initial_register_request_id));
        for (int j = 0; j < 10; j++) {
            memset(router_states[i].listen_addresses[j], 0, sizeof(router_states[i].listen_addresses[j]));
        }
        
        // 初始化连接表
        for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; ++j) {
            router_states[i].connections[j].socket_fd = -1;
            router_states[i].connections[j].peer_router_id = -1;
            router_states[i].connections[j].peer_socket_fd = -1;
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
    // 互斥锁和条件变量在单线程下其实大部分情况下不是必须的，但保持用于事件队列保护和同步
    //pthread_mutex_destroy(&desd_state_mutex);
    //pthread_cond_destroy(&event_queue_cond);
    // pthread_mutex_destroy(&router_states_mutex); // This mutex is not used in handle_connection_established_event
    for (int i = 0; i <= MAX_ROUTERS; ++i) {
        if (router_states[i].comm_socket_fd != -1) {
            close(router_states[i].comm_socket_fd);
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
    
    // 对于TCP地址（包含端口）
    // 1. 先尝试精确IP+端口匹配（优先级高）
    for (int i = 1; i <= MAX_ROUTERS; i++) {
        // 排除发起连接的router自己
        if (i == caller_router_id) {
            continue;
        }
        for (int j = 0; j < router_states[i].listen_count; j++) {
            const char *listen_addr = router_states[i].listen_addresses[j];
            // 精确匹配
            if (strcmp(listen_addr, address) == 0) {
                return i;
            }
        }
    }
    
    // 2. 如果没有精确匹配，尝试0.0.0.0匹配
    for (int i = 1; i <= MAX_ROUTERS; i++) {
        // 排除发起连接的router自己
        if (i == caller_router_id) {
            continue;
        }
        for (int j = 0; j < router_states[i].listen_count; j++) {
            const char *listen_addr = router_states[i].listen_addresses[j];
            // 检查是否监听在0.0.0.0（监听所有接口）
            if (strncmp(listen_addr, "0.0.0.0:", 8) == 0) {
                // 提取监听端口
                const char *listen_port = listen_addr + 7; // 跳过"0.0.0.0"
                // 比较端口是否相同
                if (strcmp(listen_port, target_port) == 0) {
                    return i;  // 0.0.0.0可以匹配任何IP的相同端口
                }
            }
        }
    }
    return -1;  // 未找到
}

// 记录连接映射
void register_connection(int router_id, int socket_fd, int peer_router_id, int peer_socket_fd) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) {
        fprintf(stderr, "[DESD ERROR] register_connection: Invalid router_id %d\n", router_id);
        return;
    }
    
    // 简化：移除内部调试输出
    
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (!router_states[router_id].connections[i].is_active) {
            router_states[router_id].connections[i].socket_fd = socket_fd;
            router_states[router_id].connections[i].peer_router_id = peer_router_id;
            router_states[router_id].connections[i].peer_socket_fd = peer_socket_fd;
            router_states[router_id].connections[i].is_active = 1;
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
    
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (router_states[router_id].connections[i].is_active &&
            router_states[router_id].connections[i].peer_router_id == peer_router_id &&
            router_states[router_id].connections[i].peer_socket_fd == peer_socket_fd) {
            return router_states[router_id].connections[i].socket_fd;
        }
    }
    return -1;
}

// --- Event Queue Management (Min-Heap Implementation) ---
void heapify_up(int idx) {
    while (idx > 0 && event_queue[idx].timestamp < event_queue[(idx - 1) / 2].timestamp) {
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

    if (left_child < event_queue_size && event_queue[left_child].timestamp < event_queue[smallest].timestamp) {
        smallest = left_child;
    }
    if (right_child < event_queue_size && event_queue[right_child].timestamp < event_queue[smallest].timestamp) {
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

    if (event_queue_size >= MAX_EVENTS) {
        fprintf(stderr, "[DESD ERROR] Event queue is full!\n");

        return;
    }
    event_queue[event_queue_size] = new_event;
    heapify_up(event_queue_size);
    
    // 标记事件为活跃状态
    if (new_event.event_id < MAX_ACTIVE_EVENTS) {
        event_active_status[new_event.event_id] = 1;
    }
    event_queue_size++;
    //pthread_cond_signal(&event_queue_cond); // Signal event loop (if it's waiting)

}

Event pop_event() {
    //pthread_mutex_lock(&desd_state_mutex);
    while (event_queue_size == 0) {
        continue;
        // 等待新事件的到来
        printf("[DESD] Event queue is empty. Waiting for initial events...\n");
        //pthread_cond_wait(&event_queue_cond, &desd_state_mutex);
    }

    Event root = event_queue[0];
    // 标记事件为非活跃状态 (只有在事件被取消时才应该设置)
    // if (root.event_id < MAX_ACTIVE_EVENTS) {
    //     event_active_status[root.event_id] = 0; // Clear status
    // }

    event_queue_size--;
    event_queue[0] = event_queue[event_queue_size];
    heapify_down(0);

    //pthread_mutex_unlock(&desd_state_mutex);
    return root;
}

// 取消事件：将事件标记为非活跃状态，事件循环会跳过它
void cancel_event(unsigned long event_id) {
    //pthread_mutex_lock(&desd_state_mutex);
    if (event_id < MAX_ACTIVE_EVENTS) {
        event_active_status[event_id] = 0; // Mark as inactive
        printf("[DESD] Canceled event %lu.\n", event_id);
    }
    //pthread_mutex_unlock(&desd_state_mutex);
}

int is_event_queue_empty() {
    //pthread_mutex_lock(&desd_state_mutex);
    int empty = (event_queue_size == 0);
    //pthread_mutex_unlock(&desd_state_mutex);
    return empty;
}

// --- Router Communication (Single-threaded) ---
// 这些函数用于desd与libdeshook之间的控制消息通信

void send_message_to_router(int router_id, const Message* msg) {
    // pthread_mutex_lock(&router_states_mutex);
    if (router_id > 0 && router_id <= MAX_ROUTERS && router_states[router_id].comm_socket_fd != -1) {
        char *json_str = message_to_json(msg);
        if (!json_str) {
            fprintf(stderr, "[DESD ERROR] Failed to serialize message for R%d.\n", router_id);
            // pthread_mutex_unlock(&router_states_mutex);
            return;
        }
        
        // Allocate space for newline + null terminator
        size_t json_len = strlen(json_str);
        char *json_with_newline = (char*)malloc(json_len + 2); // +1 for '\n', +1 for '\0'
        if (!json_with_newline) {
            fprintf(stderr, "[DESD ERROR] Failed to allocate memory for message to R%d.\n", router_id);
            free(json_str);
            // pthread_mutex_unlock(&router_states_mutex);
            return;
        }
        strcpy(json_with_newline, json_str);
        json_with_newline[json_len] = '\n';
        json_with_newline[json_len + 1] = '\0';
        free(json_str);
        
        if (send(router_states[router_id].comm_socket_fd, json_with_newline, strlen(json_with_newline), 0) < 0) {
            perror("[DESD ERROR] send_message_to_router");
        }
        free(json_with_newline);
    } else {
        fprintf(stderr, "[DESD ERROR] Cannot send message to R%d: Not connected or invalid ID.\n", router_id);
    }
    // pthread_mutex_unlock(&router_states_mutex);
}

int receive_blocking_from_router(int router_id, Message* msg_out) {
    // pthread_mutex_lock(&router_states_mutex); // Lock router states
    int comm_fd = -1;
    if (router_id > 0 && router_id <= MAX_ROUTERS) {
        comm_fd = router_states[router_id].comm_socket_fd;
    }
    // pthread_mutex_unlock(&router_states_mutex);

    if (comm_fd == -1) {
        fprintf(stderr, "[DESD ERROR] Router %d communication FD not found for blocking receive.\n", router_id);
        return 0; // Failure
    }

    // 设置超时，防止路由器长时间不响应导致死锁
    // struct timeval tv;
    // tv.tv_sec = 5; // 5秒超时
    // tv.tv_usec = 0;
    // setsockopt(comm_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    char buffer[MAX_MSG_SIZE];
    ssize_t bytes_received = recv(comm_fd, buffer, MAX_MSG_SIZE - 1, 0);

    // 恢复socket选项
    // tv.tv_sec = 0; 
    // setsockopt(comm_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            fprintf(stderr, "[DESD ERROR] Router %d disconnected during blocking receive.\n", router_id);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "[DESD ERROR] Router %d timed out waiting for next event.\n", router_id);
            } else {
                perror("[DESD ERROR] receive_blocking_from_router");
            }
        }
        // pthread_mutex_lock(&router_states_mutex);
        router_states[router_id].comm_socket_fd = -1; // Mark as disconnected
        router_states[router_id].status = IDLE; // 设置为IDLE
        // pthread_mutex_unlock(&router_states_mutex);
        return 0; // Failure
    }
    buffer[bytes_received] = '\0';

    json_to_message(buffer, msg_out);
    //GET_VIRTUAL_TIME_EVENT 不打印
    if (msg_out->event_type != GET_VIRTUAL_TIME_EVENT) {
        printf("[DESD] Received message (Type: %s, Event: %s, ReqID: %s) from R%d.\n",
               msg_out->message_type == HOOK_TO_DESD ? "HOOK_TO_DESD" : "DESD_TO_HOOK",
               event_type_to_string(msg_out->event_type), msg_out->request_id, router_id);
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
        case GET_VIRTUAL_TIME_EVENT: return "GET_VIRTUAL_TIME_EVENT";
        default: return "UNKNOWN_EVENT";
    }
}

// --- DESD Event Loop (Single-threaded) ---
void desd_event_loop() {
    while (1) {
        Event current_event = pop_event(); // 如果队列为空，这里会阻塞

        //pthread_mutex_lock(&desd_state_mutex);
        // 检查事件是否已被取消
        if (current_event.event_id >= MAX_ACTIVE_EVENTS || event_active_status[current_event.event_id] == 0) {
            //pthread_mutex_unlock(&desd_state_mutex);
            printf("[DESD] Skipped cancelled or inactive event %lu (Type: %s).\n", current_event.event_id, event_type_to_string(current_event.event_type));
            continue;
        }

        if (current_event.timestamp < current_virtual_time) {
            printf("[DESD WARNING] Event %lu (Type: %s) at %.3f is in the past (Current VT: %.3f). Skipping.\n",
                   current_event.event_id, event_type_to_string(current_event.event_type),
                   current_event.timestamp, current_virtual_time);
            //pthread_mutex_unlock(&desd_state_mutex);
            continue;
        }
        current_virtual_time = current_event.timestamp;
        //GET_VIRTUAL_TIME_EVENT 不打印
        if (current_event.event_type != GET_VIRTUAL_TIME_EVENT) {
            printf("[DESD] Processing event %s for R%d at VT=%.3f (EventID: %lu)\n",
                   event_type_to_string(current_event.event_type),
                   current_event.router_id, current_virtual_time, current_event.event_id);
        }
        
        // 记录路由器在处理事件前的状态
        // pthread_mutex_lock(&router_states_mutex);
        RouterInfo *router_info = &router_states[current_event.router_id];
        RouterStatus status_before_event = router_info->status;
        // pthread_mutex_unlock(&router_states_mutex);
        
        // 处理事件
        handle_event(current_event);

        // 等待路由器下一个事件的条件：
        // 1. 路由器刚启动 (IDLE → RUNNING)
        // 2. 路由器被解除阻塞 (BLOCKED → RUNNING)
        // 3. 处理了路由器主动发起的事件（PACKET_SEND_EVENT, CONNECT_REQUEST_EVENT, ROUTER_BLOCK_REQUEST）
        //    这些事件是路由器发送给 desd 的，处理完后路由器会继续执行并发送下一个事件
        // 不等待的情况：
        // 4. 处理 desd 内部调度的事件（如 PACKET_RECEIVE_EVENT, TIMEOUT_EVENT, CONNECTION_ESTABLISHED_EVENT）
        //    且路由器本来就在运行 (RUNNING → RUNNING)
        // pthread_mutex_lock(&router_states_mutex);
        RouterStatus status_after_event = router_info->status;
        
        // 路由器主动发起的事件类型
        int is_router_initiated_event = (
            current_event.event_type == PACKET_SEND_EVENT ||
            current_event.event_type == CONNECT_REQUEST_EVENT ||
            current_event.event_type == ROUTER_BLOCK_REQUEST ||
            current_event.event_type == GET_VIRTUAL_TIME_EVENT
        );
        
        int should_wait_for_next_event = (
            (status_before_event == IDLE && status_after_event == RUNNING) ||    // 路由器启动
            (status_before_event == BLOCKED && status_after_event == RUNNING) || // 路由器解除阻塞
            (is_router_initiated_event && status_after_event == RUNNING)         // 处理了路由器主动发起的事件
        );
        // pthread_mutex_unlock(&router_states_mutex);

        if (should_wait_for_next_event) {
            //printf("[DESD] R%d is now RUNNING. Waiting for its next event...\n", current_event.router_id);
            
            // 循环接收路由器的事件，直到收到一个需要放入队列的事件
            while (1) {
                Message next_msg_from_router;
                int received_ok = receive_blocking_from_router(current_event.router_id, &next_msg_from_router);

                if (!received_ok) {
                    fprintf(stderr, "[DESD ERROR] R%d failed to respond with next event after unblock.\n", current_event.router_id);
                    break;
                }

                if (next_msg_from_router.message_type != HOOK_TO_DESD) {
                    fprintf(stderr, "[DESD ERROR] R%d sent a non-HOOK_TO_DESD message after unblock: Type %d.\n",
                            current_event.router_id, next_msg_from_router.message_type);
                    break;
                }

                // 检查是否是需要立即处理的瞬时事件（如 LISTEN_EVENT, CONNECTION_INFO_EVENT）
                if (next_msg_from_router.event_type == LISTEN_EVENT) {
                    // 立即处理 LISTEN_EVENT，不放入事件队列
                    Event listen_event = {
                        .timestamp = current_virtual_time,
                        .router_id = next_msg_from_router.router_id,
                        .event_type = LISTEN_EVENT,
                        .event_id = generate_event_id(),
                        .payload = next_msg_from_router.payload
                    };
                    listen_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                    printf("[DESD] R%d sent LISTEN_EVENT (ReqID: %s), processing immediately at VT %.3f.\n",
                           current_event.router_id, next_msg_from_router.request_id, current_virtual_time);
                    handle_listen_event(listen_event);
                    // 继续循环，等待下一个事件
                    continue;
                } else if (next_msg_from_router.event_type == CONNECTION_INFO_EVENT) {
                    // 立即处理 CONNECTION_INFO_EVENT，不放入事件队列
                    Event conn_info_event = {
                        .timestamp = current_virtual_time,
                        .router_id = next_msg_from_router.router_id,
                        .event_type = CONNECTION_INFO_EVENT,
                        .event_id = generate_event_id(),
                        .payload = next_msg_from_router.payload
                    };
                    conn_info_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                    printf("[DESD] R%d sent CONNECTION_INFO_EVENT (ReqID: %s), processing immediately at VT %.3f.\n",
                           current_event.router_id, next_msg_from_router.request_id, current_virtual_time);
                    handle_connection_info_event(conn_info_event);
                    // 继续循环，等待下一个事件
                    continue;
                }

                // 其他事件放入队列
                // 对于 PACKET_SEND_EVENT，增加一个小的延迟，确保在 accept() 完成后处理
                // 这样可以避免 send() 时服务器端的 socket_fd 还未注册的问题
                double event_timestamp = current_virtual_time;
                if (next_msg_from_router.event_type == PACKET_SEND_EVENT) {
                    event_timestamp = current_virtual_time + 0.002; // 延迟 2ms，确保晚于 accept (0.001ms)
                } else if (next_msg_from_router.event_type == GET_VIRTUAL_TIME_EVENT) {
                    event_timestamp = current_virtual_time + 0; // 给GET_VIRTUAL_TIME_EVENT加上10ms，推进虚拟时间
                }
                
                Event next_event = {
                    .timestamp = event_timestamp,
                    .router_id = next_msg_from_router.router_id,
                    .event_type = next_msg_from_router.event_type,
                    .event_id = generate_event_id(),
                    .payload = next_msg_from_router.payload
                };
                next_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                push_event(next_event);
                // GET_VIRTUAL_TIME_EVENT 不打印
                if(next_msg_from_router.event_type != GET_VIRTUAL_TIME_EVENT){
                    printf("[DESD] R%d generated event %s (ReqID: %s) at VT %.3f (EventID: %lu).\n",
                    current_event.router_id, event_type_to_string(next_event.event_type),
                    next_msg_from_router.request_id, event_timestamp, next_event.event_id);
                }
                break;
            }
        }
        // pthread_mutex_unlock(&desd_state_mutex); // Unlock desd_state_mutex
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
        default:
            fprintf(stderr, "[DESD WARNING] Unknown event type in handle_event: %d\n", event.event_type);
            break;
    }
}

// --- Event Handlers ---
void handle_router_start(Event event) {
    if (event.router_id > 0 && event.router_id <= MAX_ROUTERS) {
        router_states[event.router_id].status = RUNNING;
        printf("[DESD] R%d is now RUNNING.\n", event.router_id);
        
        // 使用存储的初始注册请求ID来作为响应ID
        send_success_response(event.router_id, router_states[event.router_id].initial_register_request_id, "REGISTER", "Registered", NULL);
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
    
    json_decref(payload_obj);

    if (router_id > 0 && router_id <= MAX_ROUTERS) {
        // 检查是否已达到监听地址上限
        if (router_states[router_id].listen_count >= 10) {
            fprintf(stderr, "[DESD ERROR] R%d listen address limit reached (max: 10)!\n", router_id);
            send_error_response(router_id, request_id, "Too many listen addresses");
            return;
        }
        
        // 检查是否已存在该地址（避免重复）
        for (int i = 0; i < router_states[router_id].listen_count; i++) {
            if (strcmp(router_states[router_id].listen_addresses[i], listen_address_local) == 0) {
                printf("[DESD] R%d already listening on %s, ignoring duplicate.\n", 
                       router_id, listen_address_local);
                send_success_response(router_id, request_id, "LISTEN", "Already Listening", NULL);
                return;
            }
        }
        
        // 添加新的监听地址
        int index = router_states[router_id].listen_count;
        strncpy(router_states[router_id].listen_addresses[index], listen_address_local, 255);
        router_states[router_id].listen_addresses[index][255] = '\0';
        router_states[router_id].listen_count++;
        
        // LISTEN事件信息已在下面显示，移除DEBUG输出
        printf("[DESD] R%d is now listening on %s (total: %d address%s).\n", 
               router_id, listen_address_local, 
               router_states[router_id].listen_count,
               router_states[router_id].listen_count > 1 ? "es" : "");
        
        // listen() 是非阻塞的，立即返回成功
        send_success_response(router_id, request_id, "LISTEN", "Listen Successful", NULL);
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

    if (router_id > 0 && router_id <= MAX_ROUTERS) {
        router_states[router_id].status = BLOCKED;
        strncpy(router_states[router_id].blocked_on_request_id, request_id, 63);
        router_states[router_id].blocked_on_request_id[63] = '\0';
        strncpy(router_states[router_id].blocked_on_function, "CONNECT_CALL", 63);
        router_states[router_id].blocked_on_function[63] = '\0';

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
            send_error_response(router_id, request_id, "Connection refused: Target not listening");
            router_states[router_id].status = RUNNING; // 错误发生，解除阻塞
            memset(router_states[router_id].blocked_on_request_id, 0, sizeof(router_states[router_id].blocked_on_request_id));
            memset(router_states[router_id].blocked_on_function, 0, sizeof(router_states[router_id].blocked_on_function));
            return;
        }
        
        // 额外的自连接检测（理论上不应该发生，因为find函数已经排除了）
        if (target_router_id == router_id) {
            fprintf(stderr, "[DESD ERROR] Self-connection detected! R%d trying to connect to itself at %s\n",
                    router_id, destination_abstract_address_local);
            send_error_response(router_id, request_id, "Connection refused: Cannot connect to self");
            router_states[router_id].status = RUNNING;
            memset(router_states[router_id].blocked_on_request_id, 0, sizeof(router_states[router_id].blocked_on_request_id));
            memset(router_states[router_id].blocked_on_function, 0, sizeof(router_states[router_id].blocked_on_function));
            return;
        }
        
        // 简化：只显示连接结果，不显示匹配过程
        
        // 为目标路由器（服务器）调度 CONNECTION_ESTABLISHED_EVENT
        json_t *target_payload_obj = json_object();
        json_object_set_new(target_payload_obj, "client_router_id", json_integer(router_id));
        json_object_set_new(target_payload_obj, "server_router_id", json_integer(target_router_id));
        json_object_set_new(target_payload_obj, "client_socket_fd", json_integer(client_socket_fd));
        json_object_set_new(target_payload_obj, "request_id", json_string(request_id));
        char *target_payload_str = json_dumps(target_payload_obj, JSON_COMPACT);
        json_decref(target_payload_obj);

        Event conn_est_event = {
            .timestamp = connection_established_time,
            .router_id = target_router_id, // 目标路由器
            .event_type = CONNECTION_ESTABLISHED_EVENT,
            .event_id = generate_event_id()
        };
        strncpy(conn_est_event.payload.json_str, target_payload_str, MAX_MSG_SIZE - 1);
        conn_est_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
        free(target_payload_str);
        push_event(conn_est_event);
        
        printf("[DESD] R%d sent CONNECT_REQUEST (fd:%d). Scheduled CONNECTION_ESTABLISHED_EVENT for R%d (server) at VT=%.3f and R%d (client) at VT=%.3f.\n",
               router_id, client_socket_fd, target_router_id, connection_established_time, router_id, connection_established_time - 0.001);

        // 同时为发起连接的客户端路由器调度一个CONNECTION_ESTABLISHED_EVENT
        // 重要：客户端事件略早于服务器事件，确保 real_connect() 先于 real_accept() 执行
        // 这样 accept() 不会阻塞在内核
        json_t *source_payload_obj = json_object();
        json_object_set_new(source_payload_obj, "client_router_id", json_integer(router_id));
        json_object_set_new(source_payload_obj, "server_router_id", json_integer(target_router_id));
        json_object_set_new(source_payload_obj, "client_socket_fd", json_integer(client_socket_fd));
        json_object_set_new(source_payload_obj, "request_id", json_string(request_id));
        char *source_payload_str = json_dumps(source_payload_obj, JSON_COMPACT);
        json_decref(source_payload_obj);

        Event source_conn_est_event = {
            .timestamp = connection_established_time - 0.001, // 客户端事件早 1ms，确保先执行 connect
            .router_id = router_id, // 发起连接的路由器
            .event_type = CONNECTION_ESTABLISHED_EVENT,
            .event_id = generate_event_id()
        };
        strncpy(source_conn_est_event.payload.json_str, source_payload_str, MAX_MSG_SIZE - 1);
        source_conn_est_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
        free(source_payload_str);
        push_event(source_conn_est_event);
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

    json_decref(payload_obj);

    // 判断这个事件是发给连接发起方（CONNECT）还是接收方（ACCEPT）
    int is_connector = (router_id == client_router_id);
    
    if (router_id > 0 && router_id <= MAX_ROUTERS) {
        // 对于 CONNECT 方（发起连接的路由器）
        if (is_connector) {
            if (router_states[router_id].status == BLOCKED &&
                strcmp(router_states[router_id].blocked_on_function, "CONNECT_CALL") == 0) {
                
                // 保存路由器自己的 request_id
                char router_request_id[64];
                strncpy(router_request_id, router_states[router_id].blocked_on_request_id, 63);
                router_request_id[63] = '\0';
                
                // 注册客户端的连接映射（暂时用 -1 作为对端 socket_fd 的占位符）
                // 完整的双向映射将在服务器 accept() 并发送 CONNECTION_INFO_EVENT 后建立
                register_connection(client_router_id, client_socket_fd, server_router_id, -1);
                
                router_states[router_id].status = RUNNING;
                memset(router_states[router_id].blocked_on_request_id, 0, sizeof(router_states[router_id].blocked_on_request_id));
                memset(router_states[router_id].blocked_on_function, 0, sizeof(router_states[router_id].blocked_on_function));

                printf("[DESD] R%d (client) connect() completed with R%d.\n", router_id, server_router_id);
                send_success_response(router_id, router_request_id, "CONNECT", "Connection Established", NULL);
            } else {
                printf("[DESD WARNING] R%d received CONNECTION_ESTABLISHED_EVENT but not blocked on connect (currently %s).\n", 
                       router_id, 
                       router_states[router_id].status == BLOCKED ? router_states[router_id].blocked_on_function : "not blocked");
            }
        }
        // 对于 ACCEPT 方（接收连接的路由器）
        else {
            if (router_states[router_id].status == BLOCKED &&
                strcmp(router_states[router_id].blocked_on_function, "ACCEPT_CALL") == 0) {
                
                // 保存路由器自己的 request_id
                char router_request_id[64];
                strncpy(router_request_id, router_states[router_id].blocked_on_request_id, 63);
                router_request_id[63] = '\0';
                
                // 服务器端的连接映射将在 CONNECTION_INFO_EVENT 中注册
                // （因为现在还不知道 accept() 返回的 socket_fd）
                
                router_states[router_id].status = RUNNING;
                memset(router_states[router_id].blocked_on_request_id, 0, sizeof(router_states[router_id].blocked_on_request_id));
                memset(router_states[router_id].blocked_on_function, 0, sizeof(router_states[router_id].blocked_on_function));

                printf("[DESD] R%d (server) accept() completed due to connection from R%d.\n", 
                       router_id, client_router_id);
                send_success_response(router_id, router_request_id, "ACCEPT", "Connection Established", NULL);
            } else {
                // 连接已经建立，但路由器还没有调用 accept()，记录为 pending connection
                router_states[router_id].pending_connections_count++;
                printf("[DESD] R%d has %d pending connection(s) (connection established but accept not called yet, currently %s).\n", 
                       router_id, router_states[router_id].pending_connections_count,
                       router_states[router_id].status == BLOCKED ? router_states[router_id].blocked_on_function : "not blocked");
            }
        }
    }
}

void handle_connection_info_event(Event event) {
    int router_id = event.router_id;
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_connection_info_event: Failed to parse payload JSON.\n");
        return;
    }

    const char *req_id_ptr = json_string_value(json_object_get(payload_obj, "request_id"));
    char request_id[64] = {0};
    if (req_id_ptr) {
        strncpy(request_id, req_id_ptr, 63);
    }
    
    int socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));
    json_t *is_client_json = json_object_get(payload_obj, "is_client");
    int is_client = is_client_json && json_is_true(is_client_json);
    
    if (is_client) {
        // 客户端发送的 CONNECTION_INFO_EVENT
        // 查找客户端是否已经有一个到服务端的连接（在 CONNECTION_ESTABLISHED_EVENT 中创建的）
        int updated = 0;
        int peer_router_id = -1;
        
        // 首先查找客户端自己的连接表，看是否已经有连接记录
        for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
            if (router_states[router_id].connections[i].is_active &&
                router_states[router_id].connections[i].socket_fd == socket_fd) {
                // 找到了这个连接，现在需要找到对端的 socket_fd
                peer_router_id = router_states[router_id].connections[i].peer_router_id;
                
                // 在服务端的连接表中查找对应的连接
                for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                    if (router_states[peer_router_id].connections[j].is_active &&
                        router_states[peer_router_id].connections[j].peer_router_id == router_id &&
                        router_states[peer_router_id].connections[j].peer_socket_fd == -1) {
                        // 找到了服务端的连接，它的 peer_socket_fd 还是 -1
                        // 更新双向映射
                        int peer_socket_fd = router_states[peer_router_id].connections[j].socket_fd;
                        router_states[router_id].connections[i].peer_socket_fd = peer_socket_fd;
                        router_states[peer_router_id].connections[j].peer_socket_fd = socket_fd;
                        
                        printf("[DESD] R%d (client) updated connection: fd %d <-> R%d (server) fd %d.\n", 
                               router_id, socket_fd, peer_router_id, peer_socket_fd);
                        updated = 1;
                        break;
                    }
                }
                break;
            }
        }
        
        if (!updated) {
            // 如果没有找到已有的连接，可能是时序问题，创建新连接
            peer_router_id = (router_id == 1) ? 2 : 1;
            register_connection(router_id, socket_fd, peer_router_id, -1);
            printf("[DESD] R%d (client) registered connection: fd %d <-> R%d (server) [peer fd unknown].\n", 
                   router_id, socket_fd, peer_router_id);
        }
    } else {
        // 服务端发送的 CONNECTION_INFO_EVENT
        int client_router_id = (router_id == 1) ? 2 : 1;
        
        // 查找客户端是否已经有一个连接记录指向当前服务端
        // 从后往前查找，因为新连接总是添加到后面
        int peer_socket_fd = -1;
        for (int i = MAX_CONNECTIONS_PER_ROUTER - 1; i >= 0; i--) {
            if (router_states[client_router_id].connections[i].is_active &&
                router_states[client_router_id].connections[i].peer_router_id == router_id &&
                router_states[client_router_id].connections[i].peer_socket_fd == -1) {
                // 找到了客户端的连接，更新双向映射
                peer_socket_fd = router_states[client_router_id].connections[i].socket_fd;
                router_states[client_router_id].connections[i].peer_socket_fd = socket_fd;
                break;
            }
        }
        
        // 注册服务器端的连接
        register_connection(router_id, socket_fd, client_router_id, peer_socket_fd);
        
        if (peer_socket_fd != -1) {
            printf("[DESD] R%d (server) registered connection: fd %d <-> R%d (client) fd %d.\n", 
                   router_id, socket_fd, client_router_id, peer_socket_fd);
        } else {
            printf("[DESD] R%d (server) registered connection: fd %d <-> R%d (client) [peer fd unknown].\n", 
                   router_id, socket_fd, client_router_id);
        }
    }
    
    json_decref(payload_obj);
    send_success_response(router_id, request_id, "CONNECTION_INFO", "Connection Info Recorded", NULL);
}

void handle_router_block_request(Event event) {
    int router_id = event.router_id;
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

    // 解析 nonblocking 标志（用于 RECV_CALL）
    int is_nonblocking = 0;
    json_t *nonblocking_obj = json_object_get(payload_obj, "nonblocking");
    if (nonblocking_obj && json_is_boolean(nonblocking_obj)) {
        is_nonblocking = json_boolean_value(nonblocking_obj);
    }

    json_decref(payload_obj);

    if (router_id > 0 && router_id <= MAX_ROUTERS) {
        // 特殊处理 RECV_CALL：检查是否有 pending 数据包
        if (strcmp(blocked_func_str_local, "RECV_CALL") == 0) {
            if (router_states[router_id].pending_packets_count > 0) {
                // 数据已在虚拟时间上到达，立即唤醒
                router_states[router_id].pending_packets_count--;
                router_states[router_id].status = RUNNING;
                
                // 从队列中取出 buffer_index
                int head = router_states[router_id].pending_buffer_head;
                int buffer_index = router_states[router_id].pending_buffer_indices[head];
                router_states[router_id].pending_buffer_head = (head + 1) % MAX_PENDING_PACKETS;
                
                // 发送数据给路由器
                if (buffer_index >= 0 && buffer_index < MAX_PENDING_PACKETS &&
                    router_states[router_id].packet_buffers[buffer_index].is_used) {
                    
                    json_t *resp_payload = json_object();
                    json_object_set_new(resp_payload, "status", json_string("SUCCESS"));
                    json_object_set_new(resp_payload, "blocked_function", json_string("RECV"));
                    json_object_set_new(resp_payload, "message", json_string("Packet Available"));
                    json_object_set_new(resp_payload, "packet_data", 
                                      json_string(router_states[router_id].packet_buffers[buffer_index].data));
                    
                    char *payload_str = json_dumps(resp_payload, JSON_COMPACT);
                    json_decref(resp_payload);
                    
                    Message response = {
                        .message_type = DESD_TO_HOOK,
                        .router_id = router_id,
                        .virtual_time = current_virtual_time
                    };
                    strncpy(response.request_id, request_id_local, 63);
                    response.request_id[63] = '\0';
                    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
                    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                    free(payload_str);
                    send_message_to_router(router_id, &response);
                    
                    // 释放缓冲区
                    router_states[router_id].packet_buffers[buffer_index].is_used = 0;
                }
                
                printf("[DESD] R%d recv() immediately unblocked (had %d pending packet(s)).\n", 
                       router_id, router_states[router_id].pending_packets_count + 1);
            } else {
                // 数据还没到达
                if (is_nonblocking) {
                    // 非阻塞模式：立即返回 EAGAIN
                    router_states[router_id].status = RUNNING;
                    send_eagain_response(router_id, request_id_local);
                    printf("[DESD] R%d recv() on non-blocking socket, no data available, returning EAGAIN.\n", router_id);
                } else {
                    // 阻塞模式：保持阻塞
                    router_states[router_id].status = BLOCKED;
                    strncpy(router_states[router_id].blocked_on_request_id, request_id_local, 63);
                    router_states[router_id].blocked_on_request_id[63] = '\0';
                    strncpy(router_states[router_id].blocked_on_function, blocked_func_str_local, 63);
                    router_states[router_id].blocked_on_function[63] = '\0';
                    printf("[DESD] R%d blocked on %s (ReqID: %s) - no pending packets.\n",
                           router_id, blocked_func_str_local, request_id_local);
                }
            }
        } else if (strcmp(blocked_func_str_local, "ACCEPT_CALL") == 0) {
            // 特殊处理 ACCEPT_CALL：检查是否有 pending 连接
            if (router_states[router_id].pending_connections_count > 0) {
                // 连接已在虚拟时间上建立，立即唤醒
                router_states[router_id].pending_connections_count--;
                router_states[router_id].status = RUNNING;
                send_success_response(router_id, request_id_local, "ACCEPT", "Connection Available", NULL);
                printf("[DESD] R%d accept() immediately unblocked (had %d pending connection(s)).\n", 
                       router_id, router_states[router_id].pending_connections_count + 1);
            } else {
                // 连接还没建立，保持阻塞
                router_states[router_id].status = BLOCKED;
                strncpy(router_states[router_id].blocked_on_request_id, request_id_local, 63);
                router_states[router_id].blocked_on_request_id[63] = '\0';
                strncpy(router_states[router_id].blocked_on_function, blocked_func_str_local, 63);
                router_states[router_id].blocked_on_function[63] = '\0';
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
                    
                    // 检查 POLLIN：是否有数据可读
                    if (events & 0x001) {  // POLLIN = 0x001
                        // 遍历 pending buffer 队列，查找匹配的 socket_fd
                        int head = router_states[router_id].pending_buffer_head;
                        int count = router_states[router_id].pending_packets_count;
                        
                        for (int j = 0; j < count; j++) {
                            int idx = (head + j) % MAX_PENDING_PACKETS;
                            int buf_idx = router_states[router_id].pending_buffer_indices[idx];
                            if (buf_idx >= 0 && buf_idx < MAX_PENDING_PACKETS) {
                                if (router_states[router_id].packet_buffers[buf_idx].socket_fd == fd) {
                                    revents |= 0x001;  // POLLIN
                                    break;
                                }
                            }
                        }
                    }
                    
                    // 检查 POLLOUT：socket 是否可写
                    if (events & 0x004) {  // POLLOUT = 0x004
                        // 查找该 fd 的连接信息
                        int connection_found = 0;
                        for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                            if (router_states[router_id].connections[j].is_active &&
                                router_states[router_id].connections[j].socket_fd == fd) {
                                // 连接存在且活跃，socket 可写
                                revents |= 0x004;  // POLLOUT
                                connection_found = 1;
                                break;
                            }
                        }
                        
                        // 如果连接不存在或已断开，标记 POLLHUP
                        if (!connection_found && (events & 0x004)) {
                            revents |= 0x010;  // POLLHUP
                        }
                    }
                    
                    // 检查 POLLERR/POLLHUP：连接错误或断开
                    // （这里可以扩展更多错误检测逻辑）
                    // 当前简化实现：如果连接不活跃，则标记为断开
                    for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; j++) {
                        if (router_states[router_id].connections[j].socket_fd == fd) {
                            if (!router_states[router_id].connections[j].is_active) {
                                revents |= 0x010;  // POLLHUP - 连接已断开
                            }
                            break;
                        }
                    }
                    
                    // 如果有任何事件就绪，添加到结果列表
                    if (revents != 0) {
                        json_t *ready_fd_info = json_object();
                        json_object_set_new(ready_fd_info, "fd", json_integer(fd));
                        json_object_set_new(ready_fd_info, "revents", json_integer(revents));
                        json_array_append_new(ready_fds_array, ready_fd_info);
                        has_ready_fds = 1;
                    }
                }
            }
            
            if (payload_obj2) {
                json_decref(payload_obj2);
            }
            
            // 如果有就绪的 FD，立即唤醒
            if (has_ready_fds) {
                router_states[router_id].status = RUNNING;
                
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
            } else {
                // 释放空的 ready_fds_array
                json_decref(ready_fds_array);
                // 数据未就绪，阻塞并可能注册超时事件
                router_states[router_id].status = BLOCKED;
                strncpy(router_states[router_id].blocked_on_request_id, request_id_local, 63);
                router_states[router_id].blocked_on_request_id[63] = '\0';
                strncpy(router_states[router_id].blocked_on_function, blocked_func_str_local, 63);
                router_states[router_id].blocked_on_function[63] = '\0';
                
                if (timeout_ms > 0) {
                    // 注册超时事件
                    double timeout_seconds = timeout_ms / 1000.0;
                    double timeout_time = current_virtual_time + timeout_seconds;
                    
                    Event timeout_event = {
                        .timestamp = timeout_time,
                        .router_id = router_id,
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
                    router_states[router_id].pending_timeout_event_id = timeout_event.event_id;
                    
                    printf("[DESD] R%d blocked on %s (ReqID: %s) with %dms timeout. Registered TIMEOUT_EVENT (ID: %lu) at VT=%.3f.\n",
                           router_id, blocked_func_str_local, request_id_local, timeout_ms, timeout_event.event_id, timeout_time);
                } else {
                    printf("[DESD] R%d blocked on %s (ReqID: %s) - no timeout.\n",
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
            
            router_states[router_id].status = BLOCKED;
            strncpy(router_states[router_id].blocked_on_request_id, request_id_local, 63);
            router_states[router_id].blocked_on_request_id[63] = '\0';
            strncpy(router_states[router_id].blocked_on_function, blocked_func_str_local, 63);
            router_states[router_id].blocked_on_function[63] = '\0';
            
            // 注册唤醒事件（类似超时事件）
            double wakeup_time = current_virtual_time + sleep_seconds;
            
            Event wakeup_event = {
                .timestamp = wakeup_time,
                .router_id = router_id,
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
            router_states[router_id].status = BLOCKED;
            strncpy(router_states[router_id].blocked_on_request_id, request_id_local, 63);
            router_states[router_id].blocked_on_request_id[63] = '\0';
            strncpy(router_states[router_id].blocked_on_function, blocked_func_str_local, 63);
            router_states[router_id].blocked_on_function[63] = '\0';
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
    if (source_router_id > 0 && source_router_id <= MAX_ROUTERS) {
        router_states[source_router_id].status = RUNNING;
        memset(router_states[source_router_id].blocked_on_request_id, 0, sizeof(router_states[source_router_id].blocked_on_request_id));
        memset(router_states[source_router_id].blocked_on_function, 0, sizeof(router_states[source_router_id].blocked_on_function));
    }

    double transmission_delay = 0.1; // 0.1s (100ms) - 数据包传输延迟
    double receive_time = current_virtual_time + transmission_delay;

    // 使用连接表查找目标路由器
    int target_router_id = find_peer_router(source_router_id, socket_fd);
    
    if (target_router_id == -1) {
        fprintf(stderr, "[DESD ERROR] R%d: No peer found for socket fd %d. Cannot send packet.\n", 
                source_router_id, socket_fd);
        send_error_response(source_router_id, request_id, "Invalid connection");
        return;
    }

    // 查找目标路由器对应这个连接的 socket_fd（使用源路由器的 socket_fd 进行精确匹配）
    int target_socket_fd = find_socket_fd_for_peer(target_router_id, source_router_id, socket_fd);
    if (target_socket_fd == -1) {
        fprintf(stderr, "[DESD ERROR] R%d: Cannot find socket_fd for connection from R%d (source fd=%d).\n",
                target_router_id, source_router_id, socket_fd);
        send_error_response(source_router_id, request_id, "Invalid connection mapping");
        return;
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
        send_error_response(source_router_id, request_id, "Packet buffer full");
        return;
    }
    
    // 缓冲数据，并记录对应的 socket_fd
    strncpy(router_states[target_router_id].packet_buffers[buffer_index].data, packet_data, MAX_PACKET_SIZE - 1);
    router_states[target_router_id].packet_buffers[buffer_index].data_len = strlen(packet_data);
    router_states[target_router_id].packet_buffers[buffer_index].socket_fd = target_socket_fd;
    router_states[target_router_id].packet_buffers[buffer_index].is_used = 1;

    json_t *recv_payload_obj = json_object();
    json_object_set_new(recv_payload_obj, "source_router_id", json_integer(source_router_id));
    json_object_set_new(recv_payload_obj, "destination_abstract_address", json_string(destination_abstract_address));
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
    
    send_success_response(source_router_id, request_id, "SEND", "Packet Sent", NULL); // 发送方send()成功返回
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

        if (router_states[target_router_id].status == BLOCKED &&
            router_states[target_router_id].blocked_on_request_id[0] != '\0') {
            
            const char *blocked_func = router_states[target_router_id].blocked_on_function;
            
            if (strcmp(blocked_func, "RECV_CALL") == 0) {
                // 情况1a：接收方已经在 recv 上等待，立即唤醒
                char request_id[64];
                strncpy(request_id, router_states[target_router_id].blocked_on_request_id, 63);
                request_id[63] = '\0';

                router_states[target_router_id].status = RUNNING;
                memset(router_states[target_router_id].blocked_on_request_id, 0, sizeof(router_states[target_router_id].blocked_on_request_id));
                memset(router_states[target_router_id].blocked_on_function, 0, sizeof(router_states[target_router_id].blocked_on_function));
                
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
                
                printf("[DESD] R%d was blocked on recv and now awakened by PACKET_RECEIVE_EVENT for %s.\n", target_router_id, destination_abstract_address);
            } else if (strcmp(blocked_func, "SELECT_CALL") == 0) {
                // 情况1b：接收方在 select 上等待，取消超时事件并唤醒
                char request_id[64];
                strncpy(request_id, router_states[target_router_id].blocked_on_request_id, 63);
                request_id[63] = '\0';
                
                // 取消超时事件
                if (router_states[target_router_id].pending_timeout_event_id > 0) {
                    cancel_event(router_states[target_router_id].pending_timeout_event_id);
                    printf("[DESD] Canceled timeout event %lu for R%d (data arrived before timeout).\n",
                           router_states[target_router_id].pending_timeout_event_id, target_router_id);
                    router_states[target_router_id].pending_timeout_event_id = 0;
                }

                router_states[target_router_id].status = RUNNING;
                memset(router_states[target_router_id].blocked_on_request_id, 0, sizeof(router_states[target_router_id].blocked_on_request_id));
                memset(router_states[target_router_id].blocked_on_function, 0, sizeof(router_states[target_router_id].blocked_on_function));
                
                // 将 buffer_index 加入 pending 队列，后续的 recv() 才能读取数据
                router_states[target_router_id].pending_packets_count++;
                int tail = router_states[target_router_id].pending_buffer_tail;
                router_states[target_router_id].pending_buffer_indices[tail] = buffer_index;
                router_states[target_router_id].pending_buffer_tail = (tail + 1) % MAX_PENDING_PACKETS;
                
                // 构建就绪 FD 列表（包含当前到达的数据包的 socket_fd 以及其他 pending 的）
                json_t *ready_fds_array = json_array();
                int head = router_states[target_router_id].pending_buffer_head;
                int count = router_states[target_router_id].pending_packets_count;
                
                for (int i = 0; i < count; i++) {
                    int idx = (head + i) % MAX_PENDING_PACKETS;
                    int buf_idx = router_states[target_router_id].pending_buffer_indices[idx];
                    if (buf_idx >= 0 && buf_idx < MAX_PENDING_PACKETS) {
                        int sock_fd = router_states[target_router_id].packet_buffers[buf_idx].socket_fd;
                        if (sock_fd >= 0) {
                            // 检查是否已添加
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
                    .virtual_time = current_virtual_time
                };
                strncpy(response.request_id, request_id, 63);
                response.request_id[63] = '\0';
                strncpy(response.payload.json_str, response_payload_str, MAX_MSG_SIZE - 1);
                response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                free(response_payload_str);
                send_message_to_router(target_router_id, &response);
                
                printf("[DESD] R%d was blocked on select and now awakened by PACKET_RECEIVE_EVENT for %s (buffer_index=%d added to pending queue, %zu unique FDs).\n", 
                       target_router_id, destination_abstract_address, buffer_index, ready_fds_count);
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
            printf("[DESD] R%d has %d pending packet(s) (data arrived but recv/select not called yet, currently %s).\n", 
                   target_router_id, router_states[target_router_id].pending_packets_count,
                   router_states[target_router_id].status == BLOCKED ? router_states[target_router_id].blocked_on_function : "not blocked");
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

    if (router_id > 0 && router_id <= MAX_ROUTERS &&
        router_states[router_id].status == BLOCKED &&
        strcmp(router_states[router_id].blocked_on_request_id, original_block_request_id) == 0) {
        
        router_states[router_id].status = RUNNING;
        memset(router_states[router_id].blocked_on_request_id, 0, sizeof(router_states[router_id].blocked_on_request_id));
        memset(router_states[router_id].blocked_on_function, 0, sizeof(router_states[router_id].blocked_on_function));
        
        // 清除超时事件ID
        if (router_states[router_id].pending_timeout_event_id == event.event_id) {
            router_states[router_id].pending_timeout_event_id = 0;
        }
        
        // 对于 SLEEP_CALL，发送 SUCCESS 响应；对于其他超时，发送 TIMEOUT 响应
        if (timeout_type[0] != '\0' && strcmp(timeout_type, "SLEEP_CALL") == 0) {
            send_success_response(router_id, original_block_request_id, "SLEEP", "Sleep Completed", NULL);
            printf("[DESD] R%d sleep completed for request %s at VT=%.3f.\n", router_id, original_block_request_id, current_virtual_time);
        } else {
            send_timeout_response(router_id, original_block_request_id, timeout_type[0] != '\0' ? timeout_type : "UNKNOWN");
            printf("[DESD] R%d timed out for request %s (Type: %s) at VT=%.3f.\n", router_id, original_block_request_id, timeout_type[0] != '\0' ? timeout_type : "UNKNOWN", current_virtual_time);
        }
    } else {
        printf("[DESD] TIMEOUT_EVENT %lu for R%d ignored (router not blocked on this request %s anymore or already handled).\n",
               event.event_id, router_id, original_block_request_id[0] != '\0' ? original_block_request_id : "UNKNOWN");
    }
}

// --- Helper Functions for sending specific responses ---
void send_success_response(int router_id, const char* request_id, const char* blocked_func, const char* message, const char* connection_id_str) {
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

void send_error_response(int router_id, const char* request_id, const char* error_msg) {
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "status", json_string("ERROR"));
    json_object_set_new(payload_obj, "error_message", json_string(error_msg));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    json_decref(payload_obj);

    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
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

void send_eagain_response(int router_id, const char* request_id) {
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "status", json_string("EAGAIN"));
    json_object_set_new(payload_obj, "blocked_function", json_string("RECV"));
    json_object_set_new(payload_obj, "message", json_string("No data available (non-blocking)"));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    json_decref(payload_obj);

    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
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

void send_timeout_response(int router_id, const char* request_id, const char* timeout_type) {
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "status", json_string("TIMEOUT"));
    json_object_set_new(payload_obj, "blocked_function", json_string(timeout_type));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    json_decref(payload_obj);

    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
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
void handle_get_virtual_time_event(Event event) {
    int router_id = event.router_id;
    
    // 从payload中提取request_id
    json_error_t error;
    json_t *payload_obj = json_loads(event.payload.json_str, 0, &error);
    if (!payload_obj) {
        fprintf(stderr, "[DESD ERROR] handle_get_virtual_time_event: Failed to parse payload JSON.\n");
        return;
    }
    
    const char* request_id = json_string_value(json_object_get(payload_obj, "request_id"));
    if (!request_id) {
        fprintf(stderr, "[DESD ERROR] handle_get_virtual_time_event: No request_id in payload.\n");
        json_decref(payload_obj);
        return;
    }
    
    char request_id_local[64];
    strncpy(request_id_local, request_id, 63);
    request_id_local[63] = '\0';
    json_decref(payload_obj);
    
    // 构建响应，返回当前虚拟时间
    json_t *resp_payload_obj = json_object();
    json_object_set_new(resp_payload_obj, "status", json_string("SUCCESS"));
    json_object_set_new(resp_payload_obj, "current_virtual_time", json_real(current_virtual_time));
    char *payload_str = json_dumps(resp_payload_obj, JSON_COMPACT);
    json_decref(resp_payload_obj);
    
    Message response = {
        .message_type = DESD_TO_HOOK,
        .router_id = router_id,
        .virtual_time = current_virtual_time  // 响应消息也携带虚拟时间
    };
    strncpy(response.request_id, request_id_local, 63);
    response.request_id[63] = '\0';
    strncpy(response.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    response.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    
    send_message_to_router(router_id, &response);
    //GET_VIRTUAL_TIME_EVENT 不打印
    // printf("[DESD] R%d GET_VIRTUAL_TIME_EVENT responded with VT=%.6f (ReqID: %s).\n", 
    //        router_id, current_virtual_time, request_id_local);
}
