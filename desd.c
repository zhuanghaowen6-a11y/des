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
    int is_active;          // 连接是否活跃
} ConnectionInfo;

typedef struct {
    int router_id;
    RouterStatus status;
    char blocked_on_request_id[64]; // 路由器正在等待的请求ID
    char blocked_on_function[64]; // 路由器正在等待的函数类型 (RECV_CALL, ACCEPT_CALL, SELECT_CALL等)
    int comm_socket_fd; // 与该路由器libdeshook.so通信的socket FD
    char initial_register_request_id[64]; // 存储初始注册ROUTER_START事件的请求ID
    int pending_packets_count; // 记录有多少个数据包已经在虚拟时间上到达但未被读取
    int pending_connections_count; // 记录有多少个连接已经在虚拟时间上建立但未被accept
    int is_listening; // 标记路由器是否已经调用过 listen()
    char listen_address[256]; // 记录路由器监听的地址
    ConnectionInfo connections[MAX_CONNECTIONS_PER_ROUTER]; // 连接表
    unsigned long pending_timeout_event_id; // 记录正在等待的超时事件ID（用于取消）
    // 不再有数据缓冲区，因为数据直接在路由器之间传输
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

// Helper functions for sending specific responses
void send_success_response(int router_id, const char* request_id, const char* blocked_func, const char* message, const char* connection_id_str);
void send_error_response(int router_id, const char* request_id, const char* error_msg);
void send_timeout_response(int router_id, const char* request_id, const char* timeout_type);

// Connection management helper functions
int find_router_by_listen_address(const char *address);
void register_connection(int router_id, int socket_fd, int peer_router_id);
int find_peer_router(int router_id, int socket_fd);

// New helper for reading router messages and enqueuing events
// void read_and_enqueue_router_message(int router_id, int comm_fd);


// --- Main ---
int main() {
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
        router_states[i].is_listening = 0;
        router_states[i].pending_timeout_event_id = 0;
        memset(router_states[i].blocked_on_request_id, 0, sizeof(router_states[i].blocked_on_request_id));
        memset(router_states[i].blocked_on_function, 0, sizeof(router_states[i].blocked_on_function));
        memset(router_states[i].initial_register_request_id, 0, sizeof(router_states[i].initial_register_request_id));
        memset(router_states[i].listen_address, 0, sizeof(router_states[i].listen_address));
        
        // 初始化连接表
        for (int j = 0; j < MAX_CONNECTIONS_PER_ROUTER; ++j) {
            router_states[i].connections[j].socket_fd = -1;
            router_states[i].connections[j].peer_router_id = -1;
            router_states[i].connections[j].is_active = 0;
        }
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
int find_router_by_listen_address(const char *address) {
    for (int i = 1; i <= MAX_ROUTERS; i++) {
        if (router_states[i].is_listening &&
            strcmp(router_states[i].listen_address, address) == 0) {
            return i;
        }
    }
    return -1;
}

// 记录连接映射
void register_connection(int router_id, int socket_fd, int peer_router_id) {
    if (router_id <= 0 || router_id > MAX_ROUTERS) {
        fprintf(stderr, "[DESD ERROR] register_connection: Invalid router_id %d\n", router_id);
        return;
    }
    
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (!router_states[router_id].connections[i].is_active) {
            router_states[router_id].connections[i].socket_fd = socket_fd;
            router_states[router_id].connections[i].peer_router_id = peer_router_id;
            router_states[router_id].connections[i].is_active = 1;
            printf("[DESD] Registered connection: R%d (fd:%d) <-> R%d\n", 
                   router_id, socket_fd, peer_router_id);
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
    
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (router_states[router_id].connections[i].is_active &&
            router_states[router_id].connections[i].socket_fd == socket_fd) {
            return router_states[router_id].connections[i].peer_router_id;
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
    printf("[DESD] Received message (Type: %s, Event: %s, ReqID: %s) from R%d.\n",
           msg_out->message_type == HOOK_TO_DESD ? "HOOK_TO_DESD" : "DESD_TO_HOOK",
           event_type_to_string(msg_out->event_type), msg_out->request_id, router_id);

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
        printf("[DESD] Processing event %s for R%d at VT=%.3f (EventID: %lu)\n",
               event_type_to_string(current_event.event_type),
               current_event.router_id, current_virtual_time, current_event.event_id);
        
        // 处理事件
        handle_event(current_event);

        // 如果路由器被解除阻塞，等待其下一个事件
        // pthread_mutex_lock(&router_states_mutex);
        RouterInfo *router_info = &router_states[current_event.router_id];
        int is_router_running_after_event = (router_info->status == RUNNING);
        // pthread_mutex_unlock(&router_states_mutex);

        if (is_router_running_after_event) {
            printf("[DESD] R%d is now RUNNING. Waiting for its next event...\n", current_event.router_id);
            
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
                Event next_event = {
                    .timestamp = current_virtual_time, // 路由器被唤醒后，其操作被认为是瞬时的，所以事件时间戳为当前虚拟时间
                    .router_id = next_msg_from_router.router_id,
                    .event_type = next_msg_from_router.event_type,
                    .event_id = generate_event_id(),
                    .payload = next_msg_from_router.payload
                };
                next_event.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                push_event(next_event);
                printf("[DESD] R%d generated event %s (ReqID: %s) at VT %.3f (EventID: %lu).\n",
                       current_event.router_id, event_type_to_string(next_event.event_type),
                       next_msg_from_router.request_id, current_virtual_time, next_event.event_id);
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
        // 记录路由器已经开始监听
        router_states[router_id].is_listening = 1;
        strncpy(router_states[router_id].listen_address, listen_address_local, sizeof(router_states[router_id].listen_address) - 1);
        router_states[router_id].listen_address[sizeof(router_states[router_id].listen_address) - 1] = '\0';
        
        printf("[DESD] R%d is now listening on %s.\n", router_id, router_states[router_id].listen_address);
        
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
        int target_router_id = find_router_by_listen_address(destination_abstract_address_local);

        if (target_router_id == -1) {
            // 目标路由器没有 listen，连接失败
            fprintf(stderr, "[DESD ERROR] R%d attempted to connect to %s, but no router is listening on that address.\n",
                    router_id, destination_abstract_address_local);
            send_error_response(router_id, request_id, "Connection refused: Target not listening");
            router_states[router_id].status = RUNNING; // 错误发生，解除阻塞
            memset(router_states[router_id].blocked_on_request_id, 0, sizeof(router_states[router_id].blocked_on_request_id));
            memset(router_states[router_id].blocked_on_function, 0, sizeof(router_states[router_id].blocked_on_function));
            return;
        }
        
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
        json_t *source_payload_obj = json_object();
        json_object_set_new(source_payload_obj, "client_router_id", json_integer(router_id));
        json_object_set_new(source_payload_obj, "server_router_id", json_integer(target_router_id));
        json_object_set_new(source_payload_obj, "client_socket_fd", json_integer(client_socket_fd));
        json_object_set_new(source_payload_obj, "request_id", json_string(request_id));
        char *source_payload_str = json_dumps(source_payload_obj, JSON_COMPACT);
        json_decref(source_payload_obj);

        Event source_conn_est_event = {
            .timestamp = connection_established_time - 0.001, // 客户端事件早 1ms，确保先执行
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
                
                // 注册客户端的连接映射（客户端 socket_fd -> 服务器 router_id）
                register_connection(client_router_id, client_socket_fd, server_router_id);
                
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
    
    int server_socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));
    
    json_decref(payload_obj);

    // 在两个路由器的场景下，找到另一个路由器（客户端）
    int client_router_id = (router_id == 1) ? 2 : 1;
    
    // 注册服务器端的连接映射（服务器 socket_fd -> 客户端 router_id）
    register_connection(router_id, server_socket_fd, client_router_id);
    
    printf("[DESD] R%d (server) registered connection: fd %d <-> R%d (client).\n", 
           router_id, server_socket_fd, client_router_id);
    
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

    json_decref(payload_obj);

    if (router_id > 0 && router_id <= MAX_ROUTERS) {
        // 特殊处理 RECV_CALL：检查是否有 pending 数据包
        if (strcmp(blocked_func_str_local, "RECV_CALL") == 0) {
            if (router_states[router_id].pending_packets_count > 0) {
                // 数据已在虚拟时间上到达，立即唤醒
                router_states[router_id].pending_packets_count--;
                router_states[router_id].status = RUNNING;
                send_success_response(router_id, request_id_local, "RECV", "Packet Available", NULL);
                printf("[DESD] R%d recv() immediately unblocked (had %d pending packet(s)).\n", 
                       router_id, router_states[router_id].pending_packets_count + 1);
            } else {
                // 数据还没到达，保持阻塞
                router_states[router_id].status = BLOCKED;
                strncpy(router_states[router_id].blocked_on_request_id, request_id_local, 63);
                router_states[router_id].blocked_on_request_id[63] = '\0';
                strncpy(router_states[router_id].blocked_on_function, blocked_func_str_local, 63);
                router_states[router_id].blocked_on_function[63] = '\0';
                printf("[DESD] R%d blocked on %s (ReqID: %s) - no pending packets.\n",
                       router_id, blocked_func_str_local, request_id_local);
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
            // 处理 SELECT_CALL：检查是否有超时参数
            json_error_t error2;
            json_t *payload_obj2 = json_loads(event.payload.json_str, 0, &error2);
            int timeout_ms = -1;
            if (payload_obj2) {
                timeout_ms = json_integer_value(json_object_get(payload_obj2, "timeout_ms"));
                json_decref(payload_obj2);
            }
            
            // 先检查是否有 pending 数据包（数据已到达）
            if (router_states[router_id].pending_packets_count > 0) {
                // 数据已就绪，立即唤醒
                router_states[router_id].pending_packets_count--;
                router_states[router_id].status = RUNNING;
                send_success_response(router_id, request_id_local, "SELECT", "Data Available", NULL);
                printf("[DESD] R%d select() immediately unblocked (had %d pending packet(s)).\n", 
                       router_id, router_states[router_id].pending_packets_count + 1);
            } else {
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
    
    // 复制到本地缓冲区，防止 json_decref 后访问无效内存
    char request_id[64] = {0};
    char destination_abstract_address[256] = {0};
    if (request_id_ptr) {
        strncpy(request_id, request_id_ptr, sizeof(request_id) - 1);
    }
    if (destination_abstract_address_ptr) {
        strncpy(destination_abstract_address, destination_abstract_address_ptr, sizeof(destination_abstract_address) - 1);
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

    json_t *recv_payload_obj = json_object();
    json_object_set_new(recv_payload_obj, "source_router_id", json_integer(source_router_id));
    json_object_set_new(recv_payload_obj, "destination_abstract_address", json_string(destination_abstract_address));
    // 不再传递packet_data_base64
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
                // desd不再发送数据，只通知libdeshook数据已可读
                send_success_response(target_router_id, request_id, "RECV", "Packet Available", NULL); // 通知接收方数据已到达
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
                send_success_response(target_router_id, request_id, "SELECT", "Data Available", NULL);
                printf("[DESD] R%d was blocked on select and now awakened by PACKET_RECEIVE_EVENT for %s.\n", target_router_id, destination_abstract_address);
            } else {
                // 其他阻塞类型，记录为 pending
                router_states[target_router_id].pending_packets_count++;
                printf("[DESD] R%d has %d pending packet(s) (data arrived but currently blocked on %s).\n", 
                       target_router_id, router_states[target_router_id].pending_packets_count, blocked_func);
            }
        } else {
            // 情况2：接收方还没调用recv()或select()，记录数据已到达
            router_states[target_router_id].pending_packets_count++;
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
    const char *original_block_request_id = json_string_value(json_object_get(payload_obj, "original_block_request_id"));
    const char *timeout_type = json_string_value(json_object_get(payload_obj, "timeout_type"));
    json_decref(payload_obj);

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
        if (timeout_type && strcmp(timeout_type, "SLEEP_CALL") == 0) {
            send_success_response(router_id, original_block_request_id, "SLEEP", "Sleep Completed", NULL);
            printf("[DESD] R%d sleep completed for request %s at VT=%.3f.\n", router_id, original_block_request_id, current_virtual_time);
        } else {
            send_timeout_response(router_id, original_block_request_id, timeout_type);
            printf("[DESD] R%d timed out for request %s (Type: %s) at VT=%.3f.\n", router_id, original_block_request_id, timeout_type ? timeout_type : "UNKNOWN", current_virtual_time);
        }
    } else {
        printf("[DESD] TIMEOUT_EVENT %lu for R%d ignored (router not blocked on this request %s anymore or already handled).\n",
               event.event_id, router_id, original_block_request_id ? original_block_request_id : "UNKNOWN");
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
