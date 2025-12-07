#define _GNU_SOURCE // Required for RTLD_NEXT and dlsym

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>      // For dlsym
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h> // For sockaddr_in
#include <arpa/inet.h>  // For inet_ntop
#include <errno.h>
#include <jansson.h>    // For JSON handling
#include <sys/time.h> // For select timeout
#include <poll.h> // For poll to replace select for better handling of real FDs
#include <sys/stat.h> // For fstat(), S_ISSOCK()
#include <fcntl.h> // For fcntl constants
#include <stdarg.h> // For va_list, va_start, va_end
#include <time.h> // For clock_gettime
#include <pthread.h> // For thread safety

// --- Global State ---
#define MAX_TRACKED_FDS 1024
static int socket_fds[MAX_TRACKED_FDS] = {0}; // 1表示是socket (仅AF_INET/AF_INET6)
static int nonblocking_fds[MAX_TRACKED_FDS] = {0}; // 1表示设置了O_NONBLOCK
int my_router_id = -1;
int desd_control_socket_fd = -1;
static unsigned long long total_clock_calls = 0;
static double last_reported_vtime = -1.0;

// --- Thread Safety ---
// 全局互斥锁：确保同一时刻只有一个线程与desd通信
// 这是关键：BIRD 3使用多线程，但DES假设每个路由器是单线程的
static pthread_mutex_t desd_comm_mutex = PTHREAD_MUTEX_INITIALIZER;
// 保护request_counter的原子性
static pthread_mutex_t request_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
// 保护socket_fds数组的并发访问
static pthread_mutex_t socket_fds_mutex = PTHREAD_MUTEX_INITIALIZER;

// Real function pointers
static int (*real_socket)(int, int, int) = NULL;
static int (*real_connect)(int, const struct sockaddr *, socklen_t) = NULL;
static ssize_t (*real_send)(int, const void *, size_t, int) = NULL;
static ssize_t (*real_recv)(int, void *, size_t, int) = NULL;
static int (*real_close)(int) = NULL;
static int (*real_bind)(int, const struct sockaddr *, socklen_t) = NULL;
static int (*real_listen)(int, int) = NULL;
static int (*real_accept)(int, struct sockaddr *, socklen_t *) = NULL;
static int (*real_unlink)(const char *) = NULL;
static int (*real_select)(int, fd_set *, fd_set *, fd_set *, struct timeval *) = NULL;
static int (*real_poll)(struct pollfd *, nfds_t, int) = NULL;
static unsigned int (*real_sleep)(unsigned int) = NULL;
static ssize_t (*real_read)(int, void *, size_t) = NULL;
static ssize_t (*real_write)(int, const void *, size_t) = NULL;
static int (*real_fcntl)(int, int, ...) = NULL;
static int (*real_clock_gettime)(clockid_t, struct timespec *) = NULL;

void generate_request_id(char* id_buf); // Function prototype for generate_request_id

// Function prototypes for intercepted functions
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
unsigned int sleep(unsigned int seconds);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int fcntl(int fd, int cmd, ...);

// Helper function to send message to desd and wait for response
int send_msg_to_desd_and_wait_for_response(const Message* req_msg, Message* resp_msg_out);

// --- libdeshook.so Initialization ---
__attribute__((constructor))
static void lib_init(void) {
    char *router_id_str = getenv("ROUTER_ID");
    if (!router_id_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] ROUTER_ID environment variable not set. Exiting.\n");
        _exit(1);
    }
    my_router_id = atoi(router_id_str);
    if (my_router_id <= 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Invalid ROUTER_ID: %s. Exiting.\n", router_id_str);
        _exit(1);
    }

    // Get real function pointers
    real_socket = dlsym(RTLD_NEXT, "socket");
    real_connect = dlsym(RTLD_NEXT, "connect");
    real_send = dlsym(RTLD_NEXT, "send");
    real_recv = dlsym(RTLD_NEXT, "recv");
    real_close = dlsym(RTLD_NEXT, "close");
    real_bind = dlsym(RTLD_NEXT, "bind");
    real_listen = dlsym(RTLD_NEXT, "listen");
    real_accept = dlsym(RTLD_NEXT, "accept");
    real_unlink = dlsym(RTLD_NEXT, "unlink");
    real_select = dlsym(RTLD_NEXT, "select");
    real_poll = dlsym(RTLD_NEXT, "poll");
    real_sleep = dlsym(RTLD_NEXT, "sleep");
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_fcntl = dlsym(RTLD_NEXT, "fcntl");
    real_clock_gettime = dlsym(RTLD_NEXT, "clock_gettime");

    if (!real_socket || !real_connect || !real_send || !real_recv || !real_close ||
        !real_bind || !real_listen || !real_accept || !real_unlink || !real_select || 
        !real_poll || !real_sleep || !real_read || !real_write || !real_fcntl ||
        !real_clock_gettime) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Error in dlsym: %s\n", dlerror());
        _exit(1);
    }

    // Connect to desd control socket
    desd_control_socket_fd = real_socket(AF_UNIX, SOCK_STREAM, 0);
    if (desd_control_socket_fd < 0) {
        perror("[LIBDESHOOK ERROR] socket for desd control");
        _exit(1);
    }
    struct sockaddr_un desd_addr;
    memset(&desd_addr, 0, sizeof(desd_addr));
    desd_addr.sun_family = AF_UNIX;
    strncpy(desd_addr.sun_path, DESD_CONTROL_SOCKET_PATH, sizeof(desd_addr.sun_path) - 1);

    printf("[LIBDESHOOK] R%d trying to connect to desd at %s\n", my_router_id, DESD_CONTROL_SOCKET_PATH);
    if (real_connect(desd_control_socket_fd, (struct sockaddr*)&desd_addr, sizeof(desd_addr)) < 0) {
        perror("[LIBDESHOOK ERROR] connect to desd control");
        _exit(1);
    }
    printf("[LIBDESHOOK] Connected to desd control socket (fd: %d).\n", desd_control_socket_fd);

    // Register router with desd by sending a ROUTER_START event
    Message register_req;
    memset(&register_req, 0, sizeof(Message));
    register_req.message_type = HOOK_TO_DESD;
    register_req.router_id = my_router_id;
    register_req.event_type = ROUTER_START;
    register_req.virtual_time = 0.0;
    strcpy(register_req.payload.json_str, "{}");
    generate_request_id(register_req.request_id);

    Message register_resp;
    if (send_msg_to_desd_and_wait_for_response(&register_req, &register_resp)) {
        json_error_t error;
        json_t *payload_obj = json_loads(register_resp.payload.json_str, 0, &error);
        if (payload_obj && json_string_value(json_object_get(payload_obj, "status")) &&
            strcmp(json_string_value(json_object_get(payload_obj, "status")), "SUCCESS") == 0) {
            printf("[LIBDESHOOK] Router %d successfully registered and unblocked by desd. Starting normal operation.\n", my_router_id);
        } else {
            fprintf(stderr, "[LIBDESHOOK ERROR] Router %d registration failed: %s.\n", my_router_id,
                    payload_obj ? json_string_value(json_object_get(payload_obj, "error_message")) : "Unknown error");
            _exit(1);
        }
        if (payload_obj) json_decref(payload_obj);
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to get registration response from desd.\n");
        _exit(1);
    }
}

// --- Helper Functions ---
static unsigned long next_request_id_val = 1;

void generate_request_id(char* id_buf) {
    // 🔒 线程安全：保护request_counter的原子递增
    pthread_mutex_lock(&request_counter_mutex);
    unsigned long current_id = next_request_id_val++;
    pthread_mutex_unlock(&request_counter_mutex);
    
    snprintf(id_buf, 64, "req_%lu_%d", current_id, my_router_id);
}

