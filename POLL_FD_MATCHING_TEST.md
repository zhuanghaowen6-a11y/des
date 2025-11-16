# poll() 精确 FD 匹配测试

## 测试目标

验证 **方案B** 的核心功能：`poll()` 能够**精确识别哪些 FD 有数据**，而不是简单地标记所有监听的 FD。

---

## 测试原理

### 场景设计

```
服务端 (R1):                    客户端 (R2):
┌─────────────────┐            ┌─────────────────┐
│ listen socket1  │◄───────────┤ connect socket1 │  ✓ 发送数据
│ listen socket2  │◄───────────┤ connect socket2 │  ✗ 不发送数据
│ listen socket3  │◄───────────┤ connect socket3 │  ✓ 发送数据
└─────────────────┘            └─────────────────┘
        ↓
  poll([fd1, fd2, fd3], 3, 5000)
        ↓
  期望结果（方案B）:
  - fds[0].revents = POLLIN (fd1 有数据)
  - fds[1].revents = 0      (fd2 无数据) ← 关键验证点
  - fds[2].revents = POLLIN (fd3 有数据)
  - 返回值 = 2
```

### 方案对比

| 行为 | 方案A（旧实现） | 方案B（新实现） |
|------|----------------|----------------|
| **FD1 (有数据)** | POLLIN | POLLIN |
| **FD2 (无数据)** | POLLIN ❌ | 0 ✅ |
| **FD3 (有数据)** | POLLIN | POLLIN |
| **返回值** | 3 | 2 |

---

## 测试程序

### 1. r_poll_server_test.c

**功能：**
- 监听 3 个不同的 Unix domain socket 地址
- 接受 3 个连接
- 使用 `poll()` 监听 3 个连接 FD
- 验证 `poll()` 返回结果的准确性

**关键代码：**
```c
struct pollfd fds[3];
fds[0].fd = conn_fd1;  // 连接 1
fds[1].fd = conn_fd2;  // 连接 2
fds[2].fd = conn_fd3;  // 连接 3
fds[0].events = POLLIN;
fds[1].events = POLLIN;
fds[2].events = POLLIN;

int ret = poll(fds, 3, 5000);  // 5 秒超时

// 验证每个 FD 的 revents
if (fds[0].revents & POLLIN) { /* FD1 应该就绪 */ }
if (fds[1].revents & POLLIN) { /* FD2 不应该就绪！ */ }
if (fds[2].revents & POLLIN) { /* FD3 应该就绪 */ }
```

**验证点：**
1. FD1 应该被标记为 POLLIN
2. FD2 不应该被标记（revents=0）← **核心验证**
3. FD3 应该被标记为 POLLIN
4. poll() 返回值应该是 2（不是 3）

### 2. r_poll_client_test.c

**功能：**
- 连接到服务端的 3 个地址
- 只向连接 1 和 3 发送数据
- **故意不向连接 2 发送数据**（测试关键）

**关键代码：**
```c
// 连接到 3 个地址
connect(sockfd1, SOCKET_PATH_1, ...);
connect(sockfd2, SOCKET_PATH_2, ...);
connect(sockfd3, SOCKET_PATH_3, ...);

// 选择性发送数据
send(sockfd1, "Data for connection 1", ...);  // ✓ 发送
// sockfd2: 不发送！                            // ✗ 跳过
send(sockfd3, "Data for connection 3", ...);  // ✓ 发送
```

---

## 编译方法

```bash
# 编译服务端
gcc -o r_poll_server_test r_poll_server_test.c

# 编译客户端
gcc -o r_poll_client_test r_poll_client_test.c
```

---

## 运行方法

### 使用 DES 环境测试（推荐）

**终端 1: 启动 desd**
```bash
sudo ./desd
```

**终端 2: 启动服务端（R1）**
```bash
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r_poll_server_test
```

**终端 3: 启动客户端（R2）**
```bash
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r_poll_client_test
```

### 不使用 DES 环境测试（验证程序逻辑）

**终端 1: 启动服务端**
```bash
./r_poll_server_test
```

