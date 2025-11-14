#define _GNU_SOURCE // Required for RTLD_NEXT and dlsym

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>      // For dlsym
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <jansson.h>    // For JSON handling
#include <sys/time.h> // For select timeout
#include <poll.h> // For poll to replace select for better handling of real FDs

// --- Global State ---
int my_router_id = -1;
int desd_control_socket_fd = -1;

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
// static int (*real_select)(int, fd_set *, fd_set *, fd_set *, struct timeval *) = NULL; // 不再使用select，改用poll
static int (*real_poll)(struct pollfd *, nfds_t, int) = NULL;

void generate_request_id(char* id_buf); // Function prototype for generate_request_id

// Function prototypes for intercepted functions
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
// int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout); // 不再使用select
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

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
    real_poll = dlsym(RTLD_NEXT, "poll"); // 使用poll

    if (!real_socket || !real_connect || !real_send || !real_recv || !real_close ||
        !real_bind || !real_listen || !real_accept || !real_unlink || !real_poll) {
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
    snprintf(id_buf, 64, "req_%lu_%d", next_request_id_val++, my_router_id);
}

// Helper function to send message to desd and wait for response
// 这个函数是libdeshook与desd之间同步通信的核心，路由器在此处阻塞
int send_msg_to_desd_and_wait_for_response(const Message* req_msg, Message* resp_msg_out) {
    // Send the request message
    char *json_str = message_to_json(req_msg);
    if (!json_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to serialize request message.\n");
        return 0;
    }
    
    // Allocate space for newline + null terminator
    size_t json_len = strlen(json_str);
    char *json_with_newline = (char*)malloc(json_len + 2); // +1 for '\n', +1 for '\0'
    if (!json_with_newline) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to allocate memory for message.\n");
        free(json_str);
        return 0;
    }
    strcpy(json_with_newline, json_str);
    json_with_newline[json_len] = '\n';
    json_with_newline[json_len + 1] = '\0';
    free(json_str);
    
    if (real_send(desd_control_socket_fd, json_with_newline, strlen(json_with_newline), 0) < 0) {
        perror("[LIBDESHOOK ERROR] send_msg_to_desd_and_wait_for_response - send");
        free(json_with_newline);
        return 0;
    }
    free(json_with_newline);

    // Now, synchronously wait for the response from desd
    char buffer[MAX_MSG_SIZE];
    ssize_t bytes_received = real_recv(desd_control_socket_fd, buffer, MAX_MSG_SIZE - 1, 0);
    
    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            fprintf(stderr, "[LIBDESHOOK ERROR] DESD disconnected during response wait.\n");
        } else {
            perror("[LIBDESHOOK ERROR] send_msg_to_desd_and_wait_for_response - recv");
        }
        return 0;
    }
    buffer[bytes_received] = '\0';

    json_to_message(buffer, resp_msg_out);

    // For safety, ensure the response matches the request_id. 
    // In a strict single-threaded DES, this should always be the case if no other messages intervene.
    if (strcmp(req_msg->request_id, resp_msg_out->request_id) != 0) {
        fprintf(stderr, "[LIBDESHOOK WARNING] Response request_id mismatch! Expected %s, Got %s.\n",
                req_msg->request_id, resp_msg_out->request_id);
        // We might still process it if it's the only message.
    }
    return 1; // Success
}

// --- Intercepted Functions ---

