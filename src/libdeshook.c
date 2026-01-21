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
#include <stdint.h> // For uintptr_t

// --- Global State ---
#define MAX_TRACKED_FDS 1024
static int socket_fds[MAX_TRACKED_FDS] = {0}; // 1表示是socket (仅AF_INET/AF_INET6)
static int nonblocking_fds[MAX_TRACKED_FDS] = {0}; // 1表示设置了O_NONBLOCK
static int connecting_fds[MAX_TRACKED_FDS] = {0}; // 1表示已发送过CONNECT_REQUEST（正在连接中）
int my_router_id = -1;
static unsigned long long total_clock_calls = 0;
static double last_reported_vtime = -1.0;

// --- Per-Thread State ---
// 每个线程有自己的与 DESD 的通信通道
// 接收缓冲区大小（足够容纳多条消息）
#define DESD_RECV_BUF_SIZE (MAX_MSG_SIZE * 3)

typedef struct {
    int desd_socket_fd;     // 该线程与 DESD 的通信 socket
    int thread_id;          // 线程 ID（在该 router 内从 0 开始编号）
    int is_registered;      // 是否已向 DESD 注册
    int registration_in_progress;
    // 按行拆包缓冲区：解决 DESD→HOOK 方向的粘包/半包问题
    char recv_buf[DESD_RECV_BUF_SIZE];  // 接收缓冲区
    size_t recv_len;                     // 缓冲区中有效字节数
} ThreadState;

static pthread_key_t thread_state_key;      // TLS key for per-thread state
static pthread_once_t thread_key_once = PTHREAD_ONCE_INIT;
static int next_thread_id = 0;              // 下一个可用的线程 ID
static pthread_mutex_t thread_id_mutex = PTHREAD_MUTEX_INITIALIZER;

// 为了兼容性，保留一个全局变量（但实际上每个线程使用自己的）
int desd_control_socket_fd = -1;  // 已废弃，但保留用于编译兼容

// --- Thread Safety ---
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

// Mutex hook 相关函数指针
static int (*real_pthread_mutex_lock)(pthread_mutex_t *) = NULL;
static int (*real_pthread_mutex_trylock)(pthread_mutex_t *) = NULL;
static int (*real_pthread_mutex_unlock)(pthread_mutex_t *) = NULL;

// Mutex hook 控制：只 hook BIRD 协议相关的 mutex，避免 hook 内部控制用的 mutex
static __thread int in_mutex_hook = 0;  // 防止递归 hook

void generate_request_id(char* id_buf); // Function prototype for generate_request_id

// Forward declaration for read_one_desd_response (used by ensure_thread_registered)
static int read_one_desd_response(ThreadState *state, Message *msg);

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

// --- Per-Thread DESD Connection Management ---

// TLS key destructor: cleanup when thread exits
static void thread_state_destructor(void *ptr) {
    ThreadState *state = (ThreadState *)ptr;
    if (state) {
        if (state->desd_socket_fd >= 0) {
            fprintf(stderr,
                    "[LIBDESHOOK DEBUG] R%d T%d thread_state_destructor: closing DESD control socket fd=%d on thread exit.\n",
                    my_router_id,
                    state->thread_id,
                    state->desd_socket_fd);
            fflush(stderr);
            real_close(state->desd_socket_fd);
        }
        free(state);
    }
}

// Initialize the TLS key (called once per process)
static void init_thread_key(void) {
    pthread_key_create(&thread_state_key, thread_state_destructor);
}

// Get or create ThreadState for current thread
static ThreadState* get_thread_state(void) {
    pthread_once(&thread_key_once, init_thread_key);
    
    ThreadState *state = (ThreadState *)pthread_getspecific(thread_state_key);
    if (state == NULL) {
        // First time this thread is accessing - allocate and initialize
        state = (ThreadState *)calloc(1, sizeof(ThreadState));
        if (!state) {
            fprintf(stderr, "[LIBDESHOOK ERROR] Failed to allocate ThreadState\n");
            return NULL;
        }
        state->desd_socket_fd = -1;
        state->is_registered = 0;
        state->registration_in_progress = 0;
        // 初始化接收缓冲区（calloc 已清零，这里显式设置 recv_len）
        state->recv_len = 0;
        
        // Assign a unique thread_id within this router
        pthread_mutex_lock(&thread_id_mutex);
        state->thread_id = next_thread_id++;
        pthread_mutex_unlock(&thread_id_mutex);
        
        pthread_setspecific(thread_state_key, state);
    }
    return state;
}

// Connect this thread to DESD and register it
static int ensure_thread_registered(void) {
    ThreadState *state = get_thread_state();
    if (!state) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d ensure_thread_registered: get_thread_state returned NULL\n",
                my_router_id);
        fflush(stderr);
        return 0;
    }

    if (state->registration_in_progress) {
        return 0;
    }
    
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: entered, is_registered=%d, current_fd=%d\n",
            my_router_id, state->thread_id, state->is_registered, state->desd_socket_fd);
    fflush(stderr);
    
    if (state->is_registered) {
        fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: already registered, returning existing state\n",
                my_router_id, state->thread_id);
        fflush(stderr);
        return 1;  // Already registered
    }

    state->registration_in_progress = 1;
    
    // Create socket and connect to DESD
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: creating AF_UNIX socket for DESD control\n",
            my_router_id, state->thread_id);
    fflush(stderr);
    state->desd_socket_fd = real_socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->desd_socket_fd < 0) {
        perror("[LIBDESHOOK ERROR] socket for desd control (per-thread)");
        fflush(stderr);
        state->registration_in_progress = 0;
        return 0;
    }
    
    struct sockaddr_un desd_addr;
    memset(&desd_addr, 0, sizeof(desd_addr));
    desd_addr.sun_family = AF_UNIX;
    strncpy(desd_addr.sun_path, DESD_CONTROL_SOCKET_PATH, sizeof(desd_addr.sun_path) - 1);
    
    fprintf(stderr, "[LIBDESHOOK DEBUG] R%d T%d ensure_thread_registered: created control socket fd=%d, connecting to %s\n",
            my_router_id, state->thread_id, state->desd_socket_fd, DESD_CONTROL_SOCKET_PATH);
    fflush(stderr);
    
    if (real_connect(state->desd_socket_fd, (struct sockaddr*)&desd_addr, sizeof(desd_addr)) < 0) {
        perror("[LIBDESHOOK ERROR] connect to desd control (per-thread)");
        fflush(stderr);
        real_close(state->desd_socket_fd);
        state->desd_socket_fd = -1;
        state->registration_in_progress = 0;
        return 0;
    }
    
    fprintf(stderr, "[LIBDESHOOK DEBUG] R%d T%d ensure_thread_registered: connected to desd control socket (fd=%d)\n", 
            my_router_id, state->thread_id, state->desd_socket_fd);
    fflush(stderr);
    
    // Register this thread with DESD by sending a ROUTER_START event
    Message register_req;
    memset(&register_req, 0, sizeof(Message));
    register_req.message_type = HOOK_TO_DESD;
    register_req.router_id = my_router_id;
    register_req.thread_id = state->thread_id;
    register_req.event_type = ROUTER_START;
    register_req.virtual_time = 0.0;
    
    // Include thread_id in payload for DESD to identify this thread
    json_t *payload = json_object();
    json_object_set_new(payload, "thread_id", json_integer(state->thread_id));
    char *payload_str = json_dumps(payload, JSON_COMPACT);
    if (!payload_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d T%d ensure_thread_registered: json_dumps for payload returned NULL\n",
                my_router_id, state->thread_id);
        fflush(stderr);
        json_decref(payload);
        state->registration_in_progress = 0;
        return 0;
    }
    strncpy(register_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    register_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: payload JSON='%s'\n",
            my_router_id, state->thread_id, register_req.payload.json_str);
    fflush(stderr);
    free(payload_str);
    json_decref(payload);
    
    generate_request_id(register_req.request_id);
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: generated request_id='%s'\n",
            my_router_id, state->thread_id, register_req.request_id);
    fflush(stderr);
    
    // Send registration request directly (can't use send_msg_to_desd_and_wait_for_response yet)
    char *json_str = message_to_json(&register_req);
    if (!json_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d T%d ensure_thread_registered: message_to_json returned NULL\n",
                my_router_id, state->thread_id);
        fflush(stderr);
        state->registration_in_progress = 0;
        return 0;
    }
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: serialized register_req JSON (without newline) len=%zu\n",
            my_router_id, state->thread_id, strlen(json_str));
    fflush(stderr);
    
    size_t json_len = strlen(json_str);
    char *json_with_newline = (char*)malloc(json_len + 2);
    if (!json_with_newline) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d T%d ensure_thread_registered: failed to allocate %zu bytes for json_with_newline\n",
                my_router_id, state->thread_id, json_len + 2);
        fflush(stderr);
        free(json_str);
        state->registration_in_progress = 0;
        return 0;
    }
    strcpy(json_with_newline, json_str);
    json_with_newline[json_len] = '\n';
    json_with_newline[json_len + 1] = '\0';
    free(json_str);
    
    size_t send_len = strlen(json_with_newline);
    fprintf(stderr, "[LIBDESHOOK DEBUG] R%d T%d ensure_thread_registered: sending ROUTER_START (len=%zu bytes) on fd=%d, request_id=%s\n",
            my_router_id, state->thread_id, send_len, state->desd_socket_fd, register_req.request_id);
    fflush(stderr);

    ssize_t sent = real_send(state->desd_socket_fd, json_with_newline, send_len, 0);
    free(json_with_newline);
    
    if (sent < 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d T%d ensure_thread_registered: send() failed (errno=%d)\n",
                my_router_id, state->thread_id, errno);
        perror("[LIBDESHOOK ERROR] send registration to desd");
        fflush(stderr);
        state->registration_in_progress = 0;
        return 0;
    }
    if ((size_t)sent != send_len) {
        fprintf(stderr, "[LIBDESHOOK WARNING] R%d T%d ensure_thread_registered: partial send, sent=%zd, expected=%zu\n",
                my_router_id, state->thread_id, sent, send_len);
        fflush(stderr);
    }
    fprintf(stderr, "[LIBDESHOOK DEBUG] R%d T%d ensure_thread_registered: ROUTER_START sent (%zd bytes) on fd=%d, waiting for response...\n",
            my_router_id, state->thread_id, sent, state->desd_socket_fd);
    fflush(stderr);
    
    // Wait for registration response (使用按行拆包的 helper)
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: waiting for registration response using read_one_desd_response\n",
            my_router_id, state->thread_id);
    fflush(stderr);
    
    Message register_resp;
    int read_result = read_one_desd_response(state, &register_resp);
    if (read_result != 1) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d T%d Failed to receive registration response (read_result=%d)\n",
                my_router_id, state->thread_id, read_result);
        fflush(stderr);
        state->registration_in_progress = 0;
        return 0;
    }
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: received registration response via helper\n",
            my_router_id, state->thread_id);
    fflush(stderr);
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: parsed response message_type=%d router_id=%d thread_id=%d request_id='%s'\n",
            my_router_id, state->thread_id,
            register_resp.message_type, register_resp.router_id,
            register_resp.thread_id, register_resp.request_id);
    fflush(stderr);
    
    json_error_t error;
    json_t *resp_payload = json_loads(register_resp.payload.json_str, 0, &error);
    if (!resp_payload) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d T%d ensure_thread_registered: json_loads failed on response payload (text='%s', line=%d, column=%d)\n",
                my_router_id, state->thread_id,
                error.text, error.line, error.column);
        fflush(stderr);
        state->registration_in_progress = 0;
        return 0;
    }
    const char *status_str = json_string_value(json_object_get(resp_payload, "status"));
    fprintf(stderr, "[LIBDESHOOK TRACE] R%d T%d ensure_thread_registered: response payload status='%s'\n",
            my_router_id, state->thread_id, status_str ? status_str : "(null)");
    fflush(stderr);
    if (status_str && strcmp(status_str, "SUCCESS") == 0) {
        fprintf(stderr, "[LIBDESHOOK DEBUG] R%d T%d registration SUCCESS.\n", 
                my_router_id, state->thread_id);
        fflush(stderr);
        state->is_registered = 1;
        state->registration_in_progress = 0;
        json_decref(resp_payload);
        return 1;
    } else {
        const char *err_str = json_string_value(json_object_get(resp_payload, "error_message"));
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d T%d registration failed: %s\n", 
                my_router_id, state->thread_id,
                err_str ? err_str : "Unknown");
        fflush(stderr);
        state->registration_in_progress = 0;
        json_decref(resp_payload);
        return 0;
    }
}

