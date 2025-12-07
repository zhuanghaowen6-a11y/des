# 单向Close修复 - 解决In-Flight数据包问题

## 问题背景

### 发现的新问题（在close()通知机制实现后）

运行测试后发现，虽然close()通知机制已实现，但仍然出现严重错误：

```
VT=40.003: R1 close(fd:14) → desd立即清理连接表
           ├─ 删除 R1(fd:14) <-> R2(fd:14)  ✓
           └─ 删除 R2(fd:14) <-> R1(fd:14)  ✗ 错误！

VT=40.103: R2收到数据包（来自已删除的连接）
VT=40.107: R2尝试send(fd:13) 
           └─ [DESD ERROR] Cannot find socket_fd for connection!
```

### 根本原因分析

**双向立即清理违背TCP语义**：

1. ❌ **过早清理对端记录**：R1 close时，desd不仅删除R1的记录，还立即删除R2的记录
2. ❌ **In-flight数据包丢失**：数据包还在队列中（PACKET_RECEIVE_EVENT），但连接记录已被删除
3. ❌ **无法发送响应**：R2收到数据后想回复，但连接记录不存在

**时间线问题**：
```
VT=40.003: R1 send(fd:14) → 调度PACKET_RECEIVE at VT=40.103
VT=40.003: R1 close(14)   → desd删除双方记录  ← 太早了！
VT=40.103: R2收到数据    → 尝试查找连接      ← 已被删除！
```

### TCP的正确语义

TCP close()是**单向操作**：
- 一方close不应影响另一方的socket状态
- 接收方应该能继续接收pending数据
- 接收方应该能检测到对端关闭（recv返回0）
- 只有双方都close后才完全清理

## 解决方案

### 单向清理机制

**核心思想**：close()只清理本侧记录，标记对端但保留其记录

```
R1 close(fd:13)时：
├─ 删除 R1(fd:13) <-> R2(fd:13)      ✓ 清理R1侧记录
├─ 标记 R2(fd:13).peer_closed = 1    ✓ 告知R2对端已关闭
├─ 保留 R2(fd:13)记录                ✓ R2可以继续处理数据
└─ R2 close(fd:13)时才完全清理       ✓ 双向清理完成
```

## 实现细节

### 1. 数据结构修改

#### **ConnectionInfo添加状态标记**
```c
typedef struct {
    int socket_fd;
    int peer_router_id;
    int peer_socket_fd;
    int is_active;
    int peer_closed;  // ← 新增：标记对端是否已关闭
} ConnectionInfo;
```

### 2. 单向清理逻辑

#### **handle_close_socket_event()修改**
```c
// 修改前：双向立即清理
router_states[router_id].connections[i].is_active = 0;  // 删除本地
router_states[peer_id].connections[j].is_active = 0;    // 立即删除对端 ✗

// 修改后：单向清理
router_states[router_id].connections[i].is_active = 0;  // 删除本地
router_states[peer_id].connections[j].peer_closed = 1;  // 标记对端 ✓
// 不删除对端记录，让对端继续处理pending数据
```

**关键特性**：
- ✅ 只删除本router的连接记录
- ✅ 标记对端连接的`peer_closed = 1`
- ✅ 保留对端记录，允许继续处理数据
- ✅ 当对端也close时才完全清理

### 3. 数据发送检查

#### **handle_packet_send_event()添加检查**
```c
// 检查对端是否已关闭
int peer_has_closed = 0;
for (int i = 0; i < MAX_CONNECTIONS_PER_ROUTER; i++) {
    if (router_states[source_router_id].connections[i].is_active &&
        router_states[source_router_id].connections[i].socket_fd == socket_fd) {
        peer_has_closed = router_states[source_router_id].connections[i].peer_closed;
        break;
    }
}

if (peer_has_closed) {
    // 对端已关闭，拒绝发送新数据
    send_error_response(source_router_id, request_id, "Peer connection closed");
    return;
}
```

**目的**：防止在对端已关闭后继续发送数据

### 4. EOF检测机制

#### **handle_router_block_request() RECV_CALL处理**
```c
// 当recv没有pending数据时
if (peer_has_closed) {
    // 返回EOF（0字节），符合TCP语义
    json_object_set_new(resp_payload, "status", json_string("EOF"));
    json_object_set_new(resp_payload, "packet_data", json_string(""));
    send_message_to_router(router_id, &response);
} else if (is_nonblocking) {
    // 返回EAGAIN
} else {
    // 阻塞等待数据
}
```

#### **libdeshook recv()处理EOF**
```c
else if (strcmp(status, "EOF") == 0) {
    printf("[LIBDESHOOK] R%d recv() received EOF - peer closed connection.\n", my_router_id);
    return 0;  // EOF: connection closed by peer
}
```

