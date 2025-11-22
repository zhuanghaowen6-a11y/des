# 完整实现解释：从 poll() 支持到连接映射修复

## 目录
1. [背景：DES 框架的基本架构](#背景des-框架的基本架构)
2. [第一阶段：poll() 基础支持（方案A）](#第一阶段poll-基础支持方案a)
3. [第二阶段：poll() 精确 FD 匹配（方案B）](#第二阶段poll-精确-fd-匹配方案b)
4. [第三阶段：连接映射修复（支持多连接）](#第三阶段连接映射修复支持多连接)
5. [当前系统的完整逻辑](#当前系统的完整逻辑)

---

## 背景：DES 框架的基本架构

### DES 是什么？

**Distributed Event Simulator (分布式事件模拟器)**

- 一个用于网络协议研究的**虚拟时间模拟框架**
- 允许多个路由器程序以**虚拟时间**而非真实时间运行
- 通过 `desd` (DES daemon) 统一调度所有事件

### 核心组件

```
┌─────────────────────────────────────────────────────────┐
│                    用户程序 (r1, r2)                      │
│  socket(), bind(), listen(), accept(), connect(),        │
│  send(), recv(), select(), poll(), sleep()...            │
└──────────────────┬──────────────────────────────────────┘
                   │ 被 LD_PRELOAD 拦截
                   ↓
┌─────────────────────────────────────────────────────────┐
│              libdeshook.so (钩子库)                       │
│  - 拦截 socket API 调用                                   │
│  - 与 desd 通信，阻塞/唤醒路由器                          │
│  - 管理虚拟时间                                           │
└──────────────────┬──────────────────────────────────────┘
                   │ Unix socket 通信
                   ↓
┌─────────────────────────────────────────────────────────┐
│                  desd (中央调度器)                        │
│  - 维护全局虚拟时间                                       │
│  - 管理事件队列 (最小堆)                                  │
│  - 控制路由器状态 (IDLE/RUNNING/BLOCKED)                 │
│  - 缓冲数据包                                             │
│  - 调度网络延迟                                           │
└─────────────────────────────────────────────────────────┘
```

### 关键概念

#### 1. 虚拟时间 vs 真实时间

```c
// 真实时间：墙上时钟时间
real_time: 14:30:00 → 14:30:01 → 14:30:02 ...

// 虚拟时间：由 desd 控制，可以快速推进
virtual_time: 0.0 → 0.1 → 1.5 → 2.0 ...
```

- **真实时间**：程序等待时阻塞在 desd 上（几乎不消耗真实时间）
- **虚拟时间**：由事件驱动，可以模拟数小时/数天的网络行为

#### 2. 事件驱动架构

```c
// desd 的事件队列（最小堆）
Event queue:
  [0.1s] PACKET_RECEIVE_EVENT for R1
  [0.2s] TIMEOUT_EVENT for R2
  [0.5s] CONNECTION_ESTABLISHED_EVENT for R1
  [1.0s] PACKET_SEND_EVENT for R2
  ...
```

- 事件按虚拟时间戳排序
- desd 依次处理事件，推进虚拟时间
- 路由器在 desd 控制下运行，不能自主推进时间

#### 3. 路由器状态

```c
typedef enum {
    IDLE,      // 初始状态
    RUNNING,   // 正在执行（未阻塞在 desd）
    BLOCKED    // 阻塞在 desd 上等待事件
} RouterStatus;
```

- **RUNNING**: 路由器可以执行代码
- **BLOCKED**: 路由器等待 desd 唤醒（如等待数据、等待连接）

---

## 第一阶段：poll() 基础支持（方案A）

### 问题：poll() 原本是如何工作的？

在原始的 DES 框架中，**poll() 没有被实现**。

```c
// 原始代码：poll() 直接调用真实的系统 poll()
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return real_poll(fds, nfds, timeout);  // ← 脱离 desd 控制！
}
```

**问题：**
1. 绕过了虚拟时间系统
2. 会阻塞在真实的 socket 上
3. 无法模拟网络延迟

### 解决方案：拦截 poll() 并与 desd 交互

#### 修改 1: libdeshook.c - 拦截 poll()

```c
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    // 1. 先非阻塞检查（timeout=0）
    int ready = real_poll(fds, nfds, 0);
    if (ready > 0) {
        return ready;  // 有数据立即返回
    }
    
    // 2. 没有数据，向 desd 注册阻塞事件
    Message block_req;
    block_req.event_type = ROUTER_BLOCK_REQUEST;
    // payload: { blocked_function: "SELECT_CALL", timeout_ms: timeout }
    
    // 3. 发送给 desd，阻塞等待
    send_msg_to_desd_and_wait_for_response(&block_req, &resp);
    
    // 4. desd 唤醒后，调用真实 poll()
    return real_poll(fds, nfds, 0);
}
```

**关键点：**
- `SELECT_CALL` 是历史原因，表示 select/poll 类的多路复用
- 阻塞在 desd 上，而不是真实 socket 上
- desd 控制何时唤醒

#### 修改 2: desd.c - 处理 SELECT_CALL

```c
void handle_router_block_request(Event event) {
    // ...
    if (strcmp(blocked_function, "SELECT_CALL") == 0) {
        // 检查是否有 pending 数据包
        if (router_states[router_id].pending_packets_count > 0) {
            // 有数据，立即唤醒
            send_success_response(router_id, request_id, "SELECT", "Data Available");
        } else {
            // 无数据，阻塞路由器
            router_states[router_id].status = BLOCKED;
            
            // 如果有超时，注册超时事件
            if (timeout_ms > 0) {
                Event timeout_event = {
                    .timestamp = current_virtual_time + (timeout_ms / 1000.0),
                    .router_id = router_id,
                    .event_type = TIMEOUT_EVENT,
                    // ...
                };
                push_event(timeout_event);
            }
        }
    }
}
```

**关键点：**
- 检查 `pending_packets_count`（数据包缓冲区）
- 如果有数据，立即唤醒
- 如果无数据，阻塞路由器并注册超时事件

### 方案A的问题：无法精确区分 FD

```c
// 场景：监听 3 个 FD
poll(fds=[fd1, fd2, fd3], ...)

// 只有 fd1 有数据
// 但方案A会设置所有 FD 的 revents
fds[0].revents = POLLIN;  // ✓ 正确
fds[1].revents = POLLIN;  // ✗ 错误！fd2 没有数据
fds[2].revents = POLLIN;  // ✗ 错误！fd3 没有数据
```

**原因：**
- desd 只告诉 libdeshook "有数据"
- libdeshook 不知道具体哪个 FD 有数据
- 只能标记所有被监听的 FD

---

## 第二阶段：poll() 精确 FD 匹配（方案B）

### 目标：让 desd 告诉 libdeshook **哪些 FD** 有数据

### 核心思路

```
libdeshook                    desd
    |                           |
    |  monitored_fds: [5,6,7]   |
    |─────────────────────────→ |
    |                           |  检查 pending packets
    |                           |  fd=5 有数据 ✓
    |                           |  fd=6 无数据 ✗
    |                           |  fd=7 有数据 ✓
    |                           |
    |  ready_fds: [5, 7]        |
    |←───────────────────────── |
    |                           |
设置 revents:
  fds[0].revents = POLLIN (fd=5)
  fds[1].revents = 0      (fd=6)
  fds[2].revents = POLLIN (fd=7)
```

### 修改 1: PacketBuffer 增加 socket_fd 字段

**为什么需要这个？**

原来的数据包缓冲区：

```c
typedef struct {
    char data[MAX_PACKET_SIZE];   // 数据内容
    size_t data_len;               // 数据长度
    int is_used;                   // 是否被使用
} PacketBuffer;  // ← 问题：不知道这个包是从哪个 socket 收到的！
```

修改后：

```c
typedef struct {
    char data[MAX_PACKET_SIZE];
    size_t data_len;
    int socket_fd;                 // ← 新增：记录对应的 socket fd
    int is_used;
} PacketBuffer;
```

**例子：**

```
R1 有 3 个连接：fd=5, fd=6, fd=7

packet_buffers[0]: { data: "hello", socket_fd: 5 }  ← 来自 fd=5
packet_buffers[1]: { data: "world", socket_fd: 7 }  ← 来自 fd=7
```

### 修改 2: handle_packet_send_event 记录 socket_fd

**发送端发送数据时：**

```c
void handle_packet_send_event(Event event) {
    int source_router_id = event.router_id;
    int socket_fd = ...; // 从 payload 解析
    
    // 1. 查找目标路由器
    int target_router_id = find_peer_router(source_router_id, socket_fd);
    
    // 2. 查找目标路由器对应的 socket_fd
    int target_socket_fd = find_socket_fd_for_peer(target_router_id, source_router_id);
    
    // 3. 将数据存入缓冲区，并记录 socket_fd
    router_states[target_router_id].packet_buffers[i].socket_fd = target_socket_fd;
    //                                                 ↑↑↑↑↑↑↑↑↑↑
    //                                                 关键：记录目标 fd
}
```

**例子：**

```
R2 send(fd=4) 发送数据

1. desd 查找：R2 的 fd=4 连接到 R1
2. desd 查找：R1 对应的 socket 是 fd=5
3. desd 存储数据到 R1 的缓冲区：
   packet_buffers[0] = {
       data: "hello",
       socket_fd: 5     ← 记录是发给 R1 的 fd=5
   }
```

### 修改 3: handle_router_block_request 返回 ready_fds

**当路由器调用 poll() 时：**

```c
void handle_router_block_request(Event event) {
    // SELECT_CALL 处理
    if (router_states[router_id].pending_packets_count > 0) {
        // 构建就绪的 FD 列表
        json_t *ready_fds_array = json_array();
        
        // 遍历 pending buffer 队列，提取所有 socket_fd
        for (int i = 0; i < count; i++) {
            int buf_idx = pending_buffer_indices[i];
            int sock_fd = packet_buffers[buf_idx].socket_fd;
            
            // 去重添加
            if (!already_in_array(ready_fds_array, sock_fd)) {
                json_array_append_new(ready_fds_array, json_integer(sock_fd));
            }
        }
        
        // 发送响应，包含 ready_fds
        json_object_set_new(response, "ready_fds", ready_fds_array);
        send_message_to_router(router_id, &response);
    }
}
```

**例子：**

```
R1 的缓冲区：
  packet_buffers[0]: { socket_fd: 5 }
  packet_buffers[1]: { socket_fd: 7 }
  packet_buffers[2]: { socket_fd: 5 }  ← 重复

desd 返回：
  ready_fds: [5, 7]  ← 去重后的列表
```

### 修改 4: libdeshook.c poll() 精确设置 revents

```c
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    // ... 向 desd 发送请求 ...
    
    // 从 desd 响应中解析 ready_fds
    json_t *ready_fds_array = json_object_get(resp, "ready_fds");
    
    // 清零所有 revents
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
    }
    
    // 只设置就绪的 FD
    int ready_count = 0;
    for (size_t j = 0; j < json_array_size(ready_fds_array); j++) {
        int ready_fd = json_integer_value(json_array_get(ready_fds_array, j));
        
        // 在 fds 数组中查找这个 fd
        for (nfds_t i = 0; i < nfds; i++) {
            if (fds[i].fd == ready_fd && (fds[i].events & POLLIN)) {
                fds[i].revents = POLLIN;  // ← 精确设置
                ready_count++;
                break;
            }
        }
    }
    
    return ready_count;
}
```

**例子：**

```
输入：
  fds[0].fd = 5, fds[0].events = POLLIN
  fds[1].fd = 6, fds[1].events = POLLIN
  fds[2].fd = 7, fds[2].events = POLLIN

desd 返回：
  ready_fds = [5, 7]

输出：
  fds[0].revents = POLLIN  ✓ (5 在 ready_fds 中)
  fds[1].revents = 0       ✓ (6 不在 ready_fds 中)
  fds[2].revents = POLLIN  ✓ (7 在 ready_fds 中)
  return 2
```

---

## 第三阶段：连接映射修复（支持多连接）

### 问题：方案B在多连接场景下失败

#### 测试场景

```
服务端 R1：
  listen() on /tmp/poll_test_socket
  conn_fd1 = accept()  → fd=5
  conn_fd2 = accept()  → fd=6
  conn_fd3 = accept()  → fd=7

客户端 R2：
  sockfd1 = socket(); connect()  → fd=4
  sockfd2 = socket(); connect()  → fd=5
  sockfd3 = socket(); connect()  → fd=6
  
  send(fd=4, "hello")  ← 应该到达 R1 的 fd=5
  send(fd=6, "world")  ← 应该到达 R1 的 fd=7
```

#### 失败现象

```
R2 send(fd=4) → 成功 ✓
R2 send(fd=6) → 失败 ✗ "Invalid connection mapping"

desd 日志：
  [DESD ERROR] R2: Cannot find socket_fd for connection from R1 (source fd=6).
```

### 根本原因：连接映射表不完整

#### 原有的连接映射机制

```c
typedef struct {
    int socket_fd;          // 本地 socket fd
    int peer_router_id;     // 对端路由器ID
    int is_active;
} ConnectionInfo;
```

**问题：只记录了对端的路由器ID，没有记录对端的 socket fd！**

#### 查找对端 socket 的逻辑

```c
int find_socket_fd_for_peer(int router_id, int peer_router_id) {
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (connections[i].peer_router_id == peer_router_id) {
            return connections[i].socket_fd;  // ← 返回第一个匹配！
        }
    }
}
```

**例子：**

```
R1 的连接表：
  connections[0]: { socket_fd: 5, peer_router_id: 2 }  ← 第1个连接
  connections[1]: { socket_fd: 6, peer_router_id: 2 }  ← 第2个连接
  connections[2]: { socket_fd: 7, peer_router_id: 2 }  ← 第3个连接

R2 send(fd=4)：
  desd: 查找 R1 中连接到 R2 的 socket
  find_socket_fd_for_peer(R1, R2) → 返回 5  ✓ 正确

R2 send(fd=6)：
  desd: 查找 R1 中连接到 R2 的 socket
  find_socket_fd_for_peer(R1, R2) → 还是返回 5  ✗ 错误！
  
问题：无法区分 R2 的 fd=4, 5, 6 分别对应 R1 的哪个 fd
```

### 解决方案：记录对端的 socket fd

#### 修改 1: ConnectionInfo 增加 peer_socket_fd

```c
typedef struct {
    int socket_fd;          // 本地 socket fd
    int peer_router_id;     // 对端路由器ID
    int peer_socket_fd;     // ← 新增：对端 socket fd
    int is_active;
} ConnectionInfo;
```

**完整的连接映射：**

```
R1 的连接表：
  [0]: { socket_fd: 5, peer_router_id: 2, peer_socket_fd: 4 }
       ↑ 本地 fd    ↑ 连接到 R2    ↑ R2 的 fd=4
       
  [1]: { socket_fd: 6, peer_router_id: 2, peer_socket_fd: 5 }
       ↑ 本地 fd    ↑ 连接到 R2    ↑ R2 的 fd=5
       
  [2]: { socket_fd: 7, peer_router_id: 2, peer_socket_fd: 6 }
       ↑ 本地 fd    ↑ 连接到 R2    ↑ R2 的 fd=6

R2 的连接表（镜像）：
  [0]: { socket_fd: 4, peer_router_id: 1, peer_socket_fd: 5 }
  [1]: { socket_fd: 5, peer_router_id: 1, peer_socket_fd: 6 }
  [2]: { socket_fd: 6, peer_router_id: 1, peer_socket_fd: 7 }
```

#### 修改 2: find_socket_fd_for_peer 精确匹配

```c
int find_socket_fd_for_peer(int router_id, int peer_router_id, int peer_socket_fd) {
    for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
        if (connections[i].is_active &&
            connections[i].peer_router_id == peer_router_id &&
            connections[i].peer_socket_fd == peer_socket_fd) {  // ← 精确匹配
            return connections[i].socket_fd;
        }
    }
}
```

**例子：**

```
R2 send(fd=4)：
  find_socket_fd_for_peer(R1, peer_router_id=2, peer_socket_fd=4)
  → 找到 R1 的 connections[0]
  → 返回 socket_fd=5  ✓

R2 send(fd=6)：
  find_socket_fd_for_peer(R1, peer_router_id=2, peer_socket_fd=6)
  → 找到 R1 的 connections[2]
  → 返回 socket_fd=7  ✓
```

### 问题：如何建立双向映射？

#### 时序分析

```
时间线：

T1: R2 connect(fd=4)
    → libdeshook 发送 CONNECT_REQUEST_EVENT
    → desd 调度 CONNECTION_ESTABLISHED_EVENT

T2: desd 处理 CONNECTION_ESTABLISHED_EVENT (R2)
    → R2 connect() 成功
    → 此时：R2 知道自己的 fd=4，但不知道 R1 的 fd
    → libdeshook 发送 CONNECTION_INFO_EVENT { socket_fd: 4, is_client: true }
    
T3: desd 处理 CONNECTION_ESTABLISHED_EVENT (R1)
    → R1 accept() 成功，返回 fd=5
    → 此时：R1 知道自己的 fd=5，也知道客户端是 R2
    → libdeshook 发送 CONNECTION_INFO_EVENT { socket_fd: 5, is_client: false }

T4: desd 收到两个 CONNECTION_INFO_EVENT
    → 需要将它们关联起来！
```

#### 修改 3: CONNECTION_INFO_EVENT 建立双向映射

**客户端的 CONNECTION_INFO_EVENT：**

```c
if (is_client) {
    // 客户端刚 connect() 成功，发来 socket_fd
    // 此时对端的 socket_fd 还未知，先用 -1 占位
    register_connection(router_id, socket_fd, peer_router_id, -1);
    //                                                        ↑ 占位符
}
```

**服务端的 CONNECTION_INFO_EVENT：**

```c
if (!is_client) {
    // 服务端刚 accept() 成功，发来 socket_fd
    
    // 1. 在客户端的连接表中查找 peer_socket_fd == -1 的连接
    //    （从后往前查找，因为新连接在后面）
    int peer_socket_fd = -1;
    for (int i = MAX_CONNECTIONS_PER_ROUTER - 1; i >= 0; i--) {
        if (client_connections[i].peer_socket_fd == -1) {
            // 找到了！这是最新的未配对连接
            peer_socket_fd = client_connections[i].socket_fd;
            
            // 2. 更新客户端的连接：peer_socket_fd = 服务端的 socket_fd
            client_connections[i].peer_socket_fd = socket_fd;
            break;
        }
    }
    
    // 3. 注册服务端的连接
    register_connection(router_id, socket_fd, client_router_id, peer_socket_fd);
}
```

**完整的双向映射建立过程：**

```
步骤1: R2 connect(fd=4) 成功
  R2 连接表：
    [0]: { socket_fd: 4, peer_router_id: 1, peer_socket_fd: -1 }  ← 占位

步骤2: R1 accept() 返回 fd=5
  R1 查找 R2 的连接表，找到 peer_socket_fd=-1 的连接
  → 发现是 socket_fd=4
  
  R2 连接表（更新）：
    [0]: { socket_fd: 4, peer_router_id: 1, peer_socket_fd: 5 }  ← 更新！
  
  R1 连接表（创建）：
    [0]: { socket_fd: 5, peer_router_id: 2, peer_socket_fd: 4 }  ← 配对！

双向映射完成：
  R2 (fd=4) ←→ R1 (fd=5)
```

### 额外修复：connect() 拦截问题

#### 问题：只拦截到 `/tmp/router_socket` 的连接

```c
// 原始代码
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (strcmp(addr->sun_path, ROUTER_SOCKET_PATH) != 0) {
        return real_connect(...);  // ← 其他路径不拦截
    }
    // ...
}
```

**问题：**
- `ROUTER_SOCKET_PATH` 硬编码为 `/tmp/router_socket`
- 测试程序使用 `/tmp/poll_test_socket`
- 导致 connect() 没有被拦截！

#### 修复：拦截所有 Unix socket 连接

```c
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    // 只排除 desd 控制 socket
    if (addr->sa_family != AF_UNIX) {
        return real_connect(...);
    }
    
    const char *target_path = addr->sun_path;
    if (strcmp(target_path, DESD_CONTROL_SOCKET_PATH) == 0) {
        return real_connect(...);  // 不拦截 desd 控制连接
    }
    
    // 拦截所有其他 Unix socket 连接
    // ...
}
```

---

## 当前系统的完整逻辑

### 1. 连接建立流程

```
┌─────────┐                          ┌─────────┐
│   R2    │                          │   R1    │
│ (客户端) │                          │ (服务端) │
└────┬────┘                          └────┬────┘
     │                                    │
     │ 1. listen("/tmp/test")             │
     │    ├──────────────────────────────→│
     │    │ LISTEN_EVENT                  │
     │    │                               │ desd 记录监听地址
     │    │                               │
     │ 2. accept()                        │
     │    ├──────────────────────────────→│
     │    │ ROUTER_BLOCK_REQUEST          │
     │    │ (ACCEPT_CALL)                 │
     │    │                               │ R1 阻塞，等待连接
     │                                    │
     │ 3. connect(fd=4)                   │
     ├──────────────────────────────────→ │
     │    CONNECT_REQUEST_EVENT           │
     │    { socket_fd: 4 }                │
     │                                    │ desd 调度2个事件：
     │                                    │ - R2 的 CONNECTION_ESTABLISHED (T+0.049s)
     │                                    │ - R1 的 CONNECTION_ESTABLISHED (T+0.050s)
     │                                    │
     │ 4a. CONNECTION_ESTABLISHED(R2)     │
     │←───────────────────────────────────┤
     │    SUCCESS                         │
     │    real_connect(fd=4)              │
     │    ├───────────────────────────────┤
     │    │ CONNECTION_INFO_EVENT         │
     │    │ { socket_fd: 4, is_client: 1 }│
     │    │                               │ desd 记录：
     │    │                               │ R2: { fd: 4, peer: R1, peer_fd: -1 }
     │                                    │
     │                  4b. CONNECTION_ESTABLISHED(R1)
     │                                    │←───────
     │                                    │ SUCCESS
     │                                    │ real_accept() → fd=5
     │                                    ├───────→
     │                                    │ CONNECTION_INFO_EVENT
     │                                    │ { socket_fd: 5 }
     │                                    │
     │                                    │ desd 更新：
     │                                    │ R2: { fd: 4, peer: R1, peer_fd: 5 }
     │                                    │ R1: { fd: 5, peer: R2, peer_fd: 4 }
     │                                    │
     │      双向映射完成！                  │
     │      R2 (fd=4) ←→ R1 (fd=5)       │
```

### 2. 数据发送流程

```
R2 send(fd=4, "hello")
     │
     ├──────────────────────────────────→ desd
     │    PACKET_SEND_EVENT              
     │    { socket_fd: 4, data: "hello" }
     │                                    
     │                                    查找连接映射：
     │                                    1. find_peer_router(R2, fd=4) → R1
     │                                    2. find_socket_fd_for_peer(R1, R2, fd=4)
     │                                       → 查找 R1 的连接表
     │                                       → 匹配 peer_socket_fd=4
     │                                       → 返回 socket_fd=5
     │                                    
     │                                    缓冲数据：
     │                                    R1.packet_buffers[0] = {
     │                                        data: "hello",
     │                                        socket_fd: 5   ← 记录目标 fd
     │                                    }
     │                                    
     │                                    调度事件：
     │                                    PACKET_RECEIVE_EVENT for R1 at T+0.1s
```

### 3. poll() 调用流程

```
R1 poll(fds=[5,6,7], timeout=5000)
     │
     ├──────────────────────────────────→ desd
     │    ROUTER_BLOCK_REQUEST
     │    { blocked_function: "SELECT_CALL",
     │      monitored_fds: [5,6,7],
     │      timeout_ms: 5000 }
     │
     │                                    检查 pending packets：
     │                                    packet_buffers[0]: { socket_fd: 5 }
     │                                    packet_buffers[1]: { socket_fd: 7 }
     │                                    
     │                                    构建 ready_fds:
     │                                    ready_fds = [5, 7]  (去重)
     │
     │←─────────────────────────────────
     │    SUCCESS
     │    { ready_fds: [5, 7] }
     │
     │ 设置 revents：
     │ fds[0].revents = POLLIN  (fd=5 在 ready_fds 中)
     │ fds[1].revents = 0       (fd=6 不在 ready_fds 中)
     │ fds[2].revents = POLLIN  (fd=7 在 ready_fds 中)
     │
     └→ return 2
```

### 4. 数据接收流程

```
R1 recv(fd=5, buf, len)
     │
     ├──────────────────────────────────→ desd
     │    ROUTER_BLOCK_REQUEST
     │    { blocked_function: "RECV_CALL" }
     │
     │                                    检查 pending packets：
     │                                    找到 packet_buffers[0]
     │                                    
     │                                    从队列中取出：
     │                                    data = "hello"
     │                                    pending_packets_count--
     │
     │←─────────────────────────────────
     │    SUCCESS
     │    { packet_data: "hello" }
     │
     │ 解码数据，复制到 buf
     │
     └→ return strlen("hello")
```

---

## 关键数据结构总结

### ConnectionInfo（连接信息）

```c
typedef struct {
    int socket_fd;          // 本地 socket fd
    int peer_router_id;     // 对端路由器ID
    int peer_socket_fd;     // 对端 socket fd（用于精确匹配）
    int is_active;          // 连接是否活跃
} ConnectionInfo;
```

**用途：** 
- 支持多连接场景
- 精确匹配发送端和接收端的 socket

### PacketBuffer（数据包缓冲）

```c
typedef struct {
    char data[MAX_PACKET_SIZE];  // 数据内容（hex编码）
    size_t data_len;              // 数据长度
    int socket_fd;                // 对应的 socket fd
    int is_used;                  // 是否被使用
} PacketBuffer;
```

**用途：**
- 缓冲虚拟时间上已到达但未被读取的数据
- 记录 socket_fd 用于 poll() 精确匹配

### RouterState（路由器状态）

```c
typedef struct {
    RouterStatus status;                // IDLE/RUNNING/BLOCKED
    char blocked_on_request_id[64];     // 阻塞请求ID
    char blocked_on_function[64];       // 阻塞函数类型
    
    ConnectionInfo connections[10];     // 连接表
    PacketBuffer packet_buffers[100];   // 数据包缓冲区
    int pending_packets_count;          // 待读取数据包数量
    int pending_buffer_indices[100];    // 待读取数据包索引队列
    
    char listen_addresses[10][256];     // 监听地址列表
    // ...
} RouterState;
```

---

## 总结：为什么要这么做？

### 1. poll() 支持 → 保持虚拟时间系统完整性

**不拦截 poll() 的后果：**
- 路由器会阻塞在真实 socket 上
- 虚拟时间无法推进
- 无法模拟网络延迟
- 破坏了整个 DES 框架

### 2. 精确 FD 匹配 → 正确的多路复用行为

**不精确匹配的后果：**
- poll() 会误报就绪状态
- 程序逻辑错误（读取不该读的 socket）
- 无法真实模拟网络应用

### 3. 双向连接映射 → 支持多连接场景

**没有 peer_socket_fd 的后果：**
- 多个连接无法区分
- 数据包路由错误
- send() 失败
- 无法测试复杂网络拓扑

### 4. 完整的事件驱动架构 → 可重复的实验

**核心价值：**
- 所有行为由虚拟时间控制
- 实验可重复（相同输入→相同输出）
- 可以模拟极端场景（大延迟、丢包等）
- 快速执行（虚拟时间可以快速推进）

---

## 当前系统能做什么？

✅ **支持的功能：**
- 多个路由器并发运行
- 虚拟时间模拟
- 网络延迟模拟
- socket(), bind(), listen(), accept(), connect()
- send(), recv()
- select(), poll() 精确 FD 匹配
- sleep() 虚拟时间推进
- 超时机制
- 多连接场景
- 复杂网络拓扑

✅ **应用场景：**
- 网络协议研究
- 路由算法测试
- 分布式系统模拟
- 网络性能分析

---

**这就是整个实现的完整逻辑！** 🎉

每一步修改都是为了保持 DES 框架的**事件驱动本质**和**虚拟时间控制**，同时支持越来越复杂的网络场景。