// Get the current thread's DESD socket fd (ensures registration)
static int get_thread_desd_socket(void) {
    ThreadState *state = get_thread_state();
    if (!state) {
        return -1;
    }
    if (state->is_registered) {
        return state->desd_socket_fd;
    }
    if (state->registration_in_progress) {
        return -1;
    }
    if (!ensure_thread_registered()) {
        return -1;
    }
    state = get_thread_state();
    return state ? state->desd_socket_fd : -1;
}

// Get the current thread's ID
static int get_current_thread_id(void) {
    ThreadState *state = get_thread_state();
    return state ? state->thread_id : -1;
}

// --- libdeshook.so Initialization ---
__attribute__((constructor))
static void lib_init(void) {
    char *router_id_str = getenv("ROUTER_ID");
    if (!router_id_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] ROUTER_ID environment variable not set. Exiting.\n");
        fflush(stderr);
        _exit(1);
    }
    my_router_id = atoi(router_id_str);
    if (my_router_id <= 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Invalid ROUTER_ID: %s. Exiting.\n", router_id_str);
        fflush(stderr);
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
    
    // Mutex hook 函数指针
    real_pthread_mutex_lock = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_pthread_mutex_trylock = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_pthread_mutex_unlock = dlsym(RTLD_NEXT, "pthread_mutex_unlock");

    if (!real_socket || !real_connect || !real_send || !real_recv || !real_close ||
        !real_bind || !real_listen || !real_accept || !real_unlink || !real_select || 
        !real_poll || !real_sleep || !real_read || !real_write || !real_fcntl ||
        !real_clock_gettime || !real_pthread_mutex_lock || !real_pthread_mutex_trylock ||
        !real_pthread_mutex_unlock) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Error in dlsym: %s\n", dlerror());
        fflush(stderr);
        _exit(1);
    }
    
    // Initialize TLS key
    pthread_once(&thread_key_once, init_thread_key);
    
    // 强制注册主线程 (T0)
    // 确保每个 router 进程启动时就有一个线程注册到 DESD
    // 使 desd 的初始注册循环可以正常计数
    if (!ensure_thread_registered()) {
        fprintf(stderr, "[LIBDESHOOK FATAL] R%d T0 failed to register with DESD during lib_init. Exiting.\n", my_router_id);
        fflush(stderr);
        _exit(1);
    }
    
    fprintf(stderr, "[LIBDESHOOK DEBUG] R%d T0 registered with DESD during lib_init.\n", my_router_id);
    fflush(stderr);
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

// Helper function to send message to desd (without waiting for response)
// Returns 1 on success, 0 on failure
static int send_msg_to_desd(const Message* req_msg) {
    int thread_socket = get_thread_desd_socket();
    if (thread_socket < 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Thread not registered with DESD\n");
        return 0;
    }
    
    char *json_str = message_to_json(req_msg);
    if (!json_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to serialize request message.\n");
        return 0;
    }
    
    size_t json_len = strlen(json_str);
    char *json_with_newline = (char*)malloc(json_len + 2);
    if (!json_with_newline) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to allocate memory for message.\n");
        free(json_str);
        return 0;
    }
    strcpy(json_with_newline, json_str);
    json_with_newline[json_len] = '\n';
    json_with_newline[json_len + 1] = '\0';
    free(json_str);
    
    // 调试日志：记录即将发送到 DESD 的请求
    printf("[LIBDESHOOK-REQ] R%d T%d sending request to DESD (event_type=%d, req_id=%s, vt=%.6f)\n",
           my_router_id,
           get_current_thread_id(),
           req_msg->event_type,
           req_msg->request_id,
           current_virtual_time);
    fflush(stdout);

    ssize_t sent = real_send(thread_socket, json_with_newline, strlen(json_with_newline), 0);
    free(json_with_newline);
    
    if (sent < 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] send_msg_to_desd - send failed (errno=%d)\n", errno);
        return 0;
    }
    return 1;
}