**作用**：正确模拟TCP的FIN行为，让应用层检测到连接关闭

## 修复流程对比

### 修复前（错误）：
```
VT=40.003:
R1: send(fd:14) → 调度PACKET_RECEIVE at VT=40.103
R1: close(14)   → desd删除R1和R2的记录  ← 立即删除！

VT=40.103:
R2: 收到数据包
R2: 处理数据，准备回复
R2: send()      → 查找连接记录        ← 已被删除！
                → [ERROR] Cannot find socket_fd
```

### 修复后（正确）：
```
VT=40.003:
R1: send(fd:14) → 调度PACKET_RECEIVE at VT=40.103
R1: close(14)   → desd删除R1记录
                → 标记R2.peer_closed=1    ← 保留R2记录！

VT=40.103:
R2: 收到数据包  → 连接记录存在          ✓ 可以路由
R2: 处理数据
R2: send()      → 检查peer_closed=1
                → [ERROR] Peer closed     ✓ 拒绝发送（正确行为）
或：
R2: recv()      → 检查peer_closed=1, 无数据
                → 返回EOF (0字节)          ✓ 正确的TCP行为
R2: close(14)   → desd删除R2记录        ✓ 双向清理完成
```

## 行为变化总结

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| **R1 close后R2收数据** | 连接记录不存在 → 错误 | 连接记录存在 → 成功接收 ✓ |
| **R1 close后R2发数据** | 连接记录不存在 → 错误 | peer_closed=1 → 拒绝发送 ✓ |
| **R1 close后R2 recv** | 连接记录不存在 → 错误 | peer_closed=1 → 返回EOF ✓ |
| **双方都close后** | 记录已删除 | 双向清理完成 ✓ |

## 文件修改清单

| 文件 | 修改内容 | 行数 |
|------|----------|------|
| `src/desd.c` | ConnectionInfo添加peer_closed字段 | +1 |
| `src/desd.c` | register_connection初始化peer_closed=0 | +1 |
| `src/desd.c` | handle_close_socket_event单向清理 | 修改~40行 |
| `src/desd.c` | handle_packet_send_event检查peer_closed | +15 |
| `src/desd.c` | handle_router_block_request RECV处理EOF | +35 |
| `src/libdeshook.c` | recv()处理EOF响应 | +5 |

**总计**：约100行修改/新增

## 测试验证

### 编译测试
```bash
cd /home/hwzhuang/hwzhuang/desTest/des_design
make clean && make
```
✅ 编译成功，无错误无警告

### 功能测试
运行BIRD测试，观察日志：

**期望看到**：
1. `[DESD] R1 closed socket fd XX, performing single-sided cleanup`
2. `[DESD] Marked peer connection as closed: R2 (fd:XX) peer_closed=1`
3. `[DESD] R2 recv() returning EOF - peer has closed connection`
4. 不再出现 `Cannot find socket_fd for connection` 错误

**预期改进**：
- ✅ In-flight数据包能够正确送达
- ✅ 接收方能正常处理pending数据
- ✅ 接收方能检测到连接关闭（recv返回0）
- ✅ BGP状态机不再因"Unexpected message"崩溃

## 技术要点

### 为什么需要单向清理？

**TCP half-close语义**：
- TCP允许一端关闭发送而继续接收（shutdown SHUT_WR）
- close()会关闭双向，但OS会保留接收能力直到对端FIN
- 我们的实现需要模拟这个行为

**DES时序特性**：
- 事件在队列中有延迟（传输延迟）
- close()执行时，数据包可能还在途中
- 必须保留接收能力处理这些in-flight包

### peer_closed的作用

**三个关键检查点**：
1. **发送数据时**：检查peer_closed，拒绝向已关闭的连接发送
2. **接收数据时**：允许接收（pending数据可能还在）
3. **recv无数据时**：检查peer_closed，返回EOF而不是阻塞

## 与之前修复的关系

### CLOSE_NOTIFICATION_FIX.md
- 实现了close()通知机制
- 但使用了错误的双向立即清理
- ❌ 导致in-flight数据包无法送达

### SINGLE_SIDED_CLOSE_FIX.md（本修复）
- 修正了清理逻辑为单向清理
- 保留对端记录直到双方都close
- ✅ 正确处理in-flight数据包和EOF

**本修复是close()通知机制的正确实现方式**

## 相关文档
- `CLOSE_NOTIFICATION_FIX.md` - close()通知机制（双向清理版本）
- `LISTENING_SOCKET_FIX.md` - listening socket POLLIN事件修复
- `THREAD_SAFETY_CHANGES.md` - libdeshook线程安全修复