// Helper function to send message to desd and wait for response
// 这个函数是libdeshook与desd之间同步通信的核心，路由器在此处阻塞
int send_msg_to_desd_and_wait_for_response(const Message* req_msg, Message* resp_msg_out) {
    // 🔒 关键线程安全机制：确保同一时刻只有一个线程与desd通信
    // 这实现了DES的单线程假设：每个路由器在同一时刻只有一个活跃的执行流
    pthread_mutex_lock(&desd_comm_mutex);
    
    // Send the request message
    char *json_str = message_to_json(req_msg);
    if (!json_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to serialize request message.\n");
        fflush(stderr);
        pthread_mutex_unlock(&desd_comm_mutex); // 🔓 错误时也要解锁
        return 0;
    }
    
    // Allocate space for newline + null terminator
    size_t json_len = strlen(json_str);
    char *json_with_newline = (char*)malloc(json_len + 2); // +1 for '\n', +1 for '\0'
    if (!json_with_newline) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to allocate memory for message.\n");
        fflush(stderr);
        free(json_str);
        pthread_mutex_unlock(&desd_comm_mutex); // 🔓 错误时也要解锁
        return 0;
    }
    strcpy(json_with_newline, json_str);
    json_with_newline[json_len] = '\n';
    json_with_newline[json_len + 1] = '\0';
    free(json_str);
    
    // 移除频繁的消息发送日志
    
    ssize_t sent = real_send(desd_control_socket_fd, json_with_newline, strlen(json_with_newline), 0);
    if (sent < 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] send_msg_to_desd_and_wait_for_response - send failed (errno=%d): ", errno);
        perror("");
        fflush(stderr);
        free(json_with_newline);
        pthread_mutex_unlock(&desd_comm_mutex); // 🔓 错误时也要解锁
        return 0;
    }
    // 移除字节数日志
    free(json_with_newline);

    // Now, synchronously wait for the response from desd
    char buffer[MAX_MSG_SIZE];
    ssize_t bytes_received = real_recv(desd_control_socket_fd, buffer, MAX_MSG_SIZE - 1, 0);
    
    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            fprintf(stderr, "[LIBDESHOOK ERROR] DESD disconnected during response wait (recv returned 0).\n");
        } else {
            fprintf(stderr, "[LIBDESHOOK ERROR] send_msg_to_desd_and_wait_for_response - recv failed (errno=%d): ", errno);
            perror("");
        }
        fflush(stderr);
        pthread_mutex_unlock(&desd_comm_mutex); // 🔓 错误时也要解锁
        return 0;
    }
    // 移除接收字节数日志
    buffer[bytes_received] = '\0';

    json_to_message(buffer, resp_msg_out);

    // For safety, ensure the response matches the request_id. 
    // With the mutex, this should always match now
    if (strcmp(req_msg->request_id, resp_msg_out->request_id) != 0) {
        fprintf(stderr, "[LIBDESHOOK WARNING] Response request_id mismatch! Expected %s, Got %s.\n",
                req_msg->request_id, resp_msg_out->request_id);
        // We might still process it if it's the only message.
    }
    
    // 🔓 成功完成通信，释放互斥锁
    pthread_mutex_unlock(&desd_comm_mutex);
    return 1; // Success
}

// --- Intercepted Functions ---

// connect
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    printf("[LIBDESHOOK] R%d connect() called: sockfd=%d family=%d\n", 
           my_router_id, sockfd, addr->sa_family);
    fflush(stdout);
    
    // 如果desd未连接，则直接调用真实connect
    if (desd_control_socket_fd == -1) {
        return real_connect(sockfd, addr, addrlen);
    }
    
    // 只拦截 AF_UNIX 和 AF_INET 的连接
    if (addr->sa_family != AF_UNIX && addr->sa_family != AF_INET) {
        return real_connect(sockfd, addr, addrlen);
    }
    
    // 构建抽象地址字符串
    char abstract_address[256];
    
    if (addr->sa_family == AF_UNIX) {
        // Unix domain socket: 使用路径作为抽象地址
        const char *target_path = ((struct sockaddr_un *)addr)->sun_path;
        
        // 如果是连接到 desd 控制 socket，不拦截
        if (strcmp(target_path, DESD_CONTROL_SOCKET_PATH) == 0) {
            return real_connect(sockfd, addr, addrlen);
        }
        
        // 不拦截BIRD内部控制socket（/run/bird/bird.ctl）
        if (strstr(target_path, "/run/bird") != NULL || strstr(target_path, "bird.ctl") != NULL) {
            return real_connect(sockfd, addr, addrlen);
        }
        
        strncpy(abstract_address, target_path, sizeof(abstract_address) - 1);
        abstract_address[sizeof(abstract_address) - 1] = '\0';
    } else if (addr->sa_family == AF_INET) {
        // TCP socket: 使用 IP:Port 作为抽象地址
        struct sockaddr_in *tcp_addr = (struct sockaddr_in *)addr;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(tcp_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
        int port = ntohs(tcp_addr->sin_port);
        snprintf(abstract_address, sizeof(abstract_address), "%s:%d", ip_str, port);
    }

    printf("[LIBDESHOOK] R%d intercepted connect() to %s (family: %s).\n", 
           my_router_id, abstract_address, addr->sa_family == AF_UNIX ? "AF_UNIX" : "AF_INET");
    fflush(stdout);

    // 1. 告知desd：发送CONNECT_REQUEST_EVENT事件
    Message connect_req;
    memset(&connect_req, 0, sizeof(Message));
    connect_req.message_type = HOOK_TO_DESD;
    connect_req.router_id = my_router_id;
    connect_req.event_type = CONNECT_REQUEST_EVENT;
    connect_req.virtual_time = current_virtual_time;
    generate_request_id(connect_req.request_id);
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "destination_abstract_address", json_string(abstract_address));
    json_object_set_new(payload_obj, "request_id", json_string(connect_req.request_id));
    json_object_set_new(payload_obj, "socket_fd", json_integer(sockfd));  // 添加 socket_fd
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(connect_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    connect_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

    printf("[LIBDESHOOK] R%d connect() sending CONNECT_REQUEST to desd...\n", my_router_id);
    fflush(stdout);
    
    Message connect_resp;
    if (send_msg_to_desd_and_wait_for_response(&connect_req, &connect_resp)) {
        json_error_t error;
        json_t *resp_payload_obj = json_loads(connect_resp.payload.json_str, 0, &error);

        json_t *status_json = NULL;
        if (resp_payload_obj) {
            status_json = json_object_get(resp_payload_obj, "status");
        }

        if (status_json && json_is_string(status_json) &&
            strcmp(json_string_value(status_json), "SUCCESS") == 0) {

            printf("[LIBDESHOOK] R%d connect() to %s successful (DESD confirmed). Now performing real connect.\n",
                   my_router_id, abstract_address);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            
            // 标记socket为DES管理
            if (sockfd >= 0 && sockfd < MAX_TRACKED_FDS) {
                socket_fds[sockfd] = 1;
                printf("[LIBDESHOOK] R%d marked fd %d as DES-managed socket (after connect).\n", my_router_id, sockfd);
            }
            
            // 2. DESD确认连接建立事件后，调用真实的connect()
            // 注意：客户端不需要发送 CONNECTION_INFO_EVENT，因为：
            // 1. 客户端的 socket_fd 在 CONNECT_REQUEST_EVENT 中已知
            // 2. CONNECTION_ESTABLISHED_EVENT 已经注册了客户端连接
            // 3. 只有服务端需要发送 CONNECTION_INFO_EVENT（通知 accept() 返回的 fd）
            int result = real_connect(sockfd, addr, addrlen);
            int saved_errno = errno;
            printf("[LIBDESHOOK] R%d connect() real_connect returned: %d (errno=%d %s)\n",
                   my_router_id, result, saved_errno, 
                   saved_errno == EINPROGRESS ? "EINPROGRESS" :
                   saved_errno == EISCONN ? "EISCONN" : 
                   saved_errno == 0 ? "SUCCESS" : strerror(saved_errno));
            fflush(stdout);
            
            printf("[LIBDESHOOK] R%d connect() returning result %d to BIRD\n", my_router_id, result);
            fflush(stdout);
            errno = saved_errno;
            return result;

        } else {
            const char* error_message = "Unknown error";
            json_t *error_msg_json = NULL;
            if (resp_payload_obj) {
                error_msg_json = json_object_get(resp_payload_obj, "error_message");
                if (error_msg_json && json_is_string(error_msg_json)) {
                    error_message = json_string_value(error_msg_json);
                }
            }
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d connect() failed (DESD rejected or error): %s.\n", my_router_id,
                            error_message);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = ECONNREFUSED; // Simulate connection refused
            return -1;
        }
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d connect() failed: No response from desd.\n", my_router_id);
        errno = EIO; // Simulate I/O error
        return -1;
    }
}

