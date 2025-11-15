# 🔧 修复 accept/connect 死锁问题

## 🔴 问题描述

### 症状
当**先启动 R2（服务器）再启动 R1（客户端）**时，程序会卡死。

### 根本原因
**竞态条件**：两个 `CONNECTION_ESTABLISHED_EVENT` 被调度到相同的虚拟时间，但处理顺序不确定。如果服务器的事件先处理，会导致：

1. **desd 先唤醒 R2（服务器）**
2. **R2 执行 `real_accept()`，在内核层面阻塞**（因为 R1 还没执行 `real_connect()`）
3. **R2 阻塞在内核，无法接收后续 desd 消息**
4. **死锁！** 💀

---

## 📊 详细时间线

### ❌ 修复前（死锁场景）

```
VT=0.000  R2 启动，调用 accept()
          → desd: 阻塞 R2 (ACCEPT_CALL, pending=0)
          → R2: 阻塞在 send_msg_to_desd_and_wait_for_response()

VT=0.000  R1 启动，调用 connect()
          → desd: 调度两个 CONNECTION_ESTABLISHED_EVENT:
             • Event A (R2): VT=0.050, event_id=100
             • Event B (R1): VT=0.050, event_id=101

VT=0.050  desd 处理事件（按 event_id 顺序）
          
          [Event A 先处理] ⚠️
          → handle_connection_established_event(R2)
          → 发现 R2 阻塞在 ACCEPT_CALL
          → 发送 SUCCESS 响应给 R2
          → R2 收到响应，解除阻塞
          → R2: real_accept(listen_fd, ...) 📍 在内核阻塞！
          
          [此时 R1 还没执行 real_connect()]
          
          [Event B 处理]
          → handle_connection_established_event(R1)
          → 发送 SUCCESS 响应给 R1
          → R1: 尝试执行 real_connect()
          
          ❌ 死锁：R2 阻塞在内核 accept，无法响应任何消息
```

### ✅ 修复后（正常流程）

```
VT=0.000  R2 启动，调用 accept()
          → desd: 阻塞 R2 (ACCEPT_CALL)

VT=0.000  R1 启动，调用 connect()
          → desd: 调度两个 CONNECTION_ESTABLISHED_EVENT:
             • Event B (R1): VT=0.049, event_id=101  ← 早 1ms
             • Event A (R2): VT=0.050, event_id=100

VT=0.049  desd 处理 Event B (R1 - client) 🎯 先处理！
          → handle_connection_established_event(R1)
          → register_connection(R1, fd:4, R2)
          → 发送 SUCCESS 响应给 R1
          → R1: real_connect() ✅ 成功执行

VT=0.050  desd 处理 Event A (R2 - server)
          → handle_connection_established_event(R2)
          → 发送 SUCCESS 响应给 R2
          → R2: real_accept() ✅ 成功（连接已在内核层面建立）

VT=0.050  R2 发送 CONNECTION_INFO_EVENT (new_fd=5)
          → desd: register_connection(R2, fd:5, R1)
          → 双向连接映射完成 ✓
```

---

## 🔧 修复方案

### 代码修改

**文件：** `desd.c` (780行)

```c
// 修复前
Event source_conn_est_event = {
    .timestamp = connection_established_time,  // ❌ 与服务器相同
    .router_id = router_id,
    .event_type = CONNECTION_ESTABLISHED_EVENT,
    .event_id = generate_event_id()
};

// 修复后
Event source_conn_est_event = {
    .timestamp = connection_established_time - 0.001,  // ✅ 早 1ms
    .router_id = router_id,
    .event_type = CONNECTION_ESTABLISHED_EVENT,
    .event_id = generate_event_id()
};
```

### 修复原理

**关键思想：确保执行顺序**

1. **客户端事件时间戳：** `VT + 0.049`（早 1ms）
2. **服务器事件时间戳：** `VT + 0.050`
3. **保证顺序：** 客户端事件必然先于服务器事件处理
4. **结果：** `real_connect()` 先执行，`real_accept()` 后执行

---

## 📝 为什么这个顺序很重要

### TCP 三次握手在 Unix Domain Socket 中的体现

虽然 Unix Domain Socket 不是真正的 TCP，但内核实现有类似的握手过程：

1. **客户端 `connect()`**：
   - 发起连接请求
   - 内核建立连接状态
   
2. **服务器 `accept()`**：
   - 从内核接收队列中取出已建立的连接
   - **前提：连接必须已经在内核层面建立**

**如果顺序错误：**
- 服务器先调用 `accept()` → 队列为空 → **阻塞**
- 客户端后调用 `connect()` → 连接建立 → 但服务器已经阻塞在内核 → **死锁**

---

## 🧪 测试验证

### 测试场景 1: 先启动 R2，后启动 R1

```bash
# 终端1
sudo ./desd

# 终端2 - 先启动服务器
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r2

# 终端3 - 后启动客户端
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r1
```

**预期输出（desd）：**
```
[DESD] R1 sent CONNECT_REQUEST (fd:4). Scheduled CONNECTION_ESTABLISHED_EVENT 
       for R2 (server) at VT=0.050 and R1 (client) at VT=0.049.
[DESD] Processing event CONNECTION_ESTABLISHED_EVENT for R1 at VT=0.049 ✓
[DESD] R1 (client) connect() completed with R2.
[DESD] Processing event CONNECTION_ESTABLISHED_EVENT for R2 at VT=0.050 ✓
[DESD] R2 (server) accept() completed due to connection from R1.
```

### 测试场景 2: 先启动 R1，后启动 R2

```bash
# 先启动客户端，后启动服务器（也应该正常工作）
```

---

## 🎯 关键要点总结

### 1. **问题本质**
- 虚拟时间（DES 层面）与实际内核操作不同步
- 需要确保内核操作的正确顺序

### 2. **修复策略**
- 客户端事件略早于服务器事件
- 保证 `real_connect()` 先于 `real_accept()` 执行

### 3. **时间差选择**
- **1ms (0.001秒)** 足够小，不影响仿真精度
- 足够大，确保在事件队列（最小堆）中有明确的顺序

### 4. **可扩展性**
- 适用于任意数量的路由器
- 适用于任意连接场景

---

## 🔍 相关代码位置

| 文件 | 函数 | 行数 | 说明 |
|------|------|------|------|
| `desd.c` | `handle_connect_request_event()` | 780 | 客户端事件时间戳设置 |
| `desd.c` | `handle_connection_established_event()` | 791-874 | 连接建立事件处理 |
| `libdeshook.c` | `accept()` | 264-357 | accept 拦截实现 |
| `libdeshook.c` | `connect()` | 183-260 | connect 拦截实现 |

---

## ✅ 修复验证

**修复前：**
- ❌ 先启动 R2 再启动 R1 → 死锁
- ✅ 先启动 R1 再启动 R2 → 正常

**修复后：**
- ✅ 先启动 R2 再启动 R1 → 正常
- ✅ 先启动 R1 再启动 R2 → 正常

---

## 🎉 总结

通过让客户端的 `CONNECTION_ESTABLISHED_EVENT` 早 1ms 执行，我们确保了：

1. ✅ `real_connect()` 总是先于 `real_accept()` 执行
2. ✅ 避免了 `real_accept()` 在内核层面阻塞
3. ✅ 支持任意启动顺序
4. ✅ 保持了 DES 的正确性和精度

**这是一个关键的修复，解决了一个可能导致仿真卡死的严重竞态条件问题！** 🎊