**终端 2: 启动客户端**
```bash
./r_poll_client_test
```

---

## 预期输出

### 服务端输出（成功情况）

```
========================================
poll() 精确 FD 匹配测试 - 服务端
========================================

[SERVER] Listening on /tmp/poll_test_socket_1 (fd=4)
[SERVER] Listening on /tmp/poll_test_socket_2 (fd=5)
[SERVER] Listening on /tmp/poll_test_socket_3 (fd=6)

[SERVER] Waiting for 3 client connections...
[SERVER] ✓ Accepted connection 1 (fd=7)
[SERVER] ✓ Accepted connection 2 (fd=8)
[SERVER] ✓ Accepted connection 3 (fd=9)

========================================
测试开始：使用 poll() 监听 3 个 FD
========================================

[SERVER] 配置 poll():
  fds[0].fd = 7 (连接 1)
  fds[1].fd = 8 (连接 2)
  fds[2].fd = 9 (连接 3)

[SERVER] 等待 5 秒...
[SERVER] 提示：客户端应该只向连接 1 和 3 发送数据，不向连接 2 发送

========================================
poll() 返回结果
========================================

[SERVER] poll() 返回: 2 个 FD 就绪

[SERVER] 检查各个 FD 的 revents:
  fds[0] (fd=7): revents=0x1 ✓ POLLIN (可读)
  fds[1] (fd=8): revents=0x0 ✓ 无事件 (正确)
  fds[2] (fd=9): revents=0x1 ✓ POLLIN (可读)

[SERVER] 实际就绪的 FD 数量: 2

========================================
测试结果验证
========================================

[SERVER] ✓ 正确：FD1 被标记为可读
[SERVER] ✓ 正确：FD2 未被标记（精确匹配生效）
[SERVER] ✓ 正确：FD3 被标记为可读

[SERVER] 尝试从各个 FD 读取数据:
  FD1: 收到 21 字节: "Data for connection 1"
  FD3: 收到 21 字节: "Data for connection 3"

========================================
✓✓✓ 测试通过！poll() 精确 FD 匹配工作正常！
========================================
```

### 客户端输出

```
========================================
poll() 精确 FD 匹配测试 - 客户端
========================================

[CLIENT] 创建了 3 个 socket: fd=4, 5, 6

[CLIENT] 连接到 /tmp/poll_test_socket_1...
[CLIENT] ✓ 连接 1 成功 (fd=4)
[CLIENT] 连接到 /tmp/poll_test_socket_2...
[CLIENT] ✓ 连接 2 成功 (fd=5)
[CLIENT] 连接到 /tmp/poll_test_socket_3...
[CLIENT] ✓ 连接 3 成功 (fd=6)

========================================
测试场景：选择性发送数据
========================================

[CLIENT] 向连接 1 (fd=4) 发送数据: "Data for connection 1"
[CLIENT] ✓ 连接 1 发送成功

[CLIENT] 🚫 不向连接 2 (fd=5) 发送数据（故意跳过）
[CLIENT]    这是测试的关键：验证 poll() 不会标记 FD2

[CLIENT] 向连接 3 (fd=6) 发送数据: "Data for connection 3"
[CLIENT] ✓ 连接 3 发送成功

========================================
发送完成
========================================

[CLIENT] 数据发送总结:
  连接 1: ✓ 已发送数据
  连接 2: ✗ 未发送数据 (测试重点)
  连接 3: ✓ 已发送数据

[CLIENT] 期望服务端 poll() 结果:
  FD1: 应该被标记为可读 (POLLIN)
  FD2: 不应该被标记 (revents=0)
  FD3: 应该被标记为可读 (POLLIN)
  返回值: 2 (只有 2 个 FD 就绪)

[CLIENT] 等待 3 秒后退出...
[CLIENT] 测试完成。
```

### desd 输出（关键部分）

```
[DESD] R1 select() immediately unblocked (had 2 pending packet(s), 2 unique FDs).
```

**关键验证：** `2 unique FDs` 而不是 3 个！

---

## 失败情况（方案A行为）

