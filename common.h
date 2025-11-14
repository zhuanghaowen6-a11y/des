#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <pthread.h> // 仅用于互斥锁和条件变量（如果需要），不是为了多线程执行

// --- Constants ---
#define DESD_CONTROL_SOCKET_PATH "/tmp/desd_control_socket"
#define ROUTER_SOCKET_PATH "/tmp/router_socket" // R1和R2之间真实通信的socket路径
#define MAX_MSG_SIZE 4096 // desd与libdeshook之间控制消息的最大大小

// --- Enums ---

typedef enum {
    ROUTER_START,             // desd内部事件：路由器启动
    ROUTER_BLOCK_REQUEST,     // libdeshook.so -> desd：路由器请求阻塞（recv, accept, select）
    PACKET_SEND_EVENT,        // libdeshook.so -> desd：路由器调用send()，通知desd数据已发送
    PACKET_RECEIVE_EVENT,     // desd内部事件：数据包到达目的路由器socket缓冲区
    TIMEOUT_EVENT,            // desd内部事件：模拟select或connect的超时
    CONNECT_REQUEST_EVENT,    // libdeshook.so -> desd：路由器请求建立连接（connect）
    CONNECTION_ESTABLISHED_EVENT, // desd内部事件：连接在虚拟时间上建立
    LISTEN_EVENT              // libdeshook.so -> desd：路由器开始监听（listen）

} EventType;

typedef enum {
    HOOK_TO_DESD, // libdeshook.so 发送给 desd
    DESD_TO_HOOK  // desd 发送给 libdeshook.so
} MessageType;

typedef enum {
    RECV_CALL,
    ACCEPT_CALL,
    SELECT_CALL,
    CONNECT_CALL
} BlockedFunction;

typedef enum {
    RUNNING,   // 路由器正在执行
    BLOCKED,   // 路由器已阻塞，等待desd解除阻塞
    IDLE       // 路由器无事可做，等待新事件
} RouterStatus;

// --- Data Structures ---

// Payload for messages and events (JSON strings for flexibility)
typedef struct {
    char json_str[MAX_MSG_SIZE];
} Payload;

// DES Event structure
typedef struct {
    double timestamp;       // 事件的虚拟时间戳
    int router_id;          // 目标路由器ID
    EventType event_type;   // 事件类型
    unsigned long event_id; // 唯一事件ID
    Payload payload;        // 事件特定数据 (JSON)
} Event;

// Message structure for communication between desd and libdeshook.so
typedef struct {
    MessageType message_type;       // 消息方向
    int router_id;                  // 发送者或目标路由器ID
    char request_id[64];            // 唯一请求ID，用于匹配请求/响应
    EventType event_type;           // 事件类型 (仅用于 HOOK_TO_DESD 消息)
    double virtual_time;            // 发送者的当前虚拟时间
    Payload payload;                // 消息特定数据 (JSON)
} Message;

// Global event ID generator
unsigned long generate_event_id();

// Function prototypes for JSON (using Jansson library)
char* event_to_json(const Event* event);
void json_to_event(const char* json_str, Event* event);
char* message_to_json(const Message* msg);
void json_to_message(const char* json_str, Message* msg);

// Helper for Base64 encoding/decoding (用于封装原始数据包，尽管现在数据不经过desd)
char* base64_encode(const unsigned char* data, size_t input_length);
unsigned char* base64_decode(const char* data, size_t *output_length);

extern double current_virtual_time; // 定义在 desd.c 中，libdeshook.c 使用

#endif // COMMON_H
