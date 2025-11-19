# TCP 支持文档

## 目录
1. [概述](#概述)
2. [设计原理](#设计原理)
3. [实现细节](#实现细节)
4. [使用方法](#使用方法)
5. [测试验证](#测试验证)
6. [兼容性说明](#兼容性说明)
7. [应用场景](#应用场景)

---

## 概述

DES (Distributed Event Simulator) 项目现已支持 **TCP 通信**，在原有的 Unix Domain Socket (UDS) 基础上扩展了对 AF_INET socket 的支持。这使得该项目能够模拟和控制使用真实 TCP 协议的路由器镜像（如 BIRD BGP）。

### 核心特性

- ✅ **完全透明**：应用程序无需修改代码，通过 LD_PRELOAD 自动拦截
- ✅ **保持兼容**：UDS 通信仍然完全支持，不受任何影响
- ✅ **统一管理**：TCP 和 UDS 连接在 desd 中使用相同的虚拟时间机制
- ✅ **地址抽象**：UDS 路径和 TCP "IP:Port" 格式统一处理

---

## 设计原理

### 双通道架构

```
路由器应用程序
    ↓
    ├─ 控制通道（UDS）：与 desd 通信（不变）
    │   /tmp/desd_control_socket
    │
    └─ 数据通道（TCP/UDS）：路由器间通信（新增TCP支持）
        - UDS: /tmp/router_socket
        - TCP: 127.0.0.1:5000
```

### 关键设计决策

#### 1. 控制通道保持 UDS

路由器与 desd 之间的控制消息通信继续使用 UDS，原因：
- 控制消息频繁，UDS 性能更好
- 本地进程间通信，无需网络协议开销
- 简化架构，避免端口冲突

#### 2. 数据通道支持 TCP

路由器之间的数据通信新增 TCP 支持：
- 支持真实的路由器镜像（BIRD、FRR 等）
- 与真实网络环境更接近
- 仍然通过 desd 进行虚拟时间控制

#### 3. 地址抽象化

desd 使用字符串统一表示所有地址：
- **UDS 格式**：`/tmp/router_socket`
- **TCP 格式**：`127.0.0.1:5000` 或 `0.0.0.0:8080`

这样 desd 的核心逻辑无需区分地址类型，大大简化了实现。

---

## 实现细节

### libdeshook.c 的改动

#### 1. connect() 拦截

**改动前**：只拦截 AF_UNIX socket

```c
if (addr->sa_family != AF_UNIX) {
    return real_connect(sockfd, addr, addrlen);
}
```

**改动后**：拦截 AF_UNIX 和 AF_INET

```c
// 构建抽象地址字符串
char abstract_address[256];

if (addr->sa_family == AF_UNIX) {
    // Unix domain socket: 使用路径作为抽象地址
    const char *target_path = ((struct sockaddr_un *)addr)->sun_path;
    strncpy(abstract_address, target_path, sizeof(abstract_address) - 1);
} else if (addr->sa_family == AF_INET) {
    // TCP socket: 使用 IP:Port 作为抽象地址
    struct sockaddr_in *tcp_addr = (struct sockaddr_in *)addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(tcp_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
    int port = ntohs(tcp_addr->sin_port);
    snprintf(abstract_address, sizeof(abstract_address), "%s:%d", ip_str, port);
}
```

#### 2. listen() 改进

**改动前**：只支持 UDS 地址

```c
struct sockaddr_un addr;
// ...
json_object_set_new(payload_obj, "listen_address", json_string(addr.sun_path));
```

**改动后**：支持 UDS 和 TCP 地址

```c
// 使用 sockaddr_storage 支持多种地址族
struct sockaddr_storage addr_storage;
// ...
char listen_address[256];
if (addr_storage.ss_family == AF_UNIX) {
    // UDS 地址
    strncpy(listen_address, un_addr->sun_path, sizeof(listen_address) - 1);
} else if (addr_storage.ss_family == AF_INET) {
    // TCP 地址：构建 "IP:Port" 格式
    snprintf(listen_address, sizeof(listen_address), "%s:%d", ip_str, port);
}
```

#### 3. bind() 日志增强

添加对 TCP bind 的日志输出，方便调试：

```c
if (addr->sa_family == AF_INET) {
    struct sockaddr_in *tcp_addr = (struct sockaddr_in *)addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(tcp_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
    int port = ntohs(tcp_addr->sin_port);
    printf("[LIBDESHOOK] R%d intercepted bind() to %s:%d.\n", my_router_id, ip_str, port);
}
```

#### 4. 新增头文件

```c
#include <netinet/in.h> // For sockaddr_in
#include <arpa/inet.h>  // For inet_ntop
```

### desd.c 的改动

#### 无需改动！

desd.c 的 `find_router_by_listen_address()` 函数已经使用 `strcmp()` 进行字符串比较，因此无论是 UDS 路径还是 TCP 的 "IP:Port" 格式都能正常工作。

```c
int find_router_by_listen_address(const char *address) {
    for (int i = 1; i <= MAX_ROUTERS; i++) {
        for (int j = 0; j < router_states[i].listen_count; j++) {
            if (strcmp(router_states[i].listen_addresses[j], address) == 0) {
                return i;  // 找到匹配的路由器
            }
        }
    }
    return -1;  // 未找到
}
```

这就是地址抽象化的优势：desd 核心逻辑完全不需要知道地址是 UDS 还是 TCP！

---

## 使用方法

### 编译

```bash
make clean
make test  # 编译所有程序，包括 TCP 测试程序
```

### 手动测试 TCP 通信

#### 终端 1：启动 desd

```bash
sudo ./desd
```

#### 终端 2：启动 R2（TCP 服务器）

```bash
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r2_test_tcp
```

#### 终端 3：启动 R1（TCP 客户端）

```bash
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r1_test_tcp
```

### 自动化测试

使用提供的测试脚本：

```bash
sudo ./run_tcp_test.sh
```

脚本会自动：
1. 编译所有程序
2. 启动 desd
3. 启动 R2 (TCP 服务器)
4. 启动 R1 (TCP 客户端)
5. 等待测试完成
6. 输出所有日志
7. 清理进程

---

## 测试验证

### TCP 测试程序说明

#### r2_test_tcp.c（TCP 服务器）

功能：
1. 创建 TCP socket
2. Bind 到 `127.0.0.1:5000`
3. Listen（通过 desd 注册）
4. Accept 连接（阻塞在 desd）
5. 接收客户端消息
6. 发送响应
7. 进行多轮通信

#### r1_test_tcp.c（TCP 客户端）

功能：
1. 创建 TCP socket
2. Connect 到 `127.0.0.1:5000`（阻塞在 desd）
3. 发送消息
4. 接收响应
5. 进行多轮通信

### 预期输出

**R1 (客户端)：**
```
[R1_TCP] Starting TCP client test...
[R1_TCP] Created TCP socket (fd: 4)
[LIBDESHOOK] R1 intercepted connect() to 127.0.0.1:5000 (family: AF_INET).
[R1_TCP] Connecting to 127.0.0.1:5000 (DESD controlled)...
[R1_TCP] Connected to 127.0.0.1:5000 successfully!
[R1_TCP] Sent message: "Hello from R1 (TCP Client)!" (28 bytes)
[R1_TCP] Received response: "Hello from R2 (TCP Server)!" (28 bytes)
...
```

**R2 (服务器)：**
```
[R2_TCP] Starting TCP server test...
[R2_TCP] Created TCP socket (fd: 4)
[R2_TCP] Bound to 127.0.0.1:5000
[LIBDESHOOK] R2 intercepted listen() on sockfd 4.
[R2_TCP] Listening on 127.0.0.1:5000 (DESD controlled)...
[R2_TCP] Accepted connection from 127.0.0.1:xxxxx (fd: 5)
[R2_TCP] Received 28 bytes: "Hello from R1 (TCP Client)!"
[R2_TCP] Sent response: "Hello from R2 (TCP Server)!" (28 bytes)
...
```

**DESD 日志：**
```
[DESD] R2 is now listening on 127.0.0.1:5000 (total: 1 address).
[DESD] R1 sent CONNECT_REQUEST (fd:4). Scheduled CONNECTION_ESTABLISHED_EVENT...
[DESD] Processing event CONNECTION_ESTABLISHED_EVENT for R1 at VT=0.049
[DESD] R1 (client) connect() completed with R2.
[DESD] Processing event CONNECTION_ESTABLISHED_EVENT for R2 at VT=0.050
[DESD] R2 (server) accept() completed due to connection from R1.
...
```

---

## 兼容性说明

### 向后兼容

✅ **完全兼容 UDS 通信**

所有现有的 UDS 测试程序（r1.c, r2.c, r1_timeout_test.c 等）无需任何修改即可继续工作。

### 同时支持 UDS 和 TCP

路由器可以同时使用 UDS 和 TCP：
- 监听多个地址（UDS 和 TCP 混合）
- 连接到 UDS 或 TCP 地址

示例：
```c
// 路由器可以同时监听 UDS 和 TCP
listen(uds_fd, 5);   // /tmp/router_socket
listen(tcp_fd, 5);   // 127.0.0.1:5000
```

### 不影响的功能

以下功能与 TCP 支持完全独立，不受任何影响：
- ✅ 虚拟时间管理
- ✅ 事件队列机制
- ✅ select/poll 多路复用
- ✅ 超时处理
- ✅ 数据包缓冲
- ✅ 连接映射

---

## 应用场景

### 1. BIRD BGP 路由器模拟

BIRD 是一个开源的路由守护进程，使用 TCP 端口 179 进行 BGP 通信。

**使用示例**：
```bash
# 启动 desd
sudo ./desd

# 启动 BIRD 实例 1
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so bird -c bird1.conf

# 启动 BIRD 实例 2
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so bird -c bird2.conf
```

BIRD 的 TCP 连接会被 libdeshook 拦截，通过 desd 进行虚拟时间控制。

### 2. FRRouting (FRR) 模拟

FRR 支持多种路由协议（BGP, OSPF, IS-IS 等），都基于 TCP。

### 3. 自定义路由器实现

开发者可以使用标准的 TCP socket API 实现路由器，无需关心虚拟时间管理。

---

## 技术细节

### 地址格式对比

| 协议 | bind 地址示例 | 抽象地址格式 |
|------|--------------|-------------|
| UDS  | `/tmp/router_socket` | `/tmp/router_socket` |
| TCP  | `127.0.0.1:5000` | `127.0.0.1:5000` |
| TCP  | `0.0.0.0:8080` | `0.0.0.0:8080` |

### 虚拟时间流程（TCP）

```
客户端 R1                    desd                    服务器 R2
   |                          |                          |
   |--- connect(TCP) -------->|                          |
   |    (127.0.0.1:5000)      |                          |
   |    阻塞在 desd           |                          |
   |                          |                          |
   |                          |<-- listen(TCP) ----------|
   |                          |    (127.0.0.1:5000)      |
   |                          |--- 注册监听地址 ---------|
   |                          |                          |
   |                          |<-- accept() 阻塞 --------|
   |                          |                          |
   |                   [VT=0.049]                        |
   |<-- SUCCESS ---------------|                          |
   |    real_connect()         |                          |
   |    TCP 三次握手           |                          |
   |                          |                          |
   |                   [VT=0.050]                        |
   |                          |<-- SUCCESS ---------------|
   |                          |    real_accept()          |
   |                          |    返回新 socket          |
   |                          |                          |
   |--- send(data) ---------->|                          |
   |                          |                          |
   |                   [VT=0.151]                        |
   |                          |--- PACKET_RECEIVE ------>|
   |                          |    (唤醒 recv)           |
   |                          |<-- recv() 读取 ---------|
```

**关键点**：
- TCP 三次握手仍然由内核完成（真实的 `real_connect()` 和 `real_accept()`）
- 但连接建立的虚拟时间由 desd 控制（延迟 50ms）
- 数据传输完全通过 desd 缓冲，不在真实 socket 中（与 UDS 一致）

### 性能考虑

#### TCP vs UDS 性能

在真实环境中，TCP 和 UDS 性能差异：
- **UDS**：更快，无需网络协议栈
- **TCP**：稍慢，需要经过 TCP/IP 协议栈

但在 DES 中：
- ✅ **性能差异被抵消**：所有数据都通过 desd 缓冲，不走真实的内核传输
- ✅ **虚拟时间统一**：TCP 和 UDS 使用相同的延迟模型（100ms 传输延迟）

---

## 常见问题

### Q1: 为什么控制通道不使用 TCP？

**A**: 控制通道（路由器 ↔ desd）频繁传输控制消息，UDS 性能更好且更简洁。TCP 会引入不必要的网络协议开销。

### Q2: TCP 连接的三次握手如何处理？

**A**: TCP 三次握手由内核自动完成（`real_connect()` 和 `real_accept()`），但连接建立的虚拟时间由 desd 控制。客户端在 VT=0.049 完成 connect，服务器在 VT=0.050 完成 accept。

### Q3: 能否同时使用 IPv4 和 IPv6？

**A**: 目前仅支持 IPv4（AF_INET）。IPv6（AF_INET6）支持可以在未来添加，实现方式类似。

### Q4: 能否使用真实的公网 IP？

**A**: 理论上可以，但不推荐。DES 主要用于本地模拟，建议使用 `127.0.0.1` 或 `0.0.0.0`。

### Q5: 如何调试 TCP 通信问题？

**A**: 
1. 查看 libdeshook 日志：`[LIBDESHOOK]` 前缀
2. 查看 desd 日志：`[DESD]` 前缀
3. 确认地址格式正确：`IP:Port`
4. 检查路由器是否正确注册监听地址

---

## 总结

TCP 支持的实现遵循以下原则：

1. **最小化改动**：只修改必要的地方（libdeshook.c），desd.c 无需改动
2. **保持兼容**：UDS 功能完全不受影响
3. **统一抽象**：TCP 和 UDS 在 desd 中统一为字符串地址
4. **透明拦截**：应用程序无需修改代码

这使得 DES 项目能够支持真实的路由器镜像（如 BIRD BGP），同时保持架构的简洁性和可维护性。