如果使用方案A（旧实现），输出会是：

```
========================================
poll() 返回结果
========================================

[SERVER] poll() 返回: 3 个 FD 就绪  ← 错误！应该是 2

[SERVER] 检查各个 FD 的 revents:
  fds[0] (fd=7): revents=0x1 ✓ POLLIN (可读)
  fds[1] (fd=8): revents=0x1 ✗ POLLIN (不应该有数据！)  ← 错误！
  fds[2] (fd=9): revents=0x1 ✓ POLLIN (可读)

========================================
测试结果验证
========================================

[SERVER] ✓ 正确：FD1 被标记为可读
[SERVER] ✗ 错误：FD2 不应该有数据但被标记为可读
[SERVER]   这说明 poll() 无法精确区分 FD（方案A行为）
[SERVER] ✓ 正确：FD3 被标记为可读

========================================
✗✗✗ 测试失败！poll() 无法精确区分 FD。
========================================
```

---

## 测试要点

### 1. 核心验证

**最关键的验证点：**
```c
if (fds[1].revents & POLLIN) {
    // ✗ 如果进入这里，说明方案B失败
    printf("FD2 不应该有数据但被标记\n");
} else {
    // ✓ 如果进入这里，说明方案B成功
    printf("FD2 未被标记（精确匹配生效）\n");
}
```

### 2. 返回值验证

```c
int ret = poll(fds, 3, 5000);
// 方案A: ret = 3 (错误)
// 方案B: ret = 2 (正确)
```

### 3. 数据验证

只有 FD1 和 FD3 应该能读取到数据。

---

## 故障排查

### 问题 1: 所有 FD 都被标记

**症状：**
```
fds[0].revents = POLLIN
fds[1].revents = POLLIN  ← 不应该
fds[2].revents = POLLIN
返回值 = 3
```

**原因：** 方案B未正确实现，或回退到方案A逻辑

**排查：**
1. 检查 `desd` 日志中的 FD 数量
2. 检查 `PacketBuffer.socket_fd` 是否正确记录
3. 检查 `ready_fds` 数组是否正确构建

### 问题 2: poll() 超时

**症状：**
```
poll() 返回: 0
超时，没有任何 FD 就绪
```

**原因：** 数据未到达或 desd 未正确处理

**排查：**
1. 检查客户端是否成功发送数据
2. 检查 desd 日志中的 `PACKET_SEND_EVENT` 和 `PACKET_RECEIVE_EVENT`
3. 检查 `pending_packets_count`

### 问题 3: FD2 也收到数据

**症状：**
```
FD2: 收到 21 字节: "Data for connection 3"
```

**原因：** `socket_fd` 映射错误

**排查：**
1. 检查 `find_socket_fd_for_peer()` 返回值
2. 检查连接表是否正确建立

---

## 性能基准

### 预期性能

| 指标 | 值 |
|------|-----|
| **FD 查找时间** | < 1 μs |
| **ready_fds 构建时间** | < 10 μs (3 个 packet) |
| **poll() 调用时间** | < 100 μs (DES 虚拟时间) |
| **内存开销** | +12 字节 (3 × socket_fd) |

---

## 总结

### 测试覆盖

- ✅ 多 FD 监听
- ✅ 部分 FD 有数据
- ✅ 精确 revents 设置
- ✅ 正确的返回值
- ✅ 数据完整性

### 成功标准

1. FD2 的 `revents` 必须是 0
2. poll() 返回值必须是 2
3. 只能从 FD1 和 FD3 读取到数据
4. desd 日志显示 `2 unique FDs`

### 验证方案B生效

如果以上 4 个条件都满足，则证明：
✅ **方案B（精确 FD 匹配）工作正常！**

---

## 扩展测试

可以进一步测试：

1. **更多 FD**: 测试 5-10 个 FD
2. **不同组合**: 只有 FD1 有数据，或只有 FD2 和 FD3 有数据
3. **超时测试**: 没有任何 FD 有数据，验证超时
4. **并发测试**: 多个客户端同时发送

---

测试准备就绪！🚀