// ============================================================================
// read_one_desd_response: 按行拆包的响应读取 helper
// ============================================================================
// 从 ThreadState 的接收缓冲区中读取一条完整的 JSON 响应（以 '\n' 为分隔符）。
// 
// 参数:
//   state  - 线程状态（包含 recv_buf 和 recv_len）
//   msg    - 输出参数，解析后的消息
//
// 返回值:
//   1  - 成功读取并解析了一条完整消息
//   0  - 没有完整消息可读（非阻塞情况下，或只有半包）
//   -1 - 错误或连接断开
// ============================================================================
static int read_one_desd_response(ThreadState *state, Message *msg) {
    if (!state || state->desd_socket_fd < 0) {
        return -1;
    }
    
    // 清空输出消息，防止解析失败时残留旧数据
    memset(msg, 0, sizeof(Message));
    msg->event_type = -1;  // 标记为未知类型
    
    while (1) {
        // Step 1: 在缓冲区中查找 '\n'
        char *newline_pos = memchr(state->recv_buf, '\n', state->recv_len);
        
        if (newline_pos != NULL) {
            // 找到完整的一行
            size_t line_len = newline_pos - state->recv_buf;
            
            // 提取这一行（不含 '\n'）到临时缓冲区
            char line_buf[MAX_MSG_SIZE];
            if (line_len >= MAX_MSG_SIZE) {
                // 行太长，协议错误
                fprintf(stderr, "[LIBDESHOOK ERROR] read_one_desd_response: line too long (%zu bytes)\n", line_len);
                // 丢弃这一行，继续处理
                size_t remaining = state->recv_len - line_len - 1;
                memmove(state->recv_buf, newline_pos + 1, remaining);
                state->recv_len = remaining;
                continue;
            }
            
            memcpy(line_buf, state->recv_buf, line_len);
            line_buf[line_len] = '\0';
            
            // 移除已处理的数据（包括 '\n'）
            size_t remaining = state->recv_len - line_len - 1;
            memmove(state->recv_buf, newline_pos + 1, remaining);
            state->recv_len = remaining;
            
            // 解析 JSON
            if (line_len > 0) {
                json_to_message(line_buf, msg);
                
                // 检查解析是否成功
                // 对于 DESD_TO_HOOK 消息，message_type 应该是 DESD_TO_HOOK
                if (msg->message_type != DESD_TO_HOOK) {
                    fprintf(stderr, "[LIBDESHOOK WARNING] read_one_desd_response: failed to parse JSON line or unexpected message_type\n");
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
        if (state->recv_len >= DESD_RECV_BUF_SIZE - 1) {
            // 缓冲区满了但没有 '\n'，协议错误
            fprintf(stderr, "[LIBDESHOOK ERROR] read_one_desd_response: buffer full but no newline (protocol error)\n");
            // 清空缓冲区，放弃当前数据
            state->recv_len = 0;
            return -1;
        }
        
        // 从 socket 读取数据（阻塞读取）
        ssize_t bytes_received = real_recv(state->desd_socket_fd, 
                                            state->recv_buf + state->recv_len,
                                            DESD_RECV_BUF_SIZE - 1 - state->recv_len,
                                            0);  // 阻塞模式
        
        if (bytes_received > 0) {
            state->recv_len += bytes_received;
            // 继续循环，查找 '\n'
            continue;
        } else if (bytes_received == 0) {
            // 连接关闭
            fprintf(stderr,
                    "[LIBDESHOOK ERROR] read_one_desd_response: DESD disconnected (R%d T%d, fd=%d)\n",
                    my_router_id,
                    state->thread_id,
                    state->desd_socket_fd);
            return -1;
        } else {
            // recv 返回 -1
            if (errno == EINTR) {
                // 被信号中断，重试
                continue;
            } else {
                // 真正的错误
                fprintf(stderr,
                        "[LIBDESHOOK ERROR] read_one_desd_response: recv failed (errno=%d) for R%d T%d, fd=%d\n",
                        errno,
                        my_router_id,
                        state->thread_id,
                        state->desd_socket_fd);
                return -1;
            }
        }
    }
}

// Helper function to receive response from desd
// Returns 1 on success, 0 on failure
static int recv_msg_from_desd(Message* resp_msg_out) {
    ThreadState *state = get_thread_state();
    if (!state || state->desd_socket_fd < 0) {
        return 0;
    }
    
    // 使用按行拆包的 helper 读取响应
    int result = read_one_desd_response(state, resp_msg_out);
    return (result == 1) ? 1 : 0;
}

// Helper function to receive response from desd, discarding stale responses
// This is needed because fire-and-forget CANCEL requests may leave orphaned responses
// Returns 1 on success, 0 on failure
static int recv_msg_from_desd_matching(const char* expected_request_id, Message* resp_msg_out) {
    ThreadState *state = get_thread_state();
    if (!state || state->desd_socket_fd < 0) {
        return 0;
    }
    
    int max_attempts = 10;  // Prevent infinite loop
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        // 使用按行拆包的 helper 读取响应
        int result = read_one_desd_response(state, resp_msg_out);
        
        if (result != 1) {
            // 读取失败或连接断开
            fprintf(stderr, "[LIBDESHOOK ERROR] recv_msg_from_desd_matching - read_one_desd_response failed\n");
            return 0;
        }
        
        // Check if request_id matches
        if (strcmp(resp_msg_out->request_id, expected_request_id) == 0) {
            return 1;  // Found matching response
        }
        
        // Stale response - discard and try again
        printf("[LIBDESHOOK] R%d T%d discarding stale response (expected=%s, got=%s, message_type=%d, event_type=%d)\n",
               my_router_id,
               get_current_thread_id(),
               expected_request_id,
               resp_msg_out->request_id,
               resp_msg_out->message_type,
               resp_msg_out->event_type);
        fflush(stdout);
    }
    
    fprintf(stderr, "[LIBDESHOOK ERROR] R%d too many stale responses, giving up\n", my_router_id);
    return 0;
}

// Helper function to send CANCEL_BLOCK_REQUEST to desd
// Used when local non-DES fd becomes ready before DESD responds
static int send_cancel_block_request(const char* request_id) {
    Message cancel_req;
    memset(&cancel_req, 0, sizeof(Message));
    cancel_req.message_type = HOOK_TO_DESD;
    cancel_req.router_id = my_router_id;
    cancel_req.thread_id = get_current_thread_id();
    cancel_req.event_type = CANCEL_BLOCK_REQUEST;
    cancel_req.virtual_time = current_virtual_time;
    strncpy(cancel_req.request_id, request_id, 63);
    cancel_req.request_id[63] = '\0';
    
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "request_id", json_string(request_id));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(cancel_req.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    cancel_req.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);
    
    // 调试日志：记录发送的 CANCEL_BLOCK_REQUEST
    printf("[LIBDESHOOK-CANCEL] R%d T%d sending CANCEL_BLOCK_REQUEST for blocked_req=%s at VT=%.6f\n",
           my_router_id,
           cancel_req.thread_id,
           request_id,
           current_virtual_time);
    fflush(stdout);

    return send_msg_to_desd(&cancel_req);
}

// Helper function to send message to desd and wait for response
// 现在每个线程有自己的通道，不再需要全局 mutex
int send_msg_to_desd_and_wait_for_response(const Message* req_msg, Message* resp_msg_out) {
    // 获取当前线程的 DESD socket
    int thread_socket = get_thread_desd_socket();
    if (thread_socket < 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Thread not registered with DESD\n");
        return 0;
    }

    // 详细调试日志：记录即将发送的 RPC 请求
    fprintf(stderr,
            "[LIBDESHOOK-RPC] R%d T%d send_msg_to_desd_and_wait_for_response: preparing to send to DESD (fd=%d, event_type=%d, req_id=%s)\n",
            my_router_id,
            get_current_thread_id(),
            thread_socket,
            req_msg->event_type,
            req_msg->request_id);
    fflush(stderr);

    // Send the request message
    char *json_str = message_to_json(req_msg);
    if (!json_str) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to serialize request message.\n");
        fflush(stderr);
        return 0;
    }
    
    // Allocate space for newline + null terminator
    size_t json_len = strlen(json_str);
    char *json_with_newline = (char*)malloc(json_len + 2); // +1 for '\n', +1 for '\0'
    if (!json_with_newline) {
        fprintf(stderr, "[LIBDESHOOK ERROR] Failed to allocate memory for message.\n");
        fflush(stderr);
        free(json_str);
        return 0;
    }
    strcpy(json_with_newline, json_str);
    json_with_newline[json_len] = '\n';
    json_with_newline[json_len + 1] = '\0';
    free(json_str);

    size_t send_len = strlen(json_with_newline);
    fprintf(stderr,
            "[LIBDESHOOK-RPC] R%d T%d sending %zu bytes to DESD (fd=%d, req_id=%s) ...\n",
            my_router_id,
            get_current_thread_id(),
            send_len,
            thread_socket,
            req_msg->request_id);
    fflush(stderr);

    ssize_t sent = real_send(thread_socket, json_with_newline, send_len, 0);
    if (sent < 0) {
        fprintf(stderr, "[LIBDESHOOK ERROR] send_msg_to_desd_and_wait_for_response - send failed (errno=%d): ", errno);
        perror("");
        fflush(stderr);
        free(json_with_newline);
        return 0;
    }
    if ((size_t)sent != send_len) {
        fprintf(stderr,
                "[LIBDESHOOK-RPC WARNING] R%d T%d partial send to DESD: sent=%zd, expected=%zu (fd=%d, req_id=%s)\n",
                my_router_id,
                get_current_thread_id(),
                sent,
                send_len,
                thread_socket,
                req_msg->request_id);
        fflush(stderr);
    } else {
        fprintf(stderr,
                "[LIBDESHOOK-RPC] R%d T%d send to DESD completed: sent=%zd bytes (fd=%d, req_id=%s)\n",
                my_router_id,
                get_current_thread_id(),
                sent,
                thread_socket,
                req_msg->request_id);
        fflush(stderr);
    }
    free(json_with_newline);

    // Now, synchronously wait for the response from desd
    // 使用 recv_msg_from_desd_matching 按 request_id 精确匹配响应，
    // 丢弃可能存在的陈旧响应（例如之前 fire-and-forget 的 CANCEL_BLOCK_REQUEST 对应的 CANCELLED）。
    fprintf(stderr,
            "[HOOK-CONTEXT] R%d T%d RPC_WAIT_START event_type=%d, req_id=%s\n",
            my_router_id,
            get_current_thread_id(),
            req_msg->event_type,
            req_msg->request_id);
    fprintf(stderr,
            "[LIBDESHOOK-RPC] R%d T%d waiting for response from DESD for req_id=%s (fd=%d) ...\n",
            my_router_id,
            get_current_thread_id(),
            req_msg->request_id,
            thread_socket);
    fflush(stderr);
    if (!recv_msg_from_desd_matching(req_msg->request_id, resp_msg_out)) {
        fprintf(stderr,
                "[HOOK-CONTEXT] R%d T%d RPC_WAIT_FAILED event_type=%d, req_id=%s\n",
                my_router_id,
                get_current_thread_id(),
                req_msg->event_type,
                req_msg->request_id);
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d failed to receive matching response for request_id=%s.\n",
                my_router_id, req_msg->request_id);
        fflush(stderr);
        return 0;
    }

    fprintf(stderr,
            "[HOOK-CONTEXT] R%d T%d RPC_WAIT_DONE event_type=%d, req_id=%s\n",
            my_router_id,
            get_current_thread_id(),
            req_msg->event_type,
            req_msg->request_id);
    fflush(stderr);

    // 调试日志：记录成功匹配到的响应
    printf("[LIBDESHOOK-RESP] R%d T%d received matching response from DESD (req_id=%s, resp_event_type=%d)\n",
           my_router_id,
           get_current_thread_id(),
           resp_msg_out->request_id,
           resp_msg_out->event_type);
    fflush(stdout);

    return 1; // Success
}