// connect
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    // 如果desd未连接，或者不是针对ROUTER_SOCKET_PATH的连接，则直接调用真实connect
    if (desd_control_socket_fd == -1 || addr->sa_family != AF_UNIX ||
        (addr->sa_family == AF_UNIX && strcmp(((struct sockaddr_un *)addr)->sun_path, ROUTER_SOCKET_PATH) != 0)) {
        return real_connect(sockfd, addr, addrlen);
    }

    printf("[LIBDESHOOK] R%d intercepted connect() to %s.\n", my_router_id, ROUTER_SOCKET_PATH);

    // 1. 告知desd：发送CONNECT_REQUEST_EVENT事件
    Message connect_req;
    memset(&connect_req, 0, sizeof(Message));
    connect_req.message_type = HOOK_TO_DESD;
    connect_req.router_id = my_router_id;
    connect_req.event_type = CONNECT_REQUEST_EVENT;
    connect_req.virtual_time = current_virtual_time;
    generate_request_id(connect_req.request_id);
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "destination_abstract_address", json_string(ROUTER_SOCKET_PATH));
    json_object_set_new(payload_obj, "request_id", json_string(connect_req.request_id));
    json_object_set_new(payload_obj, "socket_fd", json_integer(sockfd));  // 添加 socket_fd
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(connect_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    connect_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);

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
                   my_router_id, ROUTER_SOCKET_PATH);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            // 2. DESD确认连接建立事件后，调用真实的connect()
            return real_connect(sockfd, addr, addrlen);

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

    printf("[LIBDESHOOK] R%d intercepted send() on sockfd %d, len %zu.\n", my_router_id, sockfd, len);

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
    json_object_set_new(payload_obj, "destination_abstract_address", json_string(ROUTER_SOCKET_PATH)); // 简化处理，假设发往ROUTER_SOCKET_PATH
    json_object_set_new(payload_obj, "request_id", json_string(send_req.request_id));
    json_object_set_new(payload_obj, "bytes_sent", json_integer(len)); // Use len, not bytes_sent from real_send yet
    json_object_set_new(payload_obj, "socket_fd", json_integer(sockfd));  // 添加 socket_fd
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

            printf("[LIBDESHOOK] R%d send() unblocked by DESD. Now performing real send.\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);

            // 3. DESD确认后，调用真实的send()
            ssize_t bytes_sent = real_send(sockfd, buf, len, flags);
            return bytes_sent;

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

    printf("[LIBDESHOOK] R%d intercepted recv() on sockfd %d, max len %zu.\n", my_router_id, sockfd, len);

    // 直接向desd注册阻塞请求（不再进行非阻塞尝试）
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
            
            printf("[LIBDESHOOK] R%d recv() unblocked by DESD. Now performing real recv.\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            // DESD解除阻塞后，再次尝试从真实socket读取数据 (此时应该有数据了)
            // 注意：这里仍然需要设置超时或非阻塞模式来避免死锁，但理想情况下DESD会确保数据已到
            fprintf(stderr, "before real_recv\n");
            ssize_t final_bytes_received = real_recv(sockfd, buf, len, flags); // 再次调用真实recv
            if (final_bytes_received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                fprintf(stderr, "[LIBDESHOOK ERROR] R%d recv() unblocked by DESD but no data immediately available.\n", my_router_id);
                // 这种情况理论上不应该发生，除非DESD调度错误或数据丢失
                errno = EIO; // 模拟I/O错误
            }
            return final_bytes_received;

        } else if (resp_payload_obj && json_string_value(json_object_get(resp_payload_obj, "status")) &&
                   strcmp(json_string_value(json_object_get(resp_payload_obj, "status")), "TIMEOUT") == 0) {
            printf("[LIBDESHOOK] R%d recv() timed out.\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            errno = EAGAIN; // Resource temporarily unavailable (timeout)
            return -1;
        }else {
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

// poll (取代select)
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (desd_control_socket_fd == -1) {
        return real_poll(fds, nfds, timeout);
    }

    printf("[LIBDESHOOK] R%d intercepted poll() with timeout %d ms.\n", my_router_id, timeout);

    // 1. 尝试非阻塞地调用真实的poll，看是否有FD立即可用
    int real_poll_result = real_poll(fds, nfds, 0); // 非阻塞调用
    if (real_poll_result != 0) {
        printf("[LIBDESHOOK] R%d poll() immediately returned %d ready FDs from real poll.\n", my_router_id, real_poll_result);
        return real_poll_result;
    }

    // 如果没有FD立即可用，且有超时或需要阻塞，则向desd注册阻塞请求
    if (timeout == 0) { // 非阻塞poll且没有立即就绪的FD
        return 0;
    }

    Message poll_block_req;
    memset(&poll_block_req, 0, sizeof(Message));
    poll_block_req.message_type = HOOK_TO_DESD;
    poll_block_req.router_id = my_router_id;
    poll_block_req.event_type = ROUTER_BLOCK_REQUEST;
    poll_block_req.virtual_time = current_virtual_time;
    generate_request_id(poll_block_req.request_id);

    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "blocked_function", json_string("SELECT_CALL")); // 命名为SELECT_CALL以兼容EventType
    json_object_set_new(payload_obj, "request_id", json_string(poll_block_req.request_id));
    if (timeout > 0) {
        json_object_set_new(payload_obj, "timeout_ms", json_integer(timeout));
    }
    // 传递pollfd信息会更复杂，这里简化，假设desd知道哪些FD是相关的
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
            
            printf("[LIBDESHOOK] R%d poll() unblocked by DESD. Now performing real poll with specified timeout.\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);

            // DESD解除阻塞后，调用真实的poll
            // 此时，需要根据DESD的调度，重新设置pollfd的revents
            // 这里简化为再次调用真实poll，并期望有FD就绪
            // 理想情况下，DESD会在响应中指示哪些FD就绪
            return real_poll(fds, nfds, 0); // 再次非阻塞调用，期望返回就绪FD数量

        } else if (resp_payload_obj && json_string_value(json_object_get(resp_payload_obj, "status")) &&
                   strcmp(json_string_value(json_object_get(resp_payload_obj, "status")), "TIMEOUT") == 0) {
            printf("[LIBDESHOOK] R%d poll() timed out (DESD confirmed).\n", my_router_id);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return 0; // poll timeout returns 0
        }else {
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

// close - 简单拦截，不与DESD交互
int close(int sockfd) {
    printf("[LIBDESHOOK] R%d intercepted close() for sockfd %d.\n", my_router_id, sockfd);
    // 理论上可以通知DESD，但对于简单仿真可以忽略
    return real_close(sockfd);
}

// bind - 简单拦截，不与DESD交互
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (addr->sa_family == AF_UNIX) {
        struct sockaddr_un *un_addr = (struct sockaddr_un *)addr;
        if (strcmp(un_addr->sun_path, ROUTER_SOCKET_PATH) == 0) {
            printf("[LIBDESHOOK] R%d intercepted bind() to %s. Allowing real bind.\n", my_router_id, ROUTER_SOCKET_PATH);
        }
    }
    return real_bind(sockfd, addr, addrlen);
}

// listen - 向 desd 通知开始监听
int listen(int sockfd, int backlog) {
    if (desd_control_socket_fd == -1) {
        return real_listen(sockfd, backlog);
    }

    printf("[LIBDESHOOK] R%d intercepted listen() on sockfd %d.\n", my_router_id, sockfd);

    // 首先执行真实的 listen()
    int result = real_listen(sockfd, backlog);
    if (result != 0) {
        // listen 失败，直接返回
        return result;
    }

    // listen 成功后，获取 socket 绑定的地址
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(sockfd, (struct sockaddr *)&addr, &addr_len) != 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d failed to get socket name for fd %d.\n", my_router_id, sockfd);
        return result; // listen 已经成功，所以返回成功
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
    json_object_set_new(payload_obj, "listen_address", json_string(addr.sun_path));
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
            printf("[LIBDESHOOK] R%d listen() registered with DESD on %s.\n", my_router_id, addr.sun_path);
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

// socket - 简单拦截，不与DESD交互
int socket(int domain, int type, int protocol) {
    int fd = real_socket(domain, type, protocol);
    printf("[LIBDESHOOK] R%d intercepted socket() call. Created fd: %d.\n", my_router_id, fd);
    return fd;
}