// accept
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (desd_control_socket_fd == -1) {
        return real_accept(sockfd, addr, addrlen);
    }

    // 假设是针对ROUTER_SOCKET_PATH的监听socket
    printf("[LIBDESHOOK] R%d intercepted accept() on sockfd %d.\n", my_router_id, sockfd);
    fflush(stdout);

    // 1. 告知desd：发送ROUTER_BLOCK_REQUEST事件（blocked_function=ACCEPT）
    Message block_req;
    memset(&block_req, 0, sizeof(Message));
    block_req.message_type = HOOK_TO_DESD;
    block_req.router_id = my_router_id;
    block_req.event_type = ROUTER_BLOCK_REQUEST;
    block_req.virtual_time = current_virtual_time;
    generate_request_id(block_req.request_id);
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "blocked_function", json_string("ACCEPT_CALL"));
    json_object_set_new(payload_obj, "request_id", json_string(block_req.request_id));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(block_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    block_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

    Message block_resp;
    if (send_msg_to_desd_and_wait_for_response(&block_req, &block_resp)) {
        json_error_t error;
        json_t *resp_payload_obj = json_loads(block_resp.payload.json_str, 0, &error);

        json_t *status_json = NULL;
        if (resp_payload_obj) {
            status_json = json_object_get(resp_payload_obj, "status");
        }

        if (status_json && json_is_string(status_json) &&
            strcmp(json_string_value(status_json), "SUCCESS") == 0) {
            
            printf("[LIBDESHOOK] R%d accept() unblocked by DESD. Now performing real accept.\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            
            // 2. DESD解除阻塞后，调用真实的accept()
            int new_fd = real_accept(sockfd, addr, addrlen);
            if (new_fd < 0) {
                return new_fd;  // accept 失败，直接返回
            }
            
            // 3. accept 成功，发送 CONNECTION_INFO_EVENT 给 desd，通知新连接的 fd
            Message conn_info;
            memset(&conn_info, 0, sizeof(Message));
            conn_info.message_type = HOOK_TO_DESD;
            conn_info.router_id = my_router_id;
            conn_info.event_type = CONNECTION_INFO_EVENT;
            conn_info.virtual_time = current_virtual_time;
            generate_request_id(conn_info.request_id);
            
            json_t *conn_payload_obj = json_object();
            json_object_set_new(conn_payload_obj, "socket_fd", json_integer(new_fd));
            json_object_set_new(conn_payload_obj, "listen_fd", json_integer(sockfd));
            json_object_set_new(conn_payload_obj, "request_id", json_string(conn_info.request_id));
            char *conn_payload_str = json_dumps(conn_payload_obj, JSON_COMPACT);
            strncpy(conn_info.payload.json_str, conn_payload_str, MAX_MSG_SIZE - 1);
            conn_info.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
            free(conn_payload_str);
            json_decref(conn_payload_obj);
            
            Message conn_resp;
            if (send_msg_to_desd_and_wait_for_response(&conn_info, &conn_resp)) {
                printf("[LIBDESHOOK] R%d notified DESD of new connection fd %d.\n", my_router_id, new_fd);
            } else {
                fprintf(stderr, "[LIBDESHOOK WARNING] R%d failed to notify DESD of new connection.\n", my_router_id);
            }
            
            // 标记新连接的socket为DES管理
            if (new_fd >= 0 && new_fd < MAX_TRACKED_FDS) {
                socket_fds[new_fd] = 1;
                printf("[LIBDESHOOK] R%d marked accepted fd %d as DES-managed socket.\n", my_router_id, new_fd);
            }
            
            return new_fd;

        } else {
            const char* error_message = "Unknown error";
            json_t *error_msg_json = NULL;
            if (resp_payload_obj) {
                error_msg_json = json_object_get(resp_payload_obj, "error_message");
                if (error_msg_json && json_is_string(error_msg_json)) {
                    error_message = json_string_value(error_msg_json);
                }
            }
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d accept() failed (DESD rejected or error): %s.\n", my_router_id,
                            error_message);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = ECOMM; // Simulate communication error
            return -1;
        }
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d accept() failed: No response from desd.\n", my_router_id);
        errno = EIO;
        return -1;
    }
}

// send
ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    // 不要拦截对 desd_control_socket_fd 的 send 调用
    if (desd_control_socket_fd == -1 || sockfd == desd_control_socket_fd) {
        return real_send(sockfd, buf, len, flags);
    }

    // 不拦截netlink socket（与内核通信）和其他非网络socket
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(sockfd, (struct sockaddr*)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_NETLINK || addr.ss_family == AF_UNIX) {
            return real_send(sockfd, buf, len, flags);
        }
        // 只拦截AF_INET (IPv4 TCP/UDP)
        if (addr.ss_family != AF_INET) {
            return real_send(sockfd, buf, len, flags);
        }
    }

    printf("[LIBDESHOOK] R%d intercepted send() on sockfd %d, len %zu.\n", my_router_id, sockfd, len);
    fflush(stdout);

    // 1. 构建 PACKET_SEND_EVENT 消息
    Message send_req;
    memset(&send_req, 0, sizeof(Message)); // 初始化所有字段为0
    send_req.message_type = HOOK_TO_DESD;
    send_req.router_id = my_router_id;
    send_req.event_type = PACKET_SEND_EVENT;
    send_req.virtual_time = current_virtual_time;
    generate_request_id(send_req.request_id);

    json_t *payload_obj = json_object();
    if (!payload_obj) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to create JSON object\n");
        errno = ENOMEM;
        return -1;
    }
    
    // 将数据进行 Base64 编码，包含在 payload 中发送给 desd
    size_t base64_len = ((len + 2) / 3) * 4 + 1;  // Base64 编码后的长度
    char *base64_data = (char*)malloc(base64_len);
    if (!base64_data) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to allocate base64 buffer\n");
        json_decref(payload_obj);
        errno = ENOMEM;
        return -1;
    }
    
    // 简单的 Base64 编码（为了简化，这里直接用 hex 编码）
    size_t hex_len = len * 2 + 1;
    char *hex_data = (char*)malloc(hex_len);
    if (!hex_data) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to allocate hex buffer\n");
        free(base64_data);
        json_decref(payload_obj);
        errno = ENOMEM;
        return -1;
    }
    
    const unsigned char *byte_buf = (const unsigned char *)buf;
    for (size_t i = 0; i < len; i++) {
        sprintf(hex_data + i * 2, "%02x", byte_buf[i]);
    }
    hex_data[len * 2] = '\0';
    
    json_object_set_new(payload_obj, "destination_abstract_address", json_string(ROUTER_SOCKET_PATH)); // 简化处理，假设发往ROUTER_SOCKET_PATH
    json_object_set_new(payload_obj, "request_id", json_string(send_req.request_id));
    json_object_set_new(payload_obj, "bytes_sent", json_integer(len));
    json_object_set_new(payload_obj, "socket_fd", json_integer(sockfd));
    json_object_set_new(payload_obj, "packet_data", json_string(hex_data));  // 添加数据
    
    free(hex_data);
    free(base64_data);
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    if (!payload_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to dump JSON\n");
        json_decref(payload_obj);
        errno = ENOMEM;
        return -1;
    }
    strncpy(send_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    send_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

    // 2. 发送 PACKET_SEND_EVENT 给 desd 并阻塞，等待 desd 响应
    Message send_resp;
    if (send_msg_to_desd_and_wait_for_response(&send_req, &send_resp)) {
        json_error_t error;
        json_t *resp_payload_obj = json_loads(send_resp.payload.json_str, 0, &error);
        
        json_t *status_json = NULL;
        if (resp_payload_obj) {
            status_json = json_object_get(resp_payload_obj, "status");
        }

        if (status_json && json_is_string(status_json) &&
            strcmp(json_string_value(status_json), "SUCCESS") == 0) {

            printf("[LIBDESHOOK] R%d send() completed (data sent through DESD, len=%ld).\n", my_router_id, len);
            if (resp_payload_obj) json_decref(resp_payload_obj);

            // 数据已通过 desd 传输，不需要调用 real_send()
            // 直接返回发送的字节数
            return len;

        } else {
            const char* error_message = "Unknown error";
            json_t *error_msg_json = NULL;
            if (resp_payload_obj) {
                error_msg_json = json_object_get(resp_payload_obj, "error_message");
                if (error_msg_json && json_is_string(error_msg_json)) {
                    error_message = json_string_value(error_msg_json);
                }
            }
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d send() failed (DESD rejected or error): %s.\n", my_router_id,
                            error_message);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = ECOMM; // Simulate communication error
            return -1;
        }
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d send() failed: No response from desd.\n", my_router_id);
        errno = EIO; // Simulate I/O error
        return -1;
    }
}