// --- Intercepted Functions ---

// connect
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    printf("[LIBDESHOOK] R%d connect() called: sockfd=%d family=%d\n", 
           my_router_id, sockfd, addr->sa_family);
    fflush(stdout);
    
    // 确保当前线程已注册到 DESD
    if (get_thread_desd_socket() < 0) {
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

    // 🔥 关键修复：检查这个 fd 是否已经在连接过程中（非阻塞 connect 的重试调用）
    // 典型模式：第一次 connect() 返回 EINPROGRESS，后续调用检查连接状态
    // 对于这种情况，不应该发送新的 CONNECT_REQUEST_EVENT，直接调用 real_connect()
    if (sockfd >= 0 && sockfd < MAX_TRACKED_FDS && connecting_fds[sockfd]) {
        printf("[LIBDESHOOK] R%d connect() on fd %d: already in connecting state, skipping CONNECT_REQUEST (non-blocking retry).\n",
               my_router_id, sockfd);
        fflush(stdout);
        
        int result = real_connect(sockfd, addr, addrlen);
        int saved_errno = errno;
        printf("[LIBDESHOOK] R%d connect() retry real_connect returned: %d (errno=%d %s)\n",
               my_router_id, result, saved_errno,
               saved_errno == EINPROGRESS ? "EINPROGRESS" :
               saved_errno == EISCONN ? "EISCONN" :
               saved_errno == EALREADY ? "EALREADY" :
               saved_errno == 0 ? "SUCCESS" : strerror(saved_errno));
        fflush(stdout);
        
        // 如果连接成功或返回 EISCONN，清除 connecting 状态
        if (result == 0 || saved_errno == EISCONN) {
            connecting_fds[sockfd] = 0;
            printf("[LIBDESHOOK] R%d fd %d connection completed, cleared connecting state.\n", my_router_id, sockfd);
        }
        
        errno = saved_errno;
        return result;
    }

    // 1. 告知desd：发送CONNECT_REQUEST_EVENT事件
    Message connect_req;
    memset(&connect_req, 0, sizeof(Message));
    connect_req.message_type = HOOK_TO_DESD;
    connect_req.router_id = my_router_id;
    connect_req.thread_id = get_current_thread_id();
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
            
            // 🔥 关键：如果返回 EINPROGRESS，标记 fd 为"连接中"状态
            // 后续对同一 fd 的 connect() 调用将跳过 CONNECT_REQUEST_EVENT
            if (sockfd >= 0 && sockfd < MAX_TRACKED_FDS) {
                if (saved_errno == EINPROGRESS) {
                    connecting_fds[sockfd] = 1;
                    printf("[LIBDESHOOK] R%d fd %d marked as connecting (EINPROGRESS).\n", my_router_id, sockfd);
                } else if (result == 0) {
                    // 连接立即成功，不需要标记
                    connecting_fds[sockfd] = 0;
                }
            }
            
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
    if (get_thread_desd_socket() < 0) {
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
    block_req.thread_id = get_current_thread_id();
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
        unsigned long connection_id = 0;
        int client_router_id = 0;
        
        if (resp_payload_obj) {
            status_json = json_object_get(resp_payload_obj, "status");
            // 提取connection_id和client信息
            json_t *conn_id_json = json_object_get(resp_payload_obj, "connection_id");
            if (conn_id_json && json_is_integer(conn_id_json)) {
                connection_id = (unsigned long)json_integer_value(conn_id_json);
            }
            json_t *client_rid_json = json_object_get(resp_payload_obj, "client_router_id");
            if (client_rid_json && json_is_integer(client_rid_json)) {
                client_router_id = json_integer_value(client_rid_json);
            }
            // client_socket_fd 不需要，服务端通过 connection_id 匹配连接
        }

        if (status_json && json_is_string(status_json) &&
            strcmp(json_string_value(status_json), "SUCCESS") == 0) {
            
            printf("[LIBDESHOOK] R%d accept() unblocked by DESD (conn_id=%lu from R%d). Now performing real accept.\n", 
                   my_router_id, connection_id, client_router_id);
            fflush(stdout);
            if (resp_payload_obj) json_decref(resp_payload_obj);
            
            // 2. DESD解除阻塞后，调用真实的accept()
            // 使用重试机制处理 EAGAIN：虚拟时间认为连接已到达，但真实内核可能还没把连接放入 backlog
            printf("[LIBDESHOOK-DEBUG] R%d calling real_accept() on sockfd=%d...\n", my_router_id, sockfd);
            fflush(stdout);
            
            int new_fd = -1;
            int saved_errno = 0;
            int accept_max_retries = 10;      // 最多重试 10 次
            int accept_wait_ms = 10;          // 每次等待 10ms，总共最多 100ms
            
            for (int retry = 0; retry < accept_max_retries; retry++) {
                new_fd = real_accept(sockfd, addr, addrlen);
                saved_errno = errno;
                
                if (new_fd >= 0) {
                    // 成功拿到新连接
                    printf("[LIBDESHOOK-DEBUG] R%d real_accept() succeeded on retry %d: new_fd=%d\n",
                           my_router_id, retry, new_fd);
                    fflush(stdout);
                    break;
                }
                
                // new_fd < 0，检查 errno
                if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                    // 软错误：暂时没有连接，等一等再试
                    if (retry == 0) {
                        printf("[LIBDESHOOK-DEBUG] R%d real_accept() returned EAGAIN, entering retry loop (max=%d, wait=%dms each)\n",
                               my_router_id, accept_max_retries, accept_wait_ms);
                        fflush(stdout);
                    }
                    
                    // 用 poll 在监听 socket 上等待 POLLIN
                    struct pollfd pfd;
                    pfd.fd = sockfd;
                    pfd.events = POLLIN;
                    pfd.revents = 0;
                    
                    int poll_ret = real_poll(&pfd, 1, accept_wait_ms);
                    
                    if (poll_ret < 0 && errno != EINTR) {
                        // poll 出错（非信号中断），视为硬错误
                        saved_errno = errno;
                        fprintf(stderr, "[LIBDESHOOK ERROR] R%d poll() failed during accept retry: errno=%d (%s)\n",
                                my_router_id, saved_errno, strerror(saved_errno));
                        fflush(stderr);
                        break;
                    }
                    
                    // poll_ret == 0: 超时，继续重试
                    // poll_ret > 0: 有数据，继续重试 accept
                    continue;
                } else {
                    // 硬错误（ECONNABORTED, EBADF, EMFILE 等），不再重试
                    fprintf(stderr, "[LIBDESHOOK ERROR] R%d real_accept() hard error on retry %d: errno=%d (%s)\n",
                            my_router_id, retry, saved_errno, strerror(saved_errno));
                    fflush(stderr);
                    break;
                }
            }
            
            printf("[LIBDESHOOK-DEBUG] R%d real_accept() final result: new_fd=%d, errno=%d (%s)\n", 
                   my_router_id, new_fd, saved_errno, new_fd < 0 ? strerror(saved_errno) : "success");
            fflush(stdout);
            
            if (new_fd < 0) {
                // 重试后仍然失败，通知 DESD 清理这条虚拟连接
                fprintf(stderr, "[LIBDESHOOK ERROR] R%d real_accept() FAILED after retries on sockfd=%d: errno=%d (%s)\n",
                        my_router_id, sockfd, saved_errno, strerror(saved_errno));
                fflush(stderr);
                
                // 通知 DESD 这次 accept 失败，清理对应的虚拟连接
                Message accept_fail;
                memset(&accept_fail, 0, sizeof(Message));
                accept_fail.message_type = HOOK_TO_DESD;
                accept_fail.router_id = my_router_id;
                accept_fail.thread_id = get_current_thread_id();
                accept_fail.event_type = CONNECTION_INFO_EVENT;  // 复用 CONNECTION_INFO_EVENT
                accept_fail.virtual_time = current_virtual_time;
                generate_request_id(accept_fail.request_id);
                
                json_t *fail_payload_obj = json_object();
                json_object_set_new(fail_payload_obj, "socket_fd", json_integer(-1));  // -1 表示失败
                json_object_set_new(fail_payload_obj, "connection_id", json_integer(connection_id));
                json_object_set_new(fail_payload_obj, "accept_failed", json_boolean(1));  // 标记为失败
                json_object_set_new(fail_payload_obj, "request_id", json_string(accept_fail.request_id));
                char *fail_payload_str = json_dumps(fail_payload_obj, JSON_COMPACT);
                strncpy(accept_fail.payload.json_str, fail_payload_str, MAX_MSG_SIZE - 1);
                accept_fail.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
                free(fail_payload_str);
                json_decref(fail_payload_obj);
                
                printf("[LIBDESHOOK] R%d notifying DESD of accept failure for conn_id=%lu\n",
                       my_router_id, connection_id);
                fflush(stdout);
                
                Message fail_resp;
                send_msg_to_desd_and_wait_for_response(&accept_fail, &fail_resp);

                // 恢复最后一次的 errno，确保调用方（BIRD）看到正确的错误码
                errno = saved_errno;
                return new_fd;  // accept 失败，返回 -1
            }
            
            // 3. accept 成功，发送 CONNECTION_INFO_EVENT 给 desd，通知新连接的 fd
            printf("[LIBDESHOOK-DEBUG] R%d accept() SUCCESS, new_fd=%d, preparing to send CONNECTION_INFO_EVENT...\n",
                   my_router_id, new_fd);
            fflush(stdout);
            
            Message conn_info;
            memset(&conn_info, 0, sizeof(Message));
            conn_info.message_type = HOOK_TO_DESD;
            conn_info.router_id = my_router_id;
            conn_info.thread_id = get_current_thread_id();
            conn_info.event_type = CONNECTION_INFO_EVENT;
            conn_info.virtual_time = current_virtual_time;
            generate_request_id(conn_info.request_id);
            
            json_t *conn_payload_obj = json_object();
            json_object_set_new(conn_payload_obj, "socket_fd", json_integer(new_fd));
            json_object_set_new(conn_payload_obj, "listen_fd", json_integer(sockfd));
            json_object_set_new(conn_payload_obj, "connection_id", json_integer(connection_id));
            json_object_set_new(conn_payload_obj, "request_id", json_string(conn_info.request_id));
            char *conn_payload_str = json_dumps(conn_payload_obj, JSON_COMPACT);
            strncpy(conn_info.payload.json_str, conn_payload_str, MAX_MSG_SIZE - 1);
            conn_info.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
            free(conn_payload_str);
            json_decref(conn_payload_obj);
            
            printf("[LIBDESHOOK-DEBUG] R%d sending CONNECTION_INFO_EVENT to DESD (req_id=%s, new_fd=%d, conn_id=%lu)...\n",
                   my_router_id, conn_info.request_id, new_fd, connection_id);
            fflush(stdout);
            
            printf("[LIBDESHOOK-DEBUG] R%d sending CONNECTION_INFO_EVENT (req_id=%s) for conn_id=%lu\n", 
                   my_router_id, conn_info.request_id, connection_id);
            fflush(stdout);
            
            Message conn_resp;
            if (send_msg_to_desd_and_wait_for_response(&conn_info, &conn_resp)) {
                printf("[LIBDESHOOK] R%d notified DESD of new connection fd %d. Response received.\n", my_router_id, new_fd);
                fflush(stdout);
            } else {
                fprintf(stderr, "[LIBDESHOOK WARNING] R%d failed to notify DESD of new connection.\n", my_router_id);
                printf("[LIBDESHOOK-DEBUG] R%d send_msg_to_desd_and_wait_for_response failed for CONNECTION_INFO\n", my_router_id);
                fflush(stdout);
            }
            
            // 标记新连接的socket为DES管理
            if (new_fd >= 0 && new_fd < MAX_TRACKED_FDS) {
                socket_fds[new_fd] = 1;
                printf("[LIBDESHOOK] R%d marked accepted fd %d as DES-managed socket.\n", my_router_id, new_fd);
            }

            // 对于成功的accept，也恢复当时的errno（通常为0），避免被内部调用污染
            errno = saved_errno;
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
    // 不要拦截对 DESD 控制 socket 的 send 调用
    int thread_socket = get_thread_desd_socket();
    if (thread_socket < 0 || sockfd == thread_socket) {
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
    send_req.thread_id = get_current_thread_id();
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
    // 不要拦截对 DESD 控制 socket 的 recv 调用
    int thread_socket = get_thread_desd_socket();
    if (thread_socket < 0 || sockfd == thread_socket) {
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
    recv_block_req.thread_id = get_current_thread_id();
    recv_block_req.event_type = ROUTER_BLOCK_REQUEST;
    recv_block_req.virtual_time = current_virtual_time;
    generate_request_id(recv_block_req.request_id);
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "blocked_function", json_string("RECV_CALL"));
    json_object_set_new(payload_obj, "request_id", json_string(recv_block_req.request_id));
    json_object_set_new(payload_obj, "socket_fd", json_integer(sockfd)); // 修复：字段名改为socket_fd，与desd保持一致
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

// poll的内部实现 - 新架构：libdeshook同时监听desd_fd和本地非DES fd
// 核心思想：
// 1. 虚拟时间由DESD控制，timeout通过DESD的TIMEOUT_EVENT实现
// 2. libdeshook用real_poll同时监听desd_fd和本地fd，任何一个ready都可以唤醒
// 3. 如果本地fd先ready，发送CANCEL_BLOCK_REQUEST取消DESD的阻塞状态
static int poll_internal(struct pollfd *fds, nfds_t nfds, int timeout) {
    int poll_result = 0;
    int desd_fd = get_thread_desd_socket();
    
    if (desd_fd < 0) {
        return real_poll(fds, nfds, timeout);
    }

    // 🔒 线程安全：保护socket_fds数组的读取，创建一个本地副本
    int local_socket_fds[MAX_TRACKED_FDS];
    pthread_mutex_lock(&socket_fds_mutex);
    memcpy(local_socket_fds, socket_fds, sizeof(socket_fds));
    pthread_mutex_unlock(&socket_fds_mutex);

    // Step 1: 分离DES管理的socket和非DES管理的fd
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
    
    // 极轻量统计
    static unsigned long poll_call_count = 0;
    unsigned long this_poll_call = ++poll_call_count;
    if (this_poll_call == 1 || this_poll_call == 10 || this_poll_call == 100 || (this_poll_call % 1000) == 0) {
        printf("[LIBDESHOOK-POLL] R%d T%d poll_internal called %lu time(s) (nfds=%lu, timeout=%d, des_count=%lu, non_des_count=%lu)\n",
               my_router_id, get_current_thread_id(), this_poll_call,
               (unsigned long)nfds, timeout, (unsigned long)des_count, (unsigned long)non_des_count);
        fflush(stdout);
    }

    // Step 2: 先对本地fd做一次非阻塞检查
    int non_des_ready = 0;
    struct pollfd *non_des_fds = NULL;
    nfds_t *non_des_orig_idx = NULL;  // 记录non_des_fds[i]对应fds中的原始索引
    
    if (non_des_count > 0) {
        non_des_fds = (struct pollfd*)malloc(sizeof(struct pollfd) * non_des_count);
        non_des_orig_idx = (nfds_t*)malloc(sizeof(nfds_t) * non_des_count);
        if (!non_des_fds || !non_des_orig_idx) {
            if (non_des_fds) free(non_des_fds);
            if (non_des_orig_idx) free(non_des_orig_idx);
            errno = ENOMEM;
            return -1;
        }
        
        nfds_t idx = 0;
        for (nfds_t i = 0; i < nfds; i++) {
            int fd = fds[i].fd;
            if (fd < 0 || fd >= MAX_TRACKED_FDS || !local_socket_fds[fd]) {
                non_des_fds[idx] = fds[i];
                non_des_orig_idx[idx] = i;
                idx++;
            }
        }
        
        // 非阻塞检查
        non_des_ready = real_poll(non_des_fds, non_des_count, 0);
        
        if (non_des_ready > 0) {
            // 本地fd已ready，立即返回
            for (nfds_t i = 0; i < nfds; i++) {
                fds[i].revents = 0;
            }
            for (nfds_t i = 0; i < non_des_count; i++) {
                fds[non_des_orig_idx[i]].revents = non_des_fds[i].revents;
            }
            free(non_des_fds);
            free(non_des_orig_idx);
            return non_des_ready;
        }
    }

    // Step 3: 只有非阻塞探测（timeout == 0）且没有 DES 管理的 fd 时，才绕过 DES
    // 阻塞 poll（timeout > 0 或 timeout == -1）必须通过 DES 报告 block，
    // 这样才能让 interact_with_router_until_it_blocks() 正确 return
    if (des_count == 0 && timeout == 0) {
        // 非阻塞探测，没有DES管理的socket，直接调用真实的poll
        if (non_des_fds) free(non_des_fds);
        if (non_des_orig_idx) free(non_des_orig_idx);
        
        int result = real_poll(fds, nfds, timeout);
        
        // 轻量级统计日志
        static unsigned long zero_des_poll_count = 0;
        zero_des_poll_count++;
        if (zero_des_poll_count == 1 || zero_des_poll_count == 10 || zero_des_poll_count == 100 || (zero_des_poll_count % 1000) == 0) {
            printf("[LIBDESHOOK-POLL] R%d T%d zero-des-fd non-blocking poll called %lu time(s) (nfds=%lu, result=%d) - bypassing DESD\n",
                   my_router_id, get_current_thread_id(), zero_des_poll_count,
                   (unsigned long)nfds, result);
            fflush(stdout);
        }
        
        return result;
    }

    // Step 4: 需要等待 - 向DESD发送阻塞请求（只发送，不等待响应）
    Message poll_block_req;
    memset(&poll_block_req, 0, sizeof(Message));
    poll_block_req.message_type = HOOK_TO_DESD;
    poll_block_req.router_id = my_router_id;
    poll_block_req.thread_id = get_current_thread_id();
    poll_block_req.event_type = ROUTER_BLOCK_REQUEST;
    poll_block_req.virtual_time = current_virtual_time;
    generate_request_id(poll_block_req.request_id);
    
    char saved_request_id[64];
    strncpy(saved_request_id, poll_block_req.request_id, 63);
    saved_request_id[63] = '\0';

    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "blocked_function", json_string("SELECT_CALL"));
    json_object_set_new(payload_obj, "request_id", json_string(poll_block_req.request_id));
    json_object_set_new(payload_obj, "timeout_ms", json_integer(timeout));
    
    // 只传递DES管理的socket fd
    json_t *monitored_fds_array = json_array();
    for (nfds_t i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        if (fd >= 0 && fd < MAX_TRACKED_FDS && local_socket_fds[fd]) {
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

    // 只发送，不等待
    if (!send_msg_to_desd(&poll_block_req)) {
        fprintf(stderr, "[LIBDESHOOK ERROR] R%d poll() failed to send block request to DESD.\n", my_router_id);
        if (non_des_fds) free(non_des_fds);
        if (non_des_orig_idx) free(non_des_orig_idx);
        errno = EIO;
        return -1;
    }

    // Step 5: 构建kernel_fds数组 = [desd_fd] + 本地非DES fd
    // 用real_poll同时监听DESD通道和本地fd
    nfds_t kernel_nfds = 1 + non_des_count;  // desd_fd + 本地fd
    struct pollfd *kernel_fds = (struct pollfd*)malloc(sizeof(struct pollfd) * kernel_nfds);
    if (!kernel_fds) {
        if (non_des_fds) free(non_des_fds);
        if (non_des_orig_idx) free(non_des_orig_idx);
        errno = ENOMEM;
        return -1;
    }
    
    // kernel_fds[0] = desd_fd
    kernel_fds[0].fd = desd_fd;
    kernel_fds[0].events = POLLIN;
    kernel_fds[0].revents = 0;
    
    // kernel_fds[1..] = 本地非DES fd
    if (non_des_fds) {
        for (nfds_t i = 0; i < non_des_count; i++) {
            kernel_fds[1 + i] = non_des_fds[i];
            kernel_fds[1 + i].revents = 0;
        }
    }

    // Step 6: 阻塞等待 - timeout=-1表示无限等待（由DESD的TIMEOUT_EVENT控制虚拟时间）
    int kernel_ready;
    for (;;) {
        kernel_ready = real_poll(kernel_fds, kernel_nfds, -1);
        if (kernel_ready < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d poll() real_poll failed (errno=%d)\n", my_router_id, errno);
            free(kernel_fds);
            if (non_des_fds) free(non_des_fds);
            if (non_des_orig_idx) free(non_des_orig_idx);
            return -1;
        }
        break;
    }

    int desd_ready = (kernel_fds[0].revents & POLLIN);
    int local_ready_count = 0;
    for (nfds_t i = 1; i < kernel_nfds; i++) {
        if (kernel_fds[i].revents != 0) {
            local_ready_count++;
        }
    }

    // Step 7: 处理结果
    // 优先级规则：如果DESD有响应，优先处理DESD（保证虚拟时间语义）
    if (desd_ready) {
        // DESD有响应：接收并处理
        // 使用recv_msg_from_desd_matching来处理可能的stale响应（来自之前fire-and-forget的CANCEL）
        Message poll_block_resp;
        if (!recv_msg_from_desd_matching(saved_request_id, &poll_block_resp)) {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d poll() failed to recv from DESD.\n", my_router_id);
            free(kernel_fds);
            if (non_des_fds) free(non_des_fds);
            if (non_des_orig_idx) free(non_des_orig_idx);
            errno = EIO;
            return -1;
        }
        
        // 清除所有fd的revents
        for (nfds_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
        }
        
        json_error_t error;
        json_t *resp_payload_obj = json_loads(poll_block_resp.payload.json_str, 0, &error);
        const char *status = resp_payload_obj ? json_string_value(json_object_get(resp_payload_obj, "status")) : NULL;
        
        if (status && strcmp(status, "SUCCESS") == 0) {
            // DES fd就绪
            json_t *ready_fds_array = json_object_get(resp_payload_obj, "ready_fds");
            int des_ready_count = 0;
            
            if (ready_fds_array && json_is_array(ready_fds_array)) {
                size_t array_size = json_array_size(ready_fds_array);
                for (size_t j = 0; j < array_size; j++) {
                    json_t *fd_info = json_array_get(ready_fds_array, j);
                    if (json_is_object(fd_info)) {
                        json_t *fd_obj = json_object_get(fd_info, "fd");
                        json_t *revents_obj = json_object_get(fd_info, "revents");
                        if (fd_obj && revents_obj) {
                            int ready_fd = json_integer_value(fd_obj);
                            short ready_revents = (short)json_integer_value(revents_obj);
                            for (nfds_t i = 0; i < nfds; i++) {
                                if (fds[i].fd == ready_fd) {
                                    short filtered = ready_revents & (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
                                    fds[i].revents = filtered;
                                    if (filtered != 0) des_ready_count++;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            
            // 同时检查本地fd是否也ready（此时kernel_fds[1..]的revents可用）
            for (nfds_t i = 0; i < non_des_count; i++) {
                if (kernel_fds[1 + i].revents != 0) {
                    fds[non_des_orig_idx[i]].revents = kernel_fds[1 + i].revents;
                }
            }
            
            poll_result = des_ready_count + local_ready_count;
            if (resp_payload_obj) json_decref(resp_payload_obj);
            
        } else if (status && strcmp(status, "TIMEOUT") == 0) {
            // 虚拟时间超时
            // 再检查一次本地fd（此时kernel_fds[1..]的revents可用）
            for (nfds_t i = 0; i < non_des_count; i++) {
                if (kernel_fds[1 + i].revents != 0) {
                    fds[non_des_orig_idx[i]].revents = kernel_fds[1 + i].revents;
                }
            }
            poll_result = local_ready_count;
            if (resp_payload_obj) json_decref(resp_payload_obj);
            
        } else if (status && strcmp(status, "CANCELLED") == 0) {
            // 阻塞请求被取消（不应该在这个分支发生，因为是DESD先响应）
            poll_result = 0;
            if (resp_payload_obj) json_decref(resp_payload_obj);
            
        } else {
            fprintf(stderr, "[LIBDESHOOK ERROR] R%d poll() DESD returned error: %s\n", my_router_id,
                    resp_payload_obj ? json_string_value(json_object_get(resp_payload_obj, "error_message")) : "Unknown");
            if (resp_payload_obj) json_decref(resp_payload_obj);
            free(kernel_fds);
            if (non_des_fds) free(non_des_fds);
            if (non_des_orig_idx) free(non_des_orig_idx);
            errno = ECOMM;
            return -1;
        }
        
    } else if (local_ready_count > 0) {
        // 本地fd先ready，DESD还没响应
        // 发送CANCEL_BLOCK_REQUEST取消之前的阻塞请求（fire-and-forget）
        // 注意：不等待响应，因为DESD可能正阻塞在event queue上，无法立即回复
        // 稍后的recv操作会处理可能的stale响应
        send_cancel_block_request(saved_request_id);
        
        // 设置返回值
        for (nfds_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
        }
        for (nfds_t i = 0; i < non_des_count; i++) {
            if (kernel_fds[1 + i].revents != 0) {
                fds[non_des_orig_idx[i]].revents = kernel_fds[1 + i].revents;
            }
        }
        poll_result = local_ready_count;
        
    } else {
        // 不应该到达这里（kernel_ready > 0 但既没有desd_ready也没有local_ready）
        fprintf(stderr, "[LIBDESHOOK WARNING] R%d poll() unexpected state: kernel_ready=%d but no fd ready\n",
                my_router_id, kernel_ready);
        poll_result = 0;
    }

    free(kernel_fds);
    if (non_des_fds) free(non_des_fds);
    if (non_des_orig_idx) free(non_des_orig_idx);
    return poll_result;
}

// Hook所有poll变体 - BIRD可能调用glibc的内部版本
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return poll_internal(fds, nfds, timeout);
}

// Hook __poll (弱符号版本)
int __poll(struct pollfd *fds, nfds_t nfds, int timeout) __attribute__((weak, alias("poll")));

// Hook __libc_poll (libc内部版本)
int __libc_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return poll_internal(fds, nfds, timeout);
}

// Hook __GI___poll (glibc内部版本) - 这是BIRD实际调用的！
int __GI___poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return poll_internal(fds, nfds, timeout);
}

int __poll_chk(struct pollfd *fds, nfds_t nfds, int timeout, size_t fds_size) {
    return poll_internal(fds, nfds, timeout);
}

// select - 用于支持带超时的I/O操作
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    if (get_thread_desd_socket() < 0) {
        return real_select(nfds, readfds, writefds, exceptfds, timeout);
    }

    // 计算超时时间（毫秒）
    int timeout_ms = -1;  // -1 表示无限等待
    if (timeout != NULL) {
        timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    }
    
    // 极轻量统计 select 包装调用次数，用于确认是否经由 DES 路径
    static unsigned long select_call_count = 0;
    unsigned long this_select_call = ++select_call_count;
    if (this_select_call == 1 ||
        this_select_call == 10 ||
        this_select_call == 100 ||
        (this_select_call % 1000) == 0) {
        int current_tid = get_current_thread_id();
        printf("[LIBDESHOOK-SELECT] R%d T%d select() wrapper called %lu time(s) (nfds=%d, timeout_ms=%d)\n",
               my_router_id, current_tid, this_select_call, nfds, timeout_ms);
        fflush(stdout);
    }
    
    // 如果是非阻塞调用（timeout = 0），直接调用真实的select
    if (timeout != NULL && timeout->tv_sec == 0 && timeout->tv_usec == 0) {
        return real_select(nfds, readfds, writefds, exceptfds, timeout);
    }

    // 否则，向desd注册阻塞请求（SELECT_CALL）
    Message select_block_req;
    memset(&select_block_req, 0, sizeof(Message));
    select_block_req.message_type = HOOK_TO_DESD;
    select_block_req.router_id = my_router_id;
    select_block_req.thread_id = get_current_thread_id();
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
            
            if (resp_payload_obj) json_decref(resp_payload_obj);
            return ready_count;

        } else if (status_json && json_is_string(status_json) &&
                   strcmp(json_string_value(status_json), "TIMEOUT") == 0) {
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
    if (is_des_socket && get_thread_desd_socket() >= 0) {
        printf("[LIBDESHOOK] R%d notifying DESD of socket close for fd %d.\n", my_router_id, sockfd);
        
        // 构建CLOSE_SOCKET_EVENT消息
        Message close_msg;
        memset(&close_msg, 0, sizeof(Message));
        close_msg.message_type = HOOK_TO_DESD;
        close_msg.router_id = my_router_id;
        close_msg.thread_id = get_current_thread_id();
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
        if (socket_fds[sockfd] || nonblocking_fds[sockfd] || connecting_fds[sockfd]) {
            printf("[LIBDESHOOK] R%d clearing tracking for fd %d (socket:%d, nonblocking:%d, connecting:%d).\n", 
                   my_router_id, sockfd, socket_fds[sockfd], nonblocking_fds[sockfd], connecting_fds[sockfd]);
        }
        socket_fds[sockfd] = 0;
        nonblocking_fds[sockfd] = 0;
        connecting_fds[sockfd] = 0;  // 清除连接中状态
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
    
    if (get_thread_desd_socket() < 0) {
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
    listen_msg.thread_id = get_current_thread_id();
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
    if (get_thread_desd_socket() < 0) {
        return real_sleep(seconds);
    }

    printf("[LIBDESHOOK] R%d intercepted sleep(%u seconds).\n", my_router_id, seconds);

    // 向desd注册阻塞请求（SLEEP_CALL）
    Message sleep_block_req;
    memset(&sleep_block_req, 0, sizeof(Message));
    sleep_block_req.message_type = HOOK_TO_DESD;
    sleep_block_req.router_id = my_router_id;
    sleep_block_req.thread_id = get_current_thread_id();
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
    // 标准输入/输出/错误：不拦截，避免在日志/初始化阶段产生递归注册
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        return real_read(fd, buf, count);
    }

    // 1. 获取当前线程的 DESD socket
    int thread_socket = get_thread_desd_socket();
    
    // 2. desd控制socket：不拦截
    if (fd == thread_socket) {
        return real_read(fd, buf, count);
    }
    
    // 3. 未初始化DES：不拦截
    if (thread_socket < 0) {
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
    // 标准输出/错误：不拦截，避免fprintf/printf触发write钩子导致ensure_thread_registered递归调用
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        return real_write(fd, buf, count);
    }

    // 1. 获取当前线程的 DESD socket
    int thread_socket = get_thread_desd_socket();
    
    // 2. desd控制socket：不拦截
    if (fd == thread_socket) {
        return real_write(fd, buf, count);
    }
    
    // 3. 未初始化DES：不拦截
    if (thread_socket < 0) {
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
    if (get_thread_desd_socket() < 0 || !real_clock_gettime) {
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
            .thread_id = get_current_thread_id(),
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
        
        // 详细记录每次 clock_gettime 调用（包含线程ID和序号），用于定位是否卡在此处
        static unsigned long clock_call_count = 0;
        unsigned long this_call = ++clock_call_count;
        int current_tid = get_current_thread_id();
        printf("[LIBDESHOOK] R%d T%d clock_gettime(CLOCK_MONOTONIC) called (seq=%lu)\n",
               my_router_id, current_tid, this_call);
        fflush(stdout);
        
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
        
        // 打印本次返回的虚拟时间及与上次的差值，帮助观察是否停滞
        double time_delta = (last_reported_vtime >= 0) ? (vtime_sec - last_reported_vtime) : 0;
        printf("[LIBDESHOOK] R%d T%d clock_gettime() returning VT=%.3f (delta=%.3f, total_calls=%llu, seq=%lu)\n",
               my_router_id, current_tid, vtime_sec, time_delta, total_clock_calls, this_call);
        fflush(stdout);
        last_reported_vtime = vtime_sec;
        
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

// === Mutex Hook 实现 ===

// 等待 DESD 的 MUTEX_RESUME 消息
static int wait_for_desd_mutex_resume(uintptr_t mutex_addr) {
    ThreadState *state = get_thread_state();
    if (!state || state->desd_socket_fd < 0) {
        fprintf(stderr, "[LIBDESHOOK-MUTEX ERROR] R%d wait_for_desd_mutex_resume: thread not registered\n",
                my_router_id);
        return -1;
    }
    
    printf("[LIBDESHOOK-MUTEX] R%d T%d waiting for MUTEX_RESUME on mutex 0x%lx...\n",
           my_router_id, state->thread_id, (unsigned long)mutex_addr);
    fflush(stdout);
    
    // 阻塞等待 DESD 的 MUTEX_RESUME 消息
    while (1) {
        Message resp;
        int read_result = read_one_desd_response(state, &resp);
        
        if (read_result != 1) {
            fprintf(stderr, "[LIBDESHOOK-MUTEX ERROR] R%d T%d failed to receive MUTEX_RESUME\n",
                    my_router_id, state->thread_id);
            return -1;
        }
        
        // 检查是否是 MUTEX_RETRY_EVENT 响应
        if (resp.event_type == MUTEX_RETRY_EVENT) {
            // 解析响应确认是我们等待的 mutex
            json_error_t error;
            json_t *payload_obj = json_loads(resp.payload.json_str, 0, &error);
            if (payload_obj) {
                const char *status = json_string_value(json_object_get(payload_obj, "status"));
                if (status && strcmp(status, "MUTEX_RESUME") == 0) {
                    printf("[LIBDESHOOK-MUTEX] R%d T%d received MUTEX_RESUME for mutex 0x%lx\n",
                           my_router_id, state->thread_id, (unsigned long)mutex_addr);
                    fflush(stdout);
                    json_decref(payload_obj);
                    return 0;  // 成功收到 MUTEX_RESUME
                }
                json_decref(payload_obj);
            }
        }
        
        // 收到其他消息，可能是陈旧的响应，丢弃并继续等待
        printf("[LIBDESHOOK-MUTEX] R%d T%d discarding unexpected message while waiting for MUTEX_RESUME (event_type=%d)\n",
               my_router_id, state->thread_id, resp.event_type);
        fflush(stdout);
    }
}

// 发送 mutex 控制消息给 DESD（单向，不等待响应）
static int send_mutex_msg_to_desd(EventType event_type, uintptr_t mutex_addr) {
    ThreadState *state = get_thread_state();
    if (!state || !state->is_registered) {
        return -1;  // 线程未注册，跳过 hook
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.message_type = HOOK_TO_DESD;
    msg.router_id = my_router_id;
    msg.thread_id = state->thread_id;
    msg.event_type = event_type;
    msg.virtual_time = current_virtual_time;
    generate_request_id(msg.request_id);
    
    // 构造 payload
    json_t *payload_obj = json_object();
    json_object_set_new(payload_obj, "mutex_addr", json_integer((json_int_t)mutex_addr));
    json_object_set_new(payload_obj, "request_id", json_string(msg.request_id));
    char *payload_str = json_dumps(payload_obj, JSON_COMPACT);
    strncpy(msg.payload.json_str, payload_str, MAX_MSG_SIZE - 1);
    msg.payload.json_str[MAX_MSG_SIZE - 1] = '\0';
    free(payload_str);
    json_decref(payload_obj);
    
    // 发送消息（不等待响应）
    char *json_str = message_to_json(&msg);
    if (!json_str) {
        return -1;
    }
    
    size_t json_len = strlen(json_str);
    char *json_with_newline = (char*)malloc(json_len + 2);
    if (!json_with_newline) {
        free(json_str);
        return -1;
    }
    strcpy(json_with_newline, json_str);
    json_with_newline[json_len] = '\n';
    json_with_newline[json_len + 1] = '\0';
    free(json_str);
    
    ssize_t sent = real_send(state->desd_socket_fd, json_with_newline, strlen(json_with_newline), 0);
    free(json_with_newline);
    
    return (sent > 0) ? 0 : -1;
}

// pthread_mutex_lock hook
int pthread_mutex_lock(pthread_mutex_t *mutex) {
    // 防止递归 hook（例如在 hook 内部调用了需要加锁的函数）
    if (in_mutex_hook) {
        return real_pthread_mutex_lock(mutex);
    }

    // 检查是否是我们内部使用的 mutex（request_counter_mutex, socket_fds_mutex 等）
    // 这些 mutex 不应该被 hook
    if (mutex == &request_counter_mutex || mutex == &socket_fds_mutex || mutex == &thread_id_mutex) {
        return real_pthread_mutex_lock(mutex);
    }

    // 检查线程是否已注册到 DESD
    ThreadState *state = get_thread_state();
    if (!state || !state->is_registered) {
        // 线程未注册，直接使用原始函数
        return real_pthread_mutex_lock(mutex);
    }

    in_mutex_hook = 1;

    uintptr_t mutex_addr = (uintptr_t)mutex;

    // 1. 先尝试非阻塞加锁（无竞争的快速路径）
    int result = real_pthread_mutex_trylock(mutex);

    if (result == 0) {
        // 加锁成功：可选调试日志
        // send_mutex_msg_to_desd(MUTEX_LOCK_ACQUIRED, mutex_addr);
        printf("[LIBDESHOOK-MUTEX] R%d T%d acquired mutex 0x%lx (no contention)\n",
               my_router_id, state->thread_id, (unsigned long)mutex_addr);
        fflush(stdout);
        in_mutex_hook = 0;
        return 0;
    }

    if (result != EBUSY) {
        // 其他错误，直接返回
        in_mutex_hook = 0;
        return result;
    }

    // 2. EBUSY：锁被占用，需要等待
    // 关键点：**绝不**再退回到内核级阻塞锁，否则 DESD 会失去对该线程的控制，导致死锁
    while (1) {
        printf("[LIBDESHOOK-MUTEX] R%d T%d mutex 0x%lx is busy, notifying DESD...\n",
               my_router_id, state->thread_id, (unsigned long)mutex_addr);
        fflush(stdout);

        // 2.1 通知 DESD：开始等待此 mutex
        if (send_mutex_msg_to_desd(MUTEX_WAIT_START, mutex_addr) < 0) {
            // 发送失败，只能降级到真实的 pthread_mutex_lock
            fprintf(stderr, "[LIBDESHOOK-MUTEX ERROR] R%d T%d failed to send MUTEX_WAIT_START, falling back to real lock\n",
                    my_router_id, state->thread_id);
            fflush(stderr);
            in_mutex_hook = 0;
            return real_pthread_mutex_lock(mutex);
        }

        // 2.2 阻塞等待 DESD 通过 MUTEX_RESUME 唤醒
        if (wait_for_desd_mutex_resume(mutex_addr) < 0) {
            // 等待失败，也只能降级
            fprintf(stderr, "[LIBDESHOOK-MUTEX ERROR] R%d T%d wait_for_desd_mutex_resume failed, falling back to real lock\n",
                    my_router_id, state->thread_id);
            fflush(stderr);
            in_mutex_hook = 0;
            return real_pthread_mutex_lock(mutex);
        }

        // 2.3 被 DESD 唤醒后，尝试一次非阻塞加锁
        result = real_pthread_mutex_trylock(mutex);

        if (result == 0) {
            printf("[LIBDESHOOK-MUTEX] R%d T%d acquired mutex 0x%lx after DESD resume\n",
                   my_router_id, state->thread_id, (unsigned long)mutex_addr);
            fflush(stdout);
            in_mutex_hook = 0;
            return 0;
        }

        if (result != EBUSY) {
            // 其它错误，直接返回
            in_mutex_hook = 0;
            return result;
        }

        // 2.4 仍然是 EBUSY：说明在我们被唤醒到真正执行 trylock 期间，
        // 锁又被别的线程抢走了。为了保持模拟的确定性，不在这里自旋或退回内核锁，
        // 而是重新通知 DESD 再次进入“等待-唤醒”循环。
        printf("[LIBDESHOOK-MUTEX] R%d T%d mutex 0x%lx still busy after MUTEX_RESUME, re-entering wait loop...\n",
               my_router_id, state->thread_id, (unsigned long)mutex_addr);
        fflush(stdout);

        // 回到 while(1) 顶部，再次发送 MUTEX_WAIT_START 并等待下一次 MUTEX_RESUME
    }
}

// pthread_mutex_unlock hook
int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    // 防止递归 hook
    if (in_mutex_hook) {
        return real_pthread_mutex_unlock(mutex);
    }
    
    // 检查是否是内部 mutex
    if (mutex == &request_counter_mutex || mutex == &socket_fds_mutex || mutex == &thread_id_mutex) {
        return real_pthread_mutex_unlock(mutex);
    }
    
    // 检查线程是否已注册到 DESD
    ThreadState *state = get_thread_state();
    if (!state || !state->is_registered) {
        return real_pthread_mutex_unlock(mutex);
    }
    
    in_mutex_hook = 1;
    
    uintptr_t mutex_addr = (uintptr_t)mutex;
    
    // 1. 先执行真正的解锁
    int result = real_pthread_mutex_unlock(mutex);
    
    if (result == 0) {
        // 2. 解锁成功，通知 DESD
        printf("[LIBDESHOOK-MUTEX] R%d T%d released mutex 0x%lx, notifying DESD...\n",
               my_router_id, state->thread_id, (unsigned long)mutex_addr);
        fflush(stdout);
        
        send_mutex_msg_to_desd(MUTEX_UNLOCK, mutex_addr);
    }
    
    in_mutex_hook = 0;
    return result;
}
