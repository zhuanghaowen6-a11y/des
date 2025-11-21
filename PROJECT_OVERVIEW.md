# DES (Distributed Event Simulator) 项目完整文档

## 目录
1. [项目概述](#项目概述)
2. [架构设计](#架构设计)
3. [核心组件](#核心组件)
4. [关键数据结构](#关键数据结构)
5. [事件流程](#事件流程)
6. [连接管理机制](#连接管理机制)
7. [虚拟时间管理](#虚拟时间管理)
8. [重要设计决策](#重要设计决策)
9. [已实现功能](#已实现功能)
10. [代码结构](#代码结构)
11. [常见问题和解决方案](#常见问题和解决方案)
12. [测试说明](#测试说明)

---

## 项目概述

### 目标
实现一个**分布式事件模拟器（DES）**，用于模拟网络路由器之间的通信，支持虚拟时间推进和事件驱动的网络模拟。

### 核心特性
- **虚拟时间**：所有路由器在统一的虚拟时间中运行
- **事件驱动**：通过事件队列管理所有操作
- **透明拦截**：使用 LD_PRELOAD 拦截 socket API 调用
- **精确模拟**：模拟网络延迟、连接建立时间等
- **多路复用支持**：支持 `select()` 和 `poll()` 系统调用
- **TCP/UDS 支持**：同时支持 Unix Domain Socket 和 TCP 通信

### 基本原理
```
用户程序 (r1, r2)
    ↓ (socket API 调用)
libdeshook.so (LD_PRELOAD 拦截)
    ↓ (JSON 消息)
desd (中央调度器)
    ↓ (事件队列 + 虚拟时间)
模拟网络行为
```

---

## 架构设计

### 三层架构

```
┌─────────────────────────────────────────┐
│  应用层：路由器程序 (r1, r2, ...)      │
│  - 使用标准 socket API                  │
│  - 不需要修改代码                       │
└─────────────────────────────────────────┘
           ↓ socket(), connect(), send()...
┌─────────────────────────────────────────┐
│  拦截层：libdeshook.so                  │
│  - LD_PRELOAD 拦截 socket API           │
│  - 转换为 JSON 事件消息                 │
│  - 与 desd 通信                         │
└─────────────────────────────────────────┘
           ↓ Unix Domain Socket
┌─────────────────────────────────────────┐
│  调度层：desd                           │
│  - 事件队列（最小堆）                   │
│  - 虚拟时间管理                         │
│  - 路由器状态管理                       │
│  - 数据包缓冲                           │
└─────────────────────────────────────────┘
```

### 通信流程

```
路由器 r1                    desd                    路由器 r2
   |                          |                          |
   |--- socket() ------------>|                          |
   |<-- 返回 fd=4 ------------|                          |
   |                          |                          |
   |--- connect(fd=4) ------->|                          |
   |    (阻塞在 desd)         |                          |
   |                          |--- listen 注册 --------->|
   |                          |                          |
   |                          |--- accept() 阻塞 ------->|
   |                          |                          |
   |                   [VT=0.049]                        |
   |<-- SUCCESS ---------------|                          |
   |    (connect 完成)         |                          |
   |                          |                          |
   |                   [VT=0.050]                        |
   |                          |<-- accept 返回 fd=5 -----|
   |                          |                          |
   |--- send(fd=4) ---------->|                          |
   |    (延迟到 VT=0.051)     |                          |
   |                          |                          |
   |                   [VT=0.051]                        |
   |                          |--- 缓冲数据 ------------>|
   |                          |    (socket_fd=5)         |
   |                          |                          |
   |                   [VT=0.151]                        |
   |                          |--- PACKET_RECEIVE ------>|
   |                          |    (唤醒 recv)           |
   |                          |<-- recv() 读取 ---------|
```

---

## 核心组件

### 1. desd.c (中央调度器)

**职责**：
- 管理所有路由器的生命周期和状态
- 维护事件队列，按虚拟时间顺序处理事件
- 模拟网络延迟和连接建立时间
- 缓冲数据包
- 管理路由器之间的连接映射

**关键函数**：
```c
void desd_event_loop()                      // 主事件循环
void handle_connect_request_event()         // 处理连接请求
void handle_connection_established_event()  // 处理连接建立
void handle_packet_send_event()             // 处理数据包发送
void handle_packet_receive_event()          // 处理数据包接收
void handle_router_block_request()          // 处理路由器阻塞请求
```

### 2. libdeshook.c (API 拦截层)

**职责**：
- 使用 `dlsym(RTLD_NEXT, ...)` 获取真实的 socket API
- 拦截 socket API 调用并转换为事件
- 与 desd 通信（通过 Unix Domain Socket）
- 处理真实的系统调用（在 desd 允许后）

**拦截的 API**：
```c
socket()    - 创建 socket
bind()      - 绑定地址（直接调用真实 API）
listen()    - 监听（发送 LISTEN_EVENT 给 desd）
accept()    - 接受连接（阻塞在 desd）
connect()   - 发起连接（阻塞在 desd）
send()      - 发送数据（阻塞在 desd）
recv()      - 接收数据（阻塞在 desd）
select()    - 多路复用（阻塞在 desd）
poll()      - 多路复用（阻塞在 desd）
sleep()     - 休眠（推进虚拟时间）
close()     - 关闭 socket
```

### 3. common.h/c (公共定义)

**职责**：
- 定义消息格式和事件类型
- JSON 序列化/反序列化
- 常量定义

---

## 关键数据结构

### 1. RouterInfo（路由器状态）

```c
typedef struct {
    int router_id;                       // 路由器 ID
    int comm_socket_fd;                  // 与 desd 通信的 socket
    RouterStatus status;                 // 状态：IDLE/RUNNING/BLOCKED
    double virtual_time;                 // 虚拟时间
    
    // 阻塞信息
    char blocked_on_function[64];        // 阻塞在哪个函数上
    char blocked_on_request_id[64];      // 阻塞请求的 ID
    
    // 监听地址（支持多 listen）
    char listen_addresses[MAX_LISTEN_ADDRESSES][256];
    int listen_address_count;
    
    // 连接映射表
    ConnectionInfo connections[MAX_CONNECTIONS_PER_ROUTER];
    
    // 数据包缓冲
    PacketBuffer packet_buffers[MAX_PENDING_PACKETS];
    int pending_buffer_indices[MAX_PENDING_PACKETS];
    int pending_buffer_head;
    int pending_buffer_tail;
    int pending_buffer_count;
    
    // 待处理连接数
    int pending_connections_count;
} RouterInfo;
```

### 2. ConnectionInfo（连接信息）

```c
typedef struct {
    int socket_fd;          // 本地 socket fd
    int peer_router_id;     // 对端路由器 ID
    int peer_socket_fd;     // 对端 socket fd（用于多连接场景的精确匹配）
    int is_active;          // 连接是否活跃
} ConnectionInfo;
```

**关键点**：
- `peer_socket_fd` 字段用于支持多连接精确匹配（如 poll 测试中的多个连接）
- 每个路由器最多支持 `MAX_CONNECTIONS_PER_ROUTER` (10) 个连接

### 3. PacketBuffer（数据包缓冲）

```c
typedef struct {
    char data[MAX_PACKET_SIZE];  // 数据内容（hex 编码）
    size_t data_len;             // 数据长度
    int socket_fd;               // 对应的 socket 文件描述符
    int is_used;                 // 是否被使用
} PacketBuffer;
```

**关键点**：
- `socket_fd` 字段用于精确匹配数据包属于哪个连接（支持 poll 的 FD 精确匹配）
- 数据以 hex 编码存储，避免特殊字符问题

### 4. Event（事件）

```c
typedef struct {
    double timestamp;           // 事件发生的虚拟时间
    int router_id;              // 相关路由器 ID
    EventType event_type;       // 事件类型
    uint64_t event_id;          // 事件唯一 ID
    Payload payload;            // 事件负载（JSON 字符串）
} Event;
```

**事件类型**：
```c
typedef enum {
    ROUTER_START,                    // 路由器启动
    LISTEN_EVENT,                    // listen 调用
    CONNECT_REQUEST_EVENT,           // connect 请求
    CONNECTION_ESTABLISHED_EVENT,    // 连接建立
    CONNECTION_INFO_EVENT,           // 连接信息（accept 后）
    PACKET_SEND_EVENT,               // 数据包发送
    PACKET_RECEIVE_EVENT,            // 数据包接收
    ROUTER_BLOCK_REQUEST,            // 路由器阻塞请求
    TIMEOUT_EVENT,                   // 超时事件
    UNKNOWN_EVENT
} EventType;
```

---

## 事件流程

### 1. 连接建立流程

```
客户端 (R1)                    desd                    服务器 (R2)
     |                           |                           |
     |--- listen() ------------->|                           |
     |<-- 立即返回 --------------|                           |
     |                           |                           |
     |                           |<-- listen() --------------|
     |                           |--- 记录监听地址 ---------|
     |                           |--- 立即返回 ------------>|
     |                           |                           |
     |--- connect(fd:4) -------->|                           |
     |    (阻塞)                 |                           |
     |                           |--- accept() 阻塞 -------->|
     |                           |                           |
     |                    创建事件队列：                      |
     |            CONNECTION_ESTABLISHED(R1) @ VT=0.049      |
     |            CONNECTION_ESTABLISHED(R2) @ VT=0.050      |
     |                           |                           |
     |                    [VT=0.049]                         |
     |<-- SUCCESS ---------------|                           |
     |    real_connect()         |                           |
     |    返回成功               |                           |
     |                           |                           |
     |                    [VT=0.050]                         |
     |                           |<-- SUCCESS ---------------|
     |                           |    real_accept()          |
     |                           |    返回 fd:5              |
     |                           |                           |
     |                           |<-- CONNECTION_INFO -------|
     |                           |    (fd:5, is_client:false)|
     |                           |--- 注册连接 ------------>|
```

**关键时序**：
- 客户端事件在 VT-0.001（0.049），服务器事件在 VT（0.050）
- 确保 `real_connect()` 先于 `real_accept()`，避免内核阻塞

### 2. 数据传输流程

```
发送方 (R1)                    desd                    接收方 (R2)
     |                           |                           |
     |--- send(fd:4, "hello") -->|                           |
     |                           |                           |
     |                    [VT=0.051] (延迟 2ms)              |
     |                    PACKET_SEND_EVENT                  |
     |                           |                           |
     |                    查找连接映射：                      |
     |                    R1(fd:4) -> R2(fd:5)               |
     |                           |                           |
     |                    缓冲数据：                          |
     |                    PacketBuffer[socket_fd:5]          |
     |                           |                           |
     |<-- SUCCESS ---------------|                           |
     |                           |                           |
     |                    创建 PACKET_RECEIVE_EVENT          |
     |                    @ VT=0.151 (延迟 100ms)            |
     |                           |                           |
     |                    [VT=0.151]                         |
     |                           |                           |
     |                    如果 R2 阻塞在 recv():             |
     |                           |--- 唤醒 R2 -------------->|
     |                           |<-- recv() 调用 ----------|
     |                           |--- 返回数据 ------------>|
     |                           |                           |
     |                    如果 R2 未调用 recv():             |
     |                    数据保持在缓冲区                   |
```

**关键点**：
- **PACKET_SEND_EVENT 延迟 2ms**：确保在服务器 accept() 之后处理
- 数据传输延迟：100ms（可配置）
- 数据在 desd 缓冲，不在真实 socket 中

### 3. select/poll 流程

```
路由器 (R1)                    desd
     |                           |
     |--- select([fd:4, fd:5]) ->|
     |                           |
     |                    检查是否有待处理数据               |
     |                    PacketBuffer 中 socket_fd 匹配     |
     |                           |
     |                    如果有数据：                        |
     |<-- SUCCESS ---------------|
     |    ready_fds: [4]         |  (只返回有数据的 FD)
     |                           |
     |                    如果无数据：                        |
     |    阻塞，注册 TIMEOUT      |
     |                           |
     |                    等待 PACKET_RECEIVE_EVENT 或 TIMEOUT |
```

**poll() 的增强支持（完整事件类型）**：

**请求格式**（libdeshook → desd）：
```json
{
  "monitored_fds": [
    {"fd": 4, "events": 0x001},  // POLLIN
    {"fd": 5, "events": 0x005}   // POLLIN | POLLOUT
  ],
  "timeout_ms": 5000
}
```

**响应格式**（desd → libdeshook）：
```json
{
  "ready_fds": [
    {"fd": 4, "revents": 0x001},  // POLLIN (有数据可读)
    {"fd": 5, "revents": 0x004}   // POLLOUT (可写)
  ]
}
```

**支持的事件类型**：
- `POLLIN (0x001)` - 有数据可读（检查 PacketBuffer）
- `POLLOUT (0x004)` - socket 可写（检查连接状态）
- `POLLERR (0x008)` - 错误条件（检查连接错误）
- `POLLHUP (0x010)` - 连接挂起（检查连接是否活跃）
- `POLLNVAL (0x020)` - 无效请求（基础支持）

**处理逻辑**：
1. libdeshook 发送包含每个 fd 的 events 信息
2. desd 根据不同事件类型进行检查：
   - POLLIN：遍历 PacketBuffer，查找有数据的 socket_fd
   - POLLOUT：检查连接是否存在且活跃
   - POLLHUP：检查连接是否已断开
3. desd 返回每个 fd 的 revents
4. libdeshook 精确设置每个 `pollfd.revents`

---

## 连接管理机制

### 连接映射表的建立

```c
// 1. 客户端 connect 时（CONNECTION_ESTABLISHED_EVENT）
register_connection(client_id, client_fd, server_id, -1);
// R1 (fd:4) <-> R2 (fd:-1)  // 服务器 fd 未知

// 2. 服务器 accept 后（CONNECTION_INFO_EVENT）
register_connection(server_id, server_fd, client_id, client_fd);
// R2 (fd:5) <-> R1 (fd:4)  // 完整映射

// 同时更新客户端记录
R1.connections[i].peer_socket_fd = server_fd;
// R1 (fd:4) <-> R2 (fd:5)  // 完整映射
```

### 查找连接

```c
// 查找对端路由器
int find_peer_router(int router_id, int socket_fd)

// 查找对端 socket_fd（精确匹配）
int find_socket_fd_for_peer(int router_id, int peer_router_id, int peer_socket_fd)
```

### 多连接支持

通过 `peer_socket_fd` 字段支持同一对路由器之间的多个连接：

```
R1 -> R2:
  连接1: R1(fd:4) <-> R2(fd:5)
  连接2: R1(fd:6) <-> R2(fd:7)
  连接3: R1(fd:8) <-> R2(fd:9)

查找时精确匹配 peer_socket_fd，避免混淆
```

---

## 虚拟时间管理

### 时间推进规则

1. **事件驱动**：只在处理事件时推进时间
   ```c
   current_virtual_time = event.timestamp;
   ```

2. **路由器操作瞬时**：大部分操作在当前虚拟时间发生
   ```c
   next_event.timestamp = current_virtual_time;
   ```

3. **网络延迟模拟**：
   ```c
   // 连接建立延迟：50ms
   connection_time = current_virtual_time + 0.05;
   
   // 数据传输延迟：100ms
   receive_time = current_virtual_time + 0.1;
   
   // SEND 事件延迟：2ms（确保 accept 先完成）
   send_time = current_virtual_time + 0.002;
   ```

### 超时处理

```c
// 注册超时事件
if (timeout_ms >= 0) {
    double timeout_time = current_virtual_time + (timeout_ms / 1000.0);
    Event timeout_event = {
        .timestamp = timeout_time,
        .event_type = TIMEOUT_EVENT,
        ...
    };
    push_event(timeout_event);
}

// 数据到达时取消超时
cancel_event(timeout_event_id);
```

---

## 重要设计决策

### 1. PACKET_SEND_EVENT 延迟 2ms

**问题**：客户端在 connect() 返回后立即 send()，但此时服务器可能还没 accept()。

**原始方案（复杂）**：
- 预注册服务器连接（socket_fd=-1）
- 允许 send() 时 target_socket_fd=-1
- 在 accept() 后批量更新待处理包的 socket_fd

**最终方案（简洁）**：
- 给 PACKET_SEND_EVENT 增加 2ms 延迟
- 确保在服务器 accept() (VT+0.001) 之后处理
- 代码简单，逻辑清晰

```c
// desd.c - desd_event_loop()
double event_timestamp = current_virtual_time;
if (next_msg_from_router.event_type == PACKET_SEND_EVENT) {
    event_timestamp = current_virtual_time + 0.002;
}
```

### 2. 客户端不发送 CONNECTION_INFO_EVENT

**原因**：
- 客户端的 socket_fd 在 CONNECT_REQUEST_EVENT 中已经发送
- CONNECTION_ESTABLISHED_EVENT 中已经注册了客户端连接
- 只有服务器需要发送（通知 accept() 返回的 fd）

### 3. 数据在 desd 缓冲，不在真实 socket

**原因**：
- 虚拟时间与真实时间分离
- desd 控制数据何时"到达"
- 支持任意网络延迟模拟

**影响**：
- select/poll 不能调用 `real_select/real_poll`
- 需要 desd 返回 `ready_fds` 列表

### 4. 双层阻塞机制

**虚拟时间阻塞**（desd 控制）：
```c
// recv() 时如果没数据
router_states[id].status = BLOCKED;
router_states[id].blocked_on_function = "RECV_CALL";
// 不返回响应，路由器阻塞在 recv()
```

**真实系统调用**（内核阻塞）：
```c
// desd 返回 SUCCESS 后
int n = real_recv(sockfd, buf, len, 0);
// 此时数据已在真实 socket 中，立即返回
```

---

## 已实现功能

### ✅ 基本网络操作
- [x] socket 创建
- [x] bind/listen（直接调用，记录监听地址）
- [x] connect/accept（虚拟时间阻塞）
- [x] send/recv（虚拟时间阻塞）
- [x] close（直接调用）

### ✅ 通信协议支持
- [x] Unix Domain Socket (UDS)
- [x] TCP/IP (AF_INET)
- [x] UDS 和 TCP 混合使用
- [x] 地址抽象化（UDS 路径 / TCP "IP:Port"）

### ✅ 多路复用
- [x] select() 基本支持（不精确 FD 匹配）
- [x] poll() 完整支持（精确 FD 匹配）**【已增强】**
  - 发送 `monitored_fds` 列表（包含events信息）
  - 接收 `ready_fds` 列表（包含revents信息）
  - 精确设置 `pollfd.revents`
  - **支持 POLLIN/POLLOUT/POLLERR/POLLHUP** 等多种事件
  - 测试程序：`r_poll_server_enhanced.c`, `r_poll_client_enhanced.c`

### ✅ 时间管理
- [x] sleep() 虚拟时间推进
- [x] 超时机制（select/poll）
- [x] 超时取消（数据到达时）

### ✅ 高级特性
- [x] 多路由器支持（当前配置 2 个）
- [x] 多连接支持（单个路由器最多 10 个连接）
- [x] 多监听地址支持（单个路由器最多 5 个地址）
- [x] 精确连接映射（peer_socket_fd）
- [x] 数据包缓冲和排队

---

## 代码结构

```
des_design/
├── desd.c                          # 中央调度器
├── libdeshook.c                    # API 拦截层
├── common.h                        # 公共定义
├── common.c                        # 公共函数实现
├── r1.c                            # 测试程序：UDS 客户端
├── r2.c                            # 测试程序：UDS 服务端
├── r1_test_tcp.c                   # 测试程序：TCP 客户端
├── r2_test_tcp.c                   # 测试程序：TCP 服务端
├── r1_timeout_test.c               # 超时测试：客户端
├── r2_timeout_test.c               # 超时测试：服务端
├── r_poll_server_test_v2.c         # poll 测试：服务端（多连接）
├── r_poll_client_test_v2.c         # poll 测试：客户端（多连接）
├── r_poll_server_enhanced.c        # poll 增强测试：服务端（多事件类型）
├── r_poll_client_enhanced.c        # poll 增强测试：客户端（多事件类型）
├── run_poll_test_auto_v2.sh        # poll 自动化测试脚本
├── run_poll_enhanced_test.sh       # poll 增强功能测试脚本
├── run_tcp_test.sh                 # TCP 自动化测试脚本
├── Makefile                        # 构建脚本
└── *.md                            # 文档
    ├── PROJECT_OVERVIEW.md         # 项目总览（本文档）
    ├── README.md                   # 快速开始指南
    ├── POLL_SUPPORT.md             # poll() 支持文档
    ├── POLL_ENHANCEMENT_REPORT.md  # poll 增强功能报告
    ├── POLL_USAGE_GUIDE.md         # poll 使用指南
    ├── TCP_SUPPORT.md              # TCP 支持文档
    └── 其他文档...

编译产物：
├── desd                            # 调度器可执行文件
├── libdeshook.so                   # 拦截库
├── r1, r2                          # 测试程序
└── /tmp/desd_control_socket        # desd 监听的 Unix socket
```

### 编译

```bash
make clean
make
```

### 运行

```bash
# 终端 1：启动 desd
sudo ./desd

# 终端 2：启动 R2（服务器）
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r2

# 终端 3：启动 R1（客户端）
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r1
```

---

## 常见问题和解决方案

### 1. 连接时卡死

**症状**：客户端 connect() 或服务器 accept() 卡住

**原因**：
- 路由器启动顺序问题
- 客户端先 connect，但服务器还没 listen
- 服务器 accept 阻塞在内核（没有真实连接）

**解决方案**：
- 确保服务器先启动并 listen
- CONNECTION_ESTABLISHED_EVENT 时序：客户端先于服务器 1ms
- 检查 desd 日志确认事件处理顺序

### 2. send() 报错 "Invalid connection mapping"

**症状**：客户端 send() 失败

**原因**：
- 服务器还没 accept()，连接映射未建立
- PACKET_SEND_EVENT 在 CONNECTION_INFO_EVENT 之前处理

**解决方案**：
- PACKET_SEND_EVENT 延迟 2ms（已实现）
- 确保服务器正常 accept 并发送 CONNECTION_INFO_EVENT

### 3. poll() 返回错误的就绪 FD

**症状**：poll() 返回的 FD 不准确

**原因**：
- PacketBuffer 中 socket_fd 未正确设置
- desd 未返回 ready_fds 列表
- libdeshook 未精确设置 revents

**解决方案**：
- 确保 PacketBuffer.socket_fd 正确记录
- desd 正确收集 ready_fds
- libdeshook 遍历 ready_fds 精确匹配

### 4. 超时不工作

**症状**：select/poll 超时未触发或提前触发

**原因**：
- 未注册 TIMEOUT_EVENT
- 超时时间计算错误
- 数据到达后未取消超时事件

**解决方案**：
- 检查 TIMEOUT_EVENT 创建和注册
- 验证 `timeout_time = current_vt + (timeout_ms/1000.0)`
- 确保 PACKET_RECEIVE_EVENT 时调用 `cancel_event()`

### 5. 虚拟时间不推进

**症状**：所有事件卡在同一虚拟时间

**原因**：
- 事件队列为空
- 所有路由器都在 BLOCKED 状态
- 没有新事件被创建

**解决方案**：
- 检查路由器是否正确发送事件
- 确认 desd 正确创建延迟事件
- 查看 desd 日志中的事件队列状态

---

## 测试说明

### 1. 基本通信测试

**测试程序**：`r1.c`, `r2.c`

**测试内容**：
- 基本 connect/accept
- send/recv 数据传输
- 多轮通信

**运行方式**：见上文"运行"部分

### 2. 超时测试

**测试程序**：`r1_timeout_test.c`, `r2_timeout_test.c`

**测试内容**：
- Case 1: 数据在超时前到达（select 成功）
- Case 2: 超时触发（select 返回 0）
- Case 3: 延迟数据最终到达（缓冲机制）

**预期结果**：
```
R2 TEST:
✓ Data available before timeout
⏰ TIMEOUT! No data received
✓ Received delayed message
```

### 3. poll() FD 精确匹配测试

**测试程序**：`r_poll_server_test_v2.c`, `r_poll_client_test_v2.c`

**测试内容**：
- 服务器监听单个地址，accept() 3 次
- 3 个客户端连接，每个发送不同数据
- 服务器使用 poll() 监听 3 个 FD
- 验证 poll() 返回的 FD 准确性

**运行方式**：
```bash
sudo ./run_poll_test_auto_v2.sh
```

**预期结果**：
```
Poll returned 2 ready FDs (expected 2) ✓
  FD 6: revents=POLLIN ✓
  FD 8: revents=POLLIN ✓
```

### 4. poll() 增强功能测试（多事件类型）

**测试程序**：`r_poll_server_enhanced.c`, `r_poll_client_enhanced.c`

**测试内容**：
- **Test 1: POLLOUT** - 检查 socket 可写状态
- **Test 2: POLLIN** - 检查数据可读状态
- **Test 3: POLLIN | POLLOUT** - 同时监听多种事件

**测试的事件类型**：
- `POLLIN (0x001)` - 有数据可读
- `POLLOUT (0x004)` - socket 可写
- `POLLHUP (0x010)` - 连接挂起

**运行方式**：
```bash
sudo ./run_poll_enhanced_test.sh
```

**预期结果**：
```
========== Test Summary ==========
✓ POLLOUT test PASSED
✓ POLLIN test PASSED
✓ Combined events test PASSED

Final Score: 3 passed, 0 failed
✓ All tests PASSED!
```

**详细输出示例**：
```
R2 (Server):
--- Test 1: POLLOUT (socket writable) ---
✓ poll() returned 1 ready FD(s)
✓ POLLOUT detected: socket is writable
✓ Sent 18 bytes: "Hello from server!"

--- Test 2: POLLIN (data available) ---
✓ poll() returned 1 ready FD(s)
✓ POLLIN detected: data available
✓ Received 18 bytes: "Hello from client!"

--- Test 3: POLLIN | POLLOUT (both events) ---
✓ poll() returned 1 ready FD(s)
  revents = 0x0004 (POLLOUT)
✓ Socket is writable

R1 (Client):
--- Test 1: POLLIN (waiting for server message) ---
✓ poll() returned 1 ready FD(s)
✓ POLLIN detected: data available
✓ Received 18 bytes: "Hello from server!"

--- Test 2: POLLOUT (socket writable) ---
✓ poll() returned 1 ready FD(s)
✓ POLLOUT detected: socket is writable
✓ Sent 18 bytes: "Hello from client!"

--- Test 3: POLLIN | POLLOUT (both events) ---
✓ poll() returned 1 ready FD(s)
  revents = 0x0015 (POLLIN POLLOUT POLLHUP)
✓ Socket is writable
✓ Data is available
```

**相关文档**：
- `POLL_ENHANCEMENT_REPORT.md` - 详细的实施报告
- `POLL_USAGE_GUIDE.md` - 使用指南和示例

---

## 关键代码位置

### desd.c

| 功能 | 函数 | 行号范围 |
|------|------|---------|
| 主事件循环 | `desd_event_loop()` | ~540-670 |
| 连接请求处理 | `handle_connect_request_event()` | ~810-910 |
| 连接建立处理 | `handle_connection_established_event()` | ~920-980 |
| 连接信息处理 | `handle_connection_info_event()` | ~990-1075 |
| 数据发送处理 | `handle_packet_send_event()` | ~1370-1470 |
| 数据接收处理 | `handle_packet_receive_event()` | ~1480-1600 |
| 阻塞请求处理 | `handle_router_block_request()` | ~1100-1360 |
| 连接管理 | `register_connection()` | ~280-310 |
| 查找对端路由器 | `find_peer_router()` | ~315-330 |
| 查找对端 socket | `find_socket_fd_for_peer()` | ~330-345 |

### libdeshook.c

| 功能 | 函数 | 行号范围 |
|------|------|---------|
| 初始化 | `lib_init()` | ~50-130 |
| connect 拦截 | `connect()` | ~195-280 |
| accept 拦截 | `accept()` | ~285-370 |
| send 拦截 | `send()` | ~400-490 |
| recv 拦截 | `recv()` | ~530-680 |
| select 拦截 | `select()` | ~730-820 |
| poll 拦截 | `poll()` | ~830-920 |
| sleep 拦截 | `sleep()` | ~685-730 |

---

## 下一步改进方向

### 性能优化
- [ ] 减少 JSON 序列化/反序列化开销
- [ ] 优化事件队列操作
- [ ] 支持更多路由器（当前 2 个）

### 功能扩展
- [x] ~~支持 TCP~~ （已完成）
- [ ] 支持 UDP
- [ ] 支持 IPv6 (AF_INET6)
- [ ] 支持 epoll
- [ ] 动态拓扑变化
- [ ] 数据包丢失模拟
- [ ] 更复杂的网络延迟模型

### 稳定性
- [ ] 更完善的错误处理
- [ ] 资源清理和防泄漏
- [ ] 异常场景覆盖

### 测试
- [ ] 更多边界情况测试
- [ ] 并发连接压力测试
- [ ] 长时间运行稳定性测试

---

## 附录：关键常量

```c
#define MAX_ROUTERS 2                      // 最大路由器数量
#define MAX_CONNECTIONS_PER_ROUTER 10      // 每个路由器最大连接数
#define MAX_PENDING_PACKETS 20             // 最大待处理数据包数
#define MAX_LISTEN_ADDRESSES 5             // 每个路由器最大监听地址数
#define MAX_PACKET_SIZE 4096               // 最大数据包大小
#define MAX_MSG_SIZE 8192                  // 最大消息大小
#define MAX_EVENTS 1000                    // 事件队列最大容量
#define MAX_ACTIVE_EVENTS 10000            // 最大活跃事件数
```

---

## 总结

DES 项目实现了一个功能完整的分布式事件模拟器，通过 LD_PRELOAD 机制透明地拦截 socket API，使用虚拟时间和事件驱动模型模拟网络行为。

**核心优势**：
1. **透明性**：应用程序无需修改
2. **精确性**：完全控制时间和事件顺序
3. **灵活性**：易于调整网络参数
4. **可扩展性**：易于添加新功能

**关键设计**：
1. PACKET_SEND_EVENT 延迟 2ms 解决时序问题
2. 精确连接映射支持多连接场景
3. poll() 的 ready_fds 机制支持精确 FD 匹配
4. 双层阻塞机制分离虚拟时间和真实系统调用

通过阅读本文档，新的对话会话应该能够快速理解项目架构、关键设计决策和代码结构，从而能够有效地修改和扩展代码。