// recv
ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    // 不要拦截对 desd_control_socket_fd 的 recv 调用
    if (desd_control_socket_fd == -1 || sockfd == desd_control_socket_fd) {
        return real_recv(sockfd, buf, len, flags);
    }

    // 不拦截netlink socket（与内核通信）和其他非网络socket
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(sockfd, (struct sockaddr*)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_NETLINK || addr.ss_family == AF_UNIX) {
            return real_recv(sockfd, buf, len, flags);
        }
        // 只拦截AF_INET (IPv4 TCP/UDP)
        if (addr.ss_family != AF_INET) {
            return real_recv(sockfd, buf, len, flags);
        }
    }

    printf("[LIBDESHOOK] R%d intercepted recv() on sockfd %d, max len %zu.\n", my_router_id, sockfd, len);

    // 检查是否为非阻塞socket
    int is_nonblocking = (sockfd >= 0 && sockfd < MAX_TRACKED_FDS && nonblocking_fds[sockfd]);
    if (is_nonblocking) {
        printf("[LIBDESHOOK] R%d recv() on NON-BLOCKING socket fd %d.\n", my_router_id, sockfd);
    }

    // 向desd注册阻塞请求（如果是非阻塞socket，desd应立即返回状态）
    Message recv_block_req;
    memset(&recv_block_req, 0, sizeof(Message));
    recv_block_req.message_type = HOOK_TO_DESD;
    recv_block_req.router_id = my_router_id;
    recv_block_req.event_type = ROUTER_BLOCK_REQUEST;
    recv_block_req.virtual_time = current_virtual_time;
    generate_request_id(recv_block_req.request_id);
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "blocked_function", json_string("RECV_CALL"));
    json_object_set_new(payload_obj, "request_id", json_string(recv_block_req.request_id));
    json_object_set_new(payload_obj, "sockfd", json_integer(sockfd)); // 传递sockfd信息，可能用于desd调度select
    json_object_set_new(payload_obj, "nonblocking", json_boolean(is_nonblocking)); // 传递非阻塞标志
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(recv_block_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    recv_block_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

    Message recv_block_resp;
    if (send_msg_to_desd_and_wait_for_response(&recv_block_req, &recv_block_resp)) {
        json_error_t error;
        json_t *resp_payload_obj = json_loads(recv_block_resp.payload.json_str, 0, &error);
        if (resp_payload_obj && json_string_value(json_object_get(resp_payload_obj, "status")) &&
            strcmp(json_string_value(json_object_get(resp_payload_obj, "status")), "SUCCESS") == 0) {
            
            printf("[LIBDESHOOK] R%d recv() unblocked by DESD. Receiving data from DESD.\n", my_router_id);
            
            // 从 desd 的响应中读取数据（hex 编码）
            json_t *packet_data_json = json_object_get(resp_payload_obj, "packet_data");
            if (!packet_data_json || !json_is_string(packet_data_json)) {
                fprintf(stderr, "[LIBDESHOOK ERROR] R%d recv() - no packet_data in DESD response.\n", my_router_id);
                if (resp_payload_obj) json_decref(resp_payload_obj);
                errno = EIO;
                return -1;
            }
            
            const char *hex_data = json_string_value(packet_data_json);
            size_t hex_len = strlen(hex_data);
            size_t data_len = hex_len / 2;
            
            if (data_len > len) {
                fprintf(stderr, "[LIBDESHOOK ERROR] R%d recv() - received data (%zu bytes) larger than buffer (%zu bytes).\n", 
                        my_router_id, data_len, len);
                if (resp_payload_obj) json_decref(resp_payload_obj);
                errno = EOVERFLOW;
                return -1;
            }
            
            // 解码 hex 数据到 buf
            for (size_t i = 0; i < data_len; i++) {
                unsigned int byte_val;
                sscanf(hex_data + i * 2, "%2x", &byte_val);
                ((unsigned char*)buf)[i] = (unsigned char)byte_val;
            }
            
            printf("[LIBDESHOOK] R%d recv() completed (received %zu bytes from DESD).\n", my_router_id, data_len);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return data_len;

        } else if (resp_payload_obj && json_string_value(json_object_get(resp_payload_obj, "status")) &&
                   strcmp(json_string_value(json_object_get(resp_payload_obj, "status")), "EOF") == 0) {
            // 对端已关闭连接，返回0表示EOF
            printf("[LIBDESHOOK] R%d recv() received EOF - peer closed connection.\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return 0;  // EOF: connection closed by peer
        } else if (resp_payload_obj && json_string_value(json_object_get(resp_payload_obj, "status")) &&
                   strcmp(json_string_value(json_object_get(resp_payload_obj, "status")), "TIMEOUT") == 0) {
            printf("[LIBDESHOOK] R%d recv() timed out.\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = EAGAIN; // Resource temporarily unavailable (timeout)
            return -1;
        } else if (resp_payload_obj && json_string_value(json_object_get(resp_payload_obj, "status")) &&
                   strcmp(json_string_value(json_object_get(resp_payload_obj, "status")), "EAGAIN") == 0) {
            printf("[LIBDESHOOK] R%d recv() on non-blocking socket, no data available (EAGAIN).\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = EAGAIN; // Resource temporarily unavailable (non-blocking)
            return -1;
        } else {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d recv() failed (DESD rejected or error): %s.\n", my_router_id,
                    resp_payload_obj ? json_string_value(json_object_get(resp_payload_obj, "error_message")) : "Unknown error");
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = ECOMM;
            return -1;
        }
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d recv() failed: No response from desd.\n", my_router_id);
        errno = EIO;
        return -1;
    }
}

// poll的内部实现 - 真正的hook逻辑
static int poll_internal(struct pollfd *fds, nfds_t nfds, int timeout) {
    //printf("hook poll!!!\n");
    if (desd_control_socket_fd == -1) {
        return real_poll(fds, nfds, timeout);
    }

    // 🔒 线程安全：保护socket_fds数组的读取，创建一个本地副本
    // 避免在整个poll过程中持有锁，因为poll可能会阻塞很长时间
    int local_socket_fds[MAX_TRACKED_FDS];
    pthread_mutex_lock(&socket_fds_mutex);
    memcpy(local_socket_fds, socket_fds, sizeof(socket_fds));
    pthread_mutex_unlock(&socket_fds_mutex);

    // 分离DES管理的socket和非DES管理的fd
    nfds_t des_count = 0;
    nfds_t non_des_count = 0;
    
    for (nfds_t i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < MAX_TRACKED_FDS && local_socket_fds[fd]) {
            des_count++;
        } else {
            non_des_count++;
        }
    }
    
    // 如果没有DES管理的socket，直接使用real_poll（不打印调试信息）
    if (des_count == 0) {
        return real_poll(fds, nfds, timeout);
    }

    // 只有涉及DES-managed socket时才打印调试信息
    printf("[LIBDESHOOK] R%d intercepted poll() with %lu fds, timeout %d ms.\n", my_router_id, (unsigned long)nfds, timeout);
    
    for (nfds_t i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < MAX_TRACKED_FDS && local_socket_fds[fd]) {
            printf("[LIBDESHOOK] R%d poll(): fd %d is DES-managed socket\n", my_router_id, fd);
        }
    }
    
    // printf("[LIBDESHOOK] R%d poll() split: %lu DES-managed, %lu non-DES\n", 
    //        my_router_id, (unsigned long)des_count, (unsigned long)non_des_count);

    // 1. 对于非DES管理的fd，先用real_poll非阻塞检查
    int non_des_ready = 0;
    if (non_des_count > 0) {
        struct pollfd *non_des_fds = (struct pollfd*)malloc(sizeof(struct pollfd) * non_des_count);
        nfds_t non_des_idx = 0;
        
        for (nfds_t i = 0; i < nfds; i++) {
            int fd = fds[i].fd;
            if (fd < 0 || fd >= MAX_TRACKED_FDS || !local_socket_fds[fd]) {
                non_des_fds[non_des_idx] = fds[i];
                non_des_idx++;
            }
        }
        
        // 非阻塞检查非DES fd
        non_des_ready = real_poll(non_des_fds, non_des_count, 0);
        
        if (non_des_ready > 0) {
            // printf("[LIBDESHOOK] R%d poll() real_poll returned %d ready non-DES fds\n", 
            //        my_router_id, non_des_ready);
            // 将结果复制回原数组
            non_des_idx = 0;
            for (nfds_t i = 0; i < nfds; i++) {
                int fd = fds[i].fd;
                if (fd < 0 || fd >= MAX_TRACKED_FDS || !local_socket_fds[fd]) {
                    fds[i].revents = non_des_fds[non_des_idx].revents;
                    non_des_idx++;
                }
            }
        }
        
        free(non_des_fds);
        
        // 如果非DES fd已就绪，立即返回（无论timeout是多少）
        if (non_des_ready > 0) {
            printf("[LIBDESHOOK] R%d poll() non-DES fds ready, returning immediately without waiting for DES\n", 
                   my_router_id);
            // 清除DES fd的revents（它们未被检查）
            for (nfds_t i = 0; i < nfds; i++) {
                int fd = fds[i].fd;
                if (fd >= 0 && fd < MAX_TRACKED_FDS && local_socket_fds[fd]) {
                    fds[i].revents = 0;
                }
            }
            return non_des_ready;
        }
    }

    // 如果没有DES fd需要检查，直接返回
    if (des_count == 0) {
        return non_des_ready;  // 通常为0
    }
    
    // 如果是非阻塞poll且非DES fd都没就绪，对DES fd也进行非阻塞检查
    if (timeout == 0) {
        printf("[LIBDESHOOK] R%d poll() non-blocking, non-DES not ready, checking DES fds\n", 
               my_router_id);
        // 继续向desd发送请求（timeout=0）
    }

    // 2. 向desd发送DES管理的socket请求
    Message poll_block_req;
    memset(&poll_block_req, 0, sizeof(Message));
    poll_block_req.message_type = HOOK_TO_DESD;
    poll_block_req.router_id = my_router_id;
    poll_block_req.event_type = ROUTER_BLOCK_REQUEST;
    poll_block_req.virtual_time = current_virtual_time;
    generate_request_id(poll_block_req.request_id);

    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "blocked_function", json_string("SELECT_CALL"));
    json_object_set_new(payload_obj, "request_id", json_string(poll_block_req.request_id));
    if (timeout > 0) {
        json_object_set_new(payload_obj, "timeout_ms", json_integer(timeout));
    }
    
    // 只传递DES管理的socket fd
    json_t *monitored_fds_array = json_array();
    for (nfds_t i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < MAX_TRACKED_FDS && socket_fds[fd]) {
            json_t *fd_info = json_object();
            json_object_set_new(fd_info, "fd", json_integer(fds[i].fd));
            json_object_set_new(fd_info, "events", json_integer(fds[i].events));
            json_array_append_new(monitored_fds_array, fd_info);
        }
    }
    json_object_set_new(payload_obj, "monitored_fds", monitored_fds_array);
    
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(poll_block_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    poll_block_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

    Message poll_block_resp;
    if (send_msg_to_desd_and_wait_for_response(&poll_block_req, &poll_block_resp)) {
        json_error_t error;
        json_t *resp_payload_obj = json_loads(poll_block_resp.payload.json_str, 0, &error);
        if (resp_payload_obj && json_string_value(json_object_get(resp_payload_obj, "status")) &&
            strcmp(json_string_value(json_object_get(resp_payload_obj, "status")), "SUCCESS") == 0) {
            
            // 从响应中提取就绪的 DES socket FD 列表
            json_t *ready_fds_array = json_object_get(resp_payload_obj, "ready_fds");
            
            // 只清除DES管理的socket的revents（保留非DES fd的revents）
            for (nfds_t i = 0; i < nfds; i++) {
                int fd = fds[i].fd;
                if (fd >= 0 && fd < MAX_TRACKED_FDS && socket_fds[fd]) {
                    fds[i].revents = 0;
                }
            }
            
            int des_ready_count = 0;
            
            if (ready_fds_array && json_is_array(ready_fds_array)) {
                // 根据 ready_fds 精确设置 revents（支持多种事件类型）
                size_t array_size = json_array_size(ready_fds_array);
                
                for (size_t j = 0; j < array_size; j++) {
                    json_t *fd_info = json_array_get(ready_fds_array, j);
                    
                    // 解析 {fd, revents} 对象
                    if (json_is_object(fd_info)) {
                        json_t *fd_obj = json_object_get(fd_info, "fd");
                        json_t *revents_obj = json_object_get(fd_info, "revents");
                        
                        if (fd_obj && revents_obj) {
                            int ready_fd = json_integer_value(fd_obj);
                            short ready_revents = (short)json_integer_value(revents_obj);
                            
                            // 在 fds 数组中查找匹配的 FD 并设置 revents
                            for (nfds_t i = 0; i < nfds; i++) {
                                if (fds[i].fd == ready_fd) {
                                    // 只设置客户端请求的事件类型
                                    short filtered_revents = ready_revents & (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
                                    fds[i].revents = filtered_revents;
                                    // 只有过滤后仍有事件，才累加计数（防御性编程）
                                    if (filtered_revents != 0) {
                                        des_ready_count++;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                
                printf("[LIBDESHOOK] R%d poll() DESD reported %d ready DES socket(s)\n", 
                       my_router_id, des_ready_count);
            }
            
            // 计算总的就绪fd数量（DES + 非DES）
            int total_ready = des_ready_count + non_des_ready;
            printf("[LIBDESHOOK] R%d poll() returning total %d ready FDs (%d DES + %d non-DES)\n", 
                   my_router_id, total_ready, des_ready_count, non_des_ready);
            
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return total_ready;

        } else if (resp_payload_obj && json_string_value(json_object_get(resp_payload_obj, "status")) &&
                   strcmp(json_string_value(json_object_get(resp_payload_obj, "status")), "TIMEOUT") == 0) {
            printf("[LIBDESHOOK] R%d poll() timed out (DESD confirmed).\n", my_router_id);
            
            // 超时：只清除DES socket的revents（保留非DES fd的revents）
            for (nfds_t i = 0; i < nfds; i++) {
                int fd = fds[i].fd;
                if (fd >= 0 && fd < MAX_TRACKED_FDS && socket_fds[fd]) {
                    fds[i].revents = 0;
                }
            }
            
            if (resp_payload_obj) json_decref(resp_payload_obj);
            // 如果有非DES fd就绪，返回它们的数量；否则返回0表示超时
            return non_des_ready;
        } else {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d poll() failed (DESD rejected or error): %s.\n", my_router_id,
                    resp_payload_obj ? json_string_value(json_object_get(resp_payload_obj, "error_message")) : "Unknown error");
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = ECOMM;
            return -1;
        }
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d poll() failed: No response from desd.\n", my_router_id);
        errno = EIO;
        return -1;
    }
}

// Hook所有poll变体 - BIRD可能调用glibc的内部版本
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return poll_internal(fds, nfds, timeout);
}

// Hook __poll (弱符号版本)
int __poll(struct pollfd *fds, nfds_t nfds, int timeout) __attribute__((weak, alias("poll")));

// Hook __libc_poll (libc内部版本)
int __libc_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    fprintf(stderr, "[LIBDESHOOK] R%d __libc_poll() called -> redirecting to poll_internal\n", my_router_id);
    fflush(stderr);
    return poll_internal(fds, nfds, timeout);
}

// Hook __GI___poll (glibc内部版本) - 这是BIRD实际调用的！
int __GI___poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    fprintf(stderr, "[LIBDESHOOK] R%d __GI___poll() called -> redirecting to poll_internal\n", my_router_id);
    fflush(stderr);
    return poll_internal(fds, nfds, timeout);
}

int __poll_chk(struct pollfd *fds, nfds_t nfds, int timeout, size_t fds_size) {
    return poll_internal(fds, nfds, timeout);
}

// select - 用于支持带超时的I/O操作
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    if (desd_control_socket_fd == -1) {
        return real_select(nfds, readfds, writefds, exceptfds, timeout);
    }

    // 计算超时时间（毫秒）
    int timeout_ms = -1;  // -1 表示无限等待
    if (timeout != NULL) {
        timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    }
    
    printf("[LIBDESHOOK] R%d intercepted select() with timeout %d ms.\n", my_router_id, timeout_ms);

    // 如果是非阻塞调用（timeout = 0），直接调用真实的select
    if (timeout != NULL && timeout->tv_sec == 0 && timeout->tv_usec == 0) {
        printf("[LIBDESHOOK] R%d select() is non-blocking, calling real select.\n", my_router_id);
        return real_select(nfds, readfds, writefds, exceptfds, timeout);
    }

    // 否则，向desd注册阻塞请求（SELECT_CALL）
    Message select_block_req;
    memset(&select_block_req, 0, sizeof(Message));
    select_block_req.message_type = HOOK_TO_DESD;
    select_block_req.router_id = my_router_id;
    select_block_req.event_type = ROUTER_BLOCK_REQUEST;
    select_block_req.virtual_time = current_virtual_time;
    generate_request_id(select_block_req.request_id);

    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "blocked_function", json_string("SELECT_CALL"));
    json_object_set_new(payload_obj, "request_id", json_string(select_block_req.request_id));
    if (timeout_ms >= 0) {
        json_object_set_new(payload_obj, "timeout_ms", json_integer(timeout_ms));
    }
    
    // 构建monitored_fds数组，传递要监控的文件描述符信息
    json_t *monitored_fds_array = json_array();
    
    // 添加readfds中的文件描述符
    if (readfds != NULL) {
        for (int fd = 0; fd < nfds; fd++) {
            if (FD_ISSET(fd, readfds)) {
                json_t *fd_info = json_object();
                json_object_set_new(fd_info, "fd", json_integer(fd));
                json_object_set_new(fd_info, "events", json_integer(0x001)); // POLLIN
                json_array_append_new(monitored_fds_array, fd_info);
            }
        }
    }
    
    // 添加writefds中的文件描述符
    if (writefds != NULL) {
        for (int fd = 0; fd < nfds; fd++) {
            if (FD_ISSET(fd, writefds)) {
                // 检查是否已经在数组中
                int found = 0;
                size_t array_size = json_array_size(monitored_fds_array);
                for (size_t i = 0; i < array_size; i++) {
                    json_t *existing = json_array_get(monitored_fds_array, i);
                    int existing_fd = json_integer_value(json_object_get(existing, "fd"));
                    if (existing_fd == fd) {
                        // 已存在，添加POLLOUT标志
                        int events = json_integer_value(json_object_get(existing, "events"));
                        json_object_set_new(existing, "events", json_integer(events | 0x004)); // POLLOUT
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    json_t *fd_info = json_object();
                    json_object_set_new(fd_info, "fd", json_integer(fd));
                    json_object_set_new(fd_info, "events", json_integer(0x004)); // POLLOUT
                    json_array_append_new(monitored_fds_array, fd_info);
                }
            }
        }
    }
    
    // 添加exceptfds中的文件描述符
    if (exceptfds != NULL) {
        for (int fd = 0; fd < nfds; fd++) {
            if (FD_ISSET(fd, exceptfds)) {
                int found = 0;
                size_t array_size = json_array_size(monitored_fds_array);
                for (size_t i = 0; i < array_size; i++) {
                    json_t *existing = json_array_get(monitored_fds_array, i);
                    int existing_fd = json_integer_value(json_object_get(existing, "fd"));
                    if (existing_fd == fd) {
                        int events = json_integer_value(json_object_get(existing, "events"));
                        json_object_set_new(existing, "events", json_integer(events | 0x008)); // POLLERR
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    json_t *fd_info = json_object();
                    json_object_set_new(fd_info, "fd", json_integer(fd));
                    json_object_set_new(fd_info, "events", json_integer(0x008)); // POLLERR
                    json_array_append_new(monitored_fds_array, fd_info);
                }
            }
        }
    }
    
    json_object_set_new(payload_obj, "monitored_fds", monitored_fds_array);
    
    printf("[LIBDESHOOK] R%d select() monitoring %zu fd(s).\n", my_router_id, json_array_size(monitored_fds_array));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(select_block_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    select_block_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

    Message select_block_resp;
    if (send_msg_to_desd_and_wait_for_response(&select_block_req, &select_block_resp)) {
        json_error_t error;
        json_t *resp_payload_obj = json_loads(select_block_resp.payload.json_str, 0, &error);
        
        json_t *status_json = NULL;
        if (resp_payload_obj) {
            status_json = json_object_get(resp_payload_obj, "status");
        }
        
        if (status_json && json_is_string(status_json) &&
            strcmp(json_string_value(status_json), "SUCCESS") == 0) {
            
            // 获取desd返回的ready_fds
            json_t *ready_fds_array = json_object_get(resp_payload_obj, "ready_fds");
            int ready_count = 0;
            
            if (ready_fds_array && json_is_array(ready_fds_array)) {
                // 清空原始的fd_set
                if (readfds) FD_ZERO(readfds);
                if (writefds) FD_ZERO(writefds);
                if (exceptfds) FD_ZERO(exceptfds);
                
                // 设置desd返回的就绪fd
                size_t array_size = json_array_size(ready_fds_array);
                for (size_t i = 0; i < array_size; i++) {
                    json_t *fd_info = json_array_get(ready_fds_array, i);
                    int fd = json_integer_value(json_object_get(fd_info, "fd"));
                    int revents = json_integer_value(json_object_get(fd_info, "revents"));
                    
                    if (revents & 0x001) { // POLLIN
                        if (readfds) FD_SET(fd, readfds);
                        ready_count++;
                    }
                    if (revents & 0x004) { // POLLOUT
                        if (writefds) FD_SET(fd, writefds);
                        ready_count++;
                    }
                    if (revents & 0x008) { // POLLERR
                        if (exceptfds) FD_SET(fd, exceptfds);
                        ready_count++;
                    }
                }
            }
            
            printf("[LIBDESHOOK] R%d select() unblocked by DESD (%d ready fd(s)).\n", my_router_id, ready_count);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return ready_count;

        } else if (status_json && json_is_string(status_json) &&
                   strcmp(json_string_value(status_json), "TIMEOUT") == 0) {
            printf("[LIBDESHOOK] R%d select() timed out (DESD confirmed).\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return 0; // select timeout returns 0
        } else {
            const char* error_message = "Unknown error";
            json_t *error_msg_json = NULL;
            if (resp_payload_obj) {
                error_msg_json = json_object_get(resp_payload_obj, "error_message");
                if (error_msg_json && json_is_string(error_msg_json)) {
                    error_message = json_string_value(error_msg_json);
                }
            }
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d select() failed (DESD rejected or error): %s.\n", my_router_id, error_message);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = EIO;
            return -1;
        }
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d select() failed: No response from desd.\n", my_router_id);
        errno = EIO;
        return -1;
    }
}

// close - 拦截并通知DESD socket关闭，清理FD跟踪标记
int close(int sockfd) {
    printf("[LIBDESHOOK] R%d intercepted close() for sockfd %d.\n", my_router_id, sockfd);
    
    // 检查是否是DES管理的socket
    int is_des_socket = 0;
    if (sockfd >= 0 && sockfd < MAX_TRACKED_FDS) {
        pthread_mutex_lock(&socket_fds_mutex);
        is_des_socket = socket_fds[sockfd];
        pthread_mutex_unlock(&socket_fds_mutex);
    }
    
    // 如果是DES管理的socket，通知DESD并等待响应
    if (is_des_socket && desd_control_socket_fd != -1) {
        printf("[LIBDESHOOK] R%d notifying DESD of socket close for fd %d.\n", my_router_id, sockfd);
        
        // 构建CLOSE_SOCKET_EVENT消息
        Message close_msg;
        memset(&close_msg, 0, sizeof(Message));
        close_msg.message_type = HOOK_TO_DESD;
        close_msg.router_id = my_router_id;
        close_msg.event_type = CLOSE_SOCKET_EVENT;
        close_msg.virtual_time = current_virtual_time;
        generate_request_id(close_msg.request_id);
        
        json_t *payload_obj = json_object();
        json_object_set_new(payload_obj, "socket_fd", json_integer(sockfd));
        json_object_set_new(payload_obj, "request_id", json_string(close_msg.request_id));
        char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
        strncpy(close_msg.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
        close_msg.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
        free(payload_str);
        json_decref(payload_obj);
        
        // 发送给DESD并等待响应（保持DES框架的同步性）
        Message close_resp;
        if (send_msg_to_desd_and_wait_for_response(&close_msg, &close_resp)) {
            json_error_t error;
            json_t *resp_payload_obj = json_loads(close_resp.payload.json_str, 0, &error);
            if (resp_payload_obj) {
                const char *status = json_string_value(json_object_get(resp_payload_obj, "status"));
                if (status && strcmp(status, "SUCCESS") == 0) {
                    printf("[LIBDESHOOK] R%d close() confirmed by DESD for fd %d.\n", my_router_id, sockfd);
                } else {
                    fprintf(stderr, "[LIBDESHOOK WARNING] R%d close() for fd %d, DESD response: %s\n", 
                            my_router_id, sockfd, status ? status : "UNKNOWN");
                }
                json_decref(resp_payload_obj);
            }
        } else {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d failed to get response from DESD for close(fd %d).\n", 
                    my_router_id, sockfd);
        }
    }
    
    // 清理FD跟踪标记
    if (sockfd >= 0 && sockfd < MAX_TRACKED_FDS) {
        pthread_mutex_lock(&socket_fds_mutex);
        if (socket_fds[sockfd] || nonblocking_fds[sockfd]) {
            printf("[LIBDESHOOK] R%d clearing tracking for fd %d (socket:%d, nonblocking:%d).\n", 
                   my_router_id, sockfd, socket_fds[sockfd], nonblocking_fds[sockfd]);
        }
        socket_fds[sockfd] = 0;
        nonblocking_fds[sockfd] = 0;
        pthread_mutex_unlock(&socket_fds_mutex);
    }
    
    return real_close(sockfd);
}

// bind - 简单拦截，不与DESD交互
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (addr->sa_family == AF_UNIX) {
        struct sockaddr_un *un_addr = (struct sockaddr_un *)addr;
        if (strcmp(un_addr->sun_path, ROUTER_SOCKET_PATH) == 0) {
            printf("[LIBDESHOOK] R%d intercepted bind() to %s. Allowing real bind.\n", my_router_id, ROUTER_SOCKET_PATH);
        }
    } else if (addr->sa_family == AF_INET) {
        struct sockaddr_in *tcp_addr = (struct sockaddr_in *)addr;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(tcp_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
        int port = ntohs(tcp_addr->sin_port);
        printf("[LIBDESHOOK] R%d intercepted bind() to %s:%d. Allowing real bind.\n", my_router_id, ip_str, port);
    }
    return real_bind(sockfd, addr, addrlen);
}

// listen - 向 desd 通知开始监听
int listen(int sockfd, int backlog) {
    printf("[LIBDESHOOK] R%d listen() called: sockfd=%d\n", my_router_id, sockfd);
    fflush(stdout);
    
    if (desd_control_socket_fd == -1) {
        return real_listen(sockfd, backlog);
    }

    // 首先获取socket地址，判断是否需要过滤
    struct sockaddr_storage addr_storage;
    socklen_t addr_len = sizeof(addr_storage);
    if (getsockname(sockfd, (struct sockaddr *)&addr_storage, &addr_len) == 0) {
        if (addr_storage.ss_family == AF_UNIX) {
            struct sockaddr_un *un_addr = (struct sockaddr_un *)&addr_storage;
            // 过滤BIRD内部控制socket和其他内部通信
            if (strstr(un_addr->sun_path, "/run/bird") != NULL || 
                strstr(un_addr->sun_path, "bird.ctl") != NULL) {
                printf("[LIBDESHOOK] R%d skipping listen() on internal socket: %s\n", 
                       my_router_id, un_addr->sun_path);
                return real_listen(sockfd, backlog);
            }
        }
    }

    printf("[LIBDESHOOK] R%d intercepted listen() on sockfd %d.\n", my_router_id, sockfd);

    // 首先执行真实的 listen()
    int result = real_listen(sockfd, backlog);
    if (result != 0) {
        // listen 失败，直接返回
        return result;
    }

    // listen 成功后，获取 socket 绑定的地址（支持 UDS 和 TCP）
    if (getsockname(sockfd, (struct sockaddr *)&addr_storage, &addr_len) != 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d failed to get socket name for fd %d.\n", my_router_id, sockfd);
        return result; // listen 已经成功，所以返回成功
    }

    // 构建抽象地址字符串
    char listen_address[256];
    if (addr_storage.ss_family == AF_UNIX) {
        struct sockaddr_un *un_addr = (struct sockaddr_un *)&addr_storage;
        strncpy(listen_address, un_addr->sun_path, sizeof(listen_address) - 1);
        listen_address[sizeof(listen_address) - 1] = '\0';
    } else if (addr_storage.ss_family == AF_INET) {
        struct sockaddr_in *tcp_addr = (struct sockaddr_in *)&addr_storage;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(tcp_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
        int port = ntohs(tcp_addr->sin_port);
        snprintf(listen_address, sizeof(listen_address), "%s:%d", ip_str, port);
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d unsupported address family for listen.\n", my_router_id);
        return result;
    }

    // 向 desd 发送 LISTEN_EVENT
    Message listen_msg;
    memset(&listen_msg, 0, sizeof(Message));
    listen_msg.message_type = HOOK_TO_DESD;
    listen_msg.router_id = my_router_id;
    listen_msg.event_type = LISTEN_EVENT;
    listen_msg.virtual_time = current_virtual_time;
    generate_request_id(listen_msg.request_id);

    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "listen_address", json_string(listen_address));
    json_object_set_new(payload_obj, "socket_fd", json_integer(sockfd));
    json_object_set_new(payload_obj, "request_id", json_string(listen_msg.request_id));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(listen_msg.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    listen_msg.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

    Message listen_resp;
    if (send_msg_to_desd_and_wait_for_response(&listen_msg, &listen_resp)) {
        json_error_t error;
        json_t *resp_payload_obj = json_loads(listen_resp.payload.json_str, 0, &error);
        json_t *status_json = NULL;
        if (resp_payload_obj) {
            status_json = json_object_get(resp_payload_obj, "status");
        }

        if (status_json && json_is_string(status_json) &&
            strcmp(json_string_value(status_json), "SUCCESS") == 0) {
            printf("[LIBDESHOOK] R%d listen() registered with DESD on %s.\n", my_router_id, listen_address);
            if (resp_payload_obj) json_decref(resp_payload_obj);
        } else {
            fprintf(stderr, "[LIBDESHOOK WARNING] R%d listen() registration with DESD failed.\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
        }
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d listen() failed to communicate with desd.\n", my_router_id);
    }

    return result;
}

// unlink - 简单拦截，不与DESD交互
int unlink(const char *pathname) {
    if (strcmp(pathname, ROUTER_SOCKET_PATH) == 0) {
        printf("[LIBDESHOOK] R%d intercepted unlink() for %s. Allowing real unlink.\n", my_router_id, ROUTER_SOCKET_PATH);
    }
    return real_unlink(pathname);
}

// socket - 简单拦截，不与DESD交互，但需要标记socket FD
int socket(int domain, int type, int protocol) {
    int fd = real_socket(domain, type, protocol);
    const char* domain_str = (domain == AF_INET) ? "AF_INET" : 
                             (domain == AF_INET6) ? "AF_INET6" :
                             (domain == AF_UNIX) ? "AF_UNIX" :
                             (domain == 16) ? "AF_NETLINK" : "OTHER";
    printf("[LIBDESHOOK] R%d intercepted socket() call. Created fd: %d, domain: %d (%s), type: %d.\n", 
           my_router_id, fd, domain, domain_str, type);
    fflush(stdout);
    
    // 🔒 线程安全：标记AF_INET/AF_INET6的socket（需要DES管理）
    if (fd >= 0 && fd < MAX_TRACKED_FDS && (domain == AF_INET || domain == AF_INET6)) {
        pthread_mutex_lock(&socket_fds_mutex);
        socket_fds[fd] = 1;
        pthread_mutex_unlock(&socket_fds_mutex);
        printf("[LIBDESHOOK] R%d marked fd %d as DES-managed socket.\n", my_router_id, fd);
    }
    
    return fd;
}

// sleep - 基于虚拟时间的睡眠
unsigned int sleep(unsigned int seconds) {
    if (desd_control_socket_fd == -1) {
        return real_sleep(seconds);
    }

    printf("[LIBDESHOOK] R%d intercepted sleep(%u seconds).\n", my_router_id, seconds);

    // 向desd注册阻塞请求（SLEEP_CALL）
    Message sleep_block_req;
    memset(&sleep_block_req, 0, sizeof(Message));
    sleep_block_req.message_type = HOOK_TO_DESD;
    sleep_block_req.router_id = my_router_id;
    sleep_block_req.event_type = ROUTER_BLOCK_REQUEST;
    sleep_block_req.virtual_time = current_virtual_time;
    generate_request_id(sleep_block_req.request_id);

    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "blocked_function", json_string("SLEEP_CALL"));
    json_object_set_new(payload_obj, "request_id", json_string(sleep_block_req.request_id));
    json_object_set_new(payload_obj, "sleep_seconds", json_integer(seconds));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(sleep_block_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    sleep_block_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

    Message sleep_block_resp;
    if (send_msg_to_desd_and_wait_for_response(&sleep_block_req, &sleep_block_resp)) {
        json_error_t error;
        json_t *resp_payload_obj = json_loads(sleep_block_resp.payload.json_str, 0, &error);
        
        json_t *status_json = NULL;
        if (resp_payload_obj) {
            status_json = json_object_get(resp_payload_obj, "status");
        }
        
        if (status_json && json_is_string(status_json) &&
            strcmp(json_string_value(status_json), "SUCCESS") == 0) {
            
            printf("[LIBDESHOOK] R%d sleep() completed (virtual time advanced by %u seconds).\n", my_router_id, seconds);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return 0;  // 成功睡眠
        } else {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d sleep() failed (DESD rejected or error).\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return seconds;  // 返回未睡眠的秒数
        }
    } else {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d sleep() failed: No response from desd.\n", my_router_id);
        return seconds;
    }
}

// fcntl - 拦截以跟踪非阻塞标志
int fcntl(int fd, int cmd, ...) {
    va_list args;
    va_start(args, cmd);
    
    // 对于F_SETFL和F_GETFL，需要特殊处理
    int result = 0;
    
    if (cmd == F_GETFL) {
        // 获取文件状态标志（不需要参数）
        result = real_fcntl(fd, cmd);
        printf("[LIBDESHOOK] R%d fcntl(F_GETFL) on fd %d: flags=0x%x.\n", my_router_id, fd, result);
    } else if (cmd == F_SETFL) {
        // 设置文件状态标志（需要int参数）
        int flags = va_arg(args, int);
        result = real_fcntl(fd, cmd, flags);
        
        printf("[LIBDESHOOK] R%d fcntl(F_SETFL) on fd %d: flags=0x%x.\n", my_router_id, fd, flags);
        
        // 跟踪非阻塞标志
        if (fd >= 0 && fd < MAX_TRACKED_FDS && socket_fds[fd]) {
            if (flags & O_NONBLOCK) {
                nonblocking_fds[fd] = 1;
                printf("[LIBDESHOOK] R%d marked fd %d as NON-BLOCKING.\n", my_router_id, fd);
            } else {
                nonblocking_fds[fd] = 0;
                printf("[LIBDESHOOK] R%d marked fd %d as BLOCKING.\n", my_router_id, fd);
            }
        }
    } else if (cmd == F_SETFD || cmd == F_GETFD) {
        // F_SETFD需要int参数，F_GETFD不需要
        if (cmd == F_SETFD) {
            int flags = va_arg(args, int);
            result = real_fcntl(fd, cmd, flags);
        } else {
            result = real_fcntl(fd, cmd);
        }
    } else {
        // 其他fcntl命令，假设需要一个long参数（最常见情况）
        long arg = va_arg(args, long);
        result = real_fcntl(fd, cmd, arg);
    }
    
    va_end(args);
    return result;
}

// read - 拦截并转发socket读取到recv()
ssize_t read(int fd, void *buf, size_t count) {
    // 1. desd控制socket：不拦截
    if (fd == desd_control_socket_fd) {
        return real_read(fd, buf, count);
    }
    
    // 2. 未初始化DES：不拦截
    if (desd_control_socket_fd == -1) {
        return real_read(fd, buf, count);
    }
    
    // 3. 检查是否是DES管理的socket
    if (fd >= 0 && fd < MAX_TRACKED_FDS && socket_fds[fd]) {
        // 这是DES管理的socket，转发到recv()
        printf("[LIBDESHOOK] R%d read() on socket fd=%d, forwarding to recv().\n", my_router_id, fd);
        return recv(fd, buf, count, 0);
    }
    
    // 4. 普通文件或其他FD：不拦截
    return real_read(fd, buf, count);
}

// write - 拦截并转发socket写入到send()
ssize_t write(int fd, const void *buf, size_t count) {
    // 1. desd控制socket：不拦截
    if (fd == desd_control_socket_fd) {
        return real_write(fd, buf, count);
    }
    
    // 2. 未初始化DES：不拦截
    if (desd_control_socket_fd == -1) {
        return real_write(fd, buf, count);
    }
    
    // 3. 检查是否是DES管理的socket
    if (fd >= 0 && fd < MAX_TRACKED_FDS && socket_fds[fd]) {
        // 这是DES管理的socket，转发到send()
        printf("[LIBDESHOOK] R%d write() on socket fd=%d, forwarding to send().\n", my_router_id, fd);
        return send(fd, buf, count, 0);
    }
    
    // 4. 普通文件或其他FD：不拦截
    return real_write(fd, buf, count);
}

// clock_gettime - 拦截并向desd查询虚拟时间
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    // 1. 未初始化DES：使用真实时间
    if (desd_control_socket_fd == -1 || !real_clock_gettime) {
        if (real_clock_gettime) {
            return real_clock_gettime(clk_id, tp);
        }
        errno = ENOSYS;
        return -1;
    }
    
    // 2. 只拦截CLOCK_MONOTONIC（BIRD使用的时钟）
    if (clk_id == CLOCK_MONOTONIC) {
        // 向desd发送GET_VIRTUAL_TIME_EVENT请求
        Message req = {
            .message_type = HOOK_TO_DESD,
            .router_id = my_router_id,
            .event_type = GET_VIRTUAL_TIME_EVENT,
            .virtual_time = 0.0
        };
        generate_request_id(req.request_id);
        
        // 构建payload
        json_t *payload_obj = json_object();
        json_object_set_new(payload_obj, "request_id", json_string(req.request_id));
        char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
        json_decref(payload_obj);
        
        strncpy(req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
        req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
        free(payload_str);
        
        // 临时开启clock_gettime日志用于调试
        static int clock_call_count = 0;
        if ((++clock_call_count % 10) == 0) {  // 每10次打印一次
            printf("[LIBDESHOOK] R%d clock_gettime(CLOCK_MONOTONIC) called (count: %d)\n",
                   my_router_id, clock_call_count);
            fflush(stdout);
        }
        
        // 发送请求并等待响应
        Message resp;
        if (!send_msg_to_desd_and_wait_for_response(&req, &resp)) {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d clock_gettime() failed to get virtual time from desd.\n", 
                    my_router_id);
            fflush(stderr);
            // 降级到真实时间
            return real_clock_gettime(clk_id, tp);
        }
        
        // 移除clock_gettime响应日志
        
        // 解析响应中的虚拟时间
        json_error_t error;
        json_t *resp_payload_obj = json_loads(resp.payload.json_str, 0, &error);
        if (!resp_payload_obj) {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d clock_gettime() failed to parse response.\n", 
                    my_router_id);
            fflush(stderr);
            return real_clock_gettime(clk_id, tp);
        }
        
        json_t *vtime_json = json_object_get(resp_payload_obj, "current_virtual_time");
        if (!vtime_json || !json_is_number(vtime_json)) {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d clock_gettime() no virtual time in response.\n", 
                    my_router_id);
            fflush(stderr);
            json_decref(resp_payload_obj);
            return real_clock_gettime(clk_id, tp);
        }
        
        double vtime_sec = json_number_value(vtime_json);
        json_decref(resp_payload_obj);
        
        total_clock_calls++;
        
        // 临时开启返回值日志 + 检测时间跳跃
        if ((clock_call_count % 10) == 0) {
            double time_delta = (last_reported_vtime >= 0) ? (vtime_sec - last_reported_vtime) : 0;
            printf("[LIBDESHOOK] R%d clock_gettime() returning VT=%.3f (delta=%.3f, total_calls=%llu)\n",
                   my_router_id, vtime_sec, time_delta, total_clock_calls);
            fflush(stdout);
            last_reported_vtime = vtime_sec;
        }
        
        // 转换为timespec
        if (tp) {
            tp->tv_sec = (time_t)vtime_sec;
            tp->tv_nsec = (long)((vtime_sec - tp->tv_sec) * 1000000000);
        }
        
        return 0;
    }
    
    // 3. 其他时钟类型使用真实时间
    return real_clock_gettime(clk_id, tp);
}
