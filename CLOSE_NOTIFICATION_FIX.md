# Close()通知机制实现

## 问题背景

### 发现的问题
在实现listening socket POLLIN修复后，发现BIRD在连接建立过程中会出现以下行为：
1. 双方同时发起连接（TCP Simultaneous Open）
2. 每一方都建立了两条连接：一条client连接（connect），一条server连接（accept）
3. BIRD的BGP状态机检测到"Unexpected message"错误后，会关闭所有相关连接
4. **Linux内核会重用刚释放的fd号**（总是分配最小未使用的fd）
5. BIRD立即重新发起连接，复用了相同的fd号

### 核心问题
**desd无法区分新旧socket：**
```
时间线：
VT=64.050: R1 socket(13) → connect() → 连接建立
VT=70.000: R1 close(13)              ← desd不知道！
VT=70.001: R1 socket(13) → connect() ← 新的socket，但fd号相同！
```

**desd连接表污染：**
```
旧的: R1 (fd:13, 已关闭) <-> R2 (peer_fd:14)
新的: R1 (fd:13, 新连接) <-> R2 (peer_fd:??)
```
desd中同一个fd有多条记录，导致数据包路由错误。

### 为什么没有collision detection
分析日志发现，这**不是**BGP的collision detection，而是：
1. 消息被路由到错误的socket（已关闭的旧fd）
2. BGP协议解析失败 → "Unexpected message"
3. BGP状态机崩溃 → 关闭所有连接
4. 不断重试 → 循环往复

## 解决方案

### 实现close()通知机制
当BIRD关闭socket时，libdeshook立即通知desd清理连接表。

## 修改内容

### 1. `src/common.h`
添加新的事件类型：
```c
typedef enum {
    // ... 现有事件类型 ...
    CLOSE_SOCKET_EVENT,       // libdeshook.so -> desd：通知desd socket已关闭
    GET_VIRTUAL_TIME_EVENT
} EventType;
```

### 2. `src/common.c`
在所有字符串转换函数中添加`CLOSE_SOCKET_EVENT`的支持：
- `event_to_json()`
- `json_to_event()`
- `message_to_json()`
- `json_to_message()`

### 3. `src/libdeshook.c`
修改`close()`函数，添加通知逻辑：

**关键特性：**
- ✅ **线程安全**：使用`socket_fds_mutex`和`desd_comm_mutex`保护
- ✅ **同步等待**：等待desd响应，确保DES框架的虚拟时间一致性
- ✅ **仅通知DES-managed socket**：不干扰非DES socket

```c
int close(int sockfd) {
    // 1. 检查是否是DES管理的socket
    if (is_des_socket(sockfd) && desd_control_socket_fd != -1) {
        // 2. 构建CLOSE_SOCKET_EVENT消息
        // 3. 发送给desd并等待响应
        // 4. 收到SUCCESS后继续
    }
    // 5. 清理本地跟踪标记
    // 6. 调用real_close()
}
```

### 4. `src/desd.c`

#### 添加事件处理函数
```c
void handle_close_socket_event(Event event) {
    // 1. 解析socket_fd
    // 2. 遍历连接表，删除所有涉及该fd的记录
    // 3. 同时清理对端router的对应记录
    // 4. 清理pending的数据包缓冲区
}
```

**清理逻辑：**
- 删除本router中`socket_fd == sockfd`的所有连接
- 同时删除对端router中`peer_socket_fd == sockfd`的连接
- 清理该fd的所有pending数据包

#### 添加到事件处理流程
1. 在`process_event()`的switch中添加case
2. 在消息接收循环中立即处理（类似CONNECTION_INFO_EVENT）
3. 添加到`event_type_to_string()`函数

## 设计决策

### 为什么必须等待desd响应？（关键设计原则）

**DES框架的核心要求：所有网络操作必须在desd的控制下同步进行**

1. **保持虚拟时间一致性**
   - close()是一个状态改变操作
   - 必须等待desd完成状态清理
   - 才能让router继续执行后续操作

2. **避免fd复用竞态条件**
   ```
   错误场景（fire-and-forget）：
   T1: Router close(13) → 发送通知 → 立即返回
   T2: Router socket() → 分配fd 13  ← desd还在清理旧的fd 13！
   T3: Desd清理完成 → 但新的fd 13已经创建
   ```
   
   ```
   正确场景（等待响应）：
   T1: Router close(13) → 发送通知 → 阻塞等待
   T2: Desd清理连接表 → 发送SUCCESS
   T3: Router收到SUCCESS → 继续执行
   T4: Router socket() → 分配fd 13 ✓ desd已经清理完成
   ```

3. **保证事件顺序正确性**
   - DES框架要求所有事件按虚拟时间有序处理
   - 如果close()不等待，可能破坏事件的因果关系

### 为什么同时清理对端连接？
- 当一端close时，连接已经物理断开
- 对端继续持有记录会导致：
  - 数据发送到已关闭的socket
  - 连接表无法复用（一直占用槽位）
- 清理对端记录可以：
  - 立即释放连接表空间
  - 避免向dead socket发送数据
  - 让对端也能复用fd

### 为什么立即处理不放入事件队列？
- CLOSE_SOCKET_EVENT是状态清理操作，不是网络事件
- 应该立即生效，避免在队列中等待时产生更多错误
- 类似LISTEN_EVENT和CONNECTION_INFO_EVENT的处理方式

## 预期效果

### 修复后的行为
```
VT=64.050: R1 close(13) → 通知desd → 清理所有fd:13的记录
VT=64.051: R1 socket(13) → connect() → desd注册新连接（干净的状态）
```

### 解决的问题
1. ✅ **连接表泄漏** - 关闭的连接会被立即清理
2. ✅ **fd复用混乱** - 新的fd不会和旧记录冲突
3. ✅ **数据包路由错误** - 数据总是发送到正确的active socket
4. ✅ **连接表满错误** - 及时清理释放空间

## 测试验证

### 编译测试
```bash
cd /home/hwzhuang/hwzhuang/desTest/des_design
make clean && make
```
✅ 编译成功，无错误无警告

### 功能测试
运行BIRD测试，观察日志中：
1. `[LIBDESHOOK] R1 notifying DESD of socket close for fd XX` - libdeshook通知
2. `[DESD] R1 closed socket fd XX, cleaning up connections` - desd接收
3. `[DESD] Removing connection: R1 (fd:XX) <-> R2 (fd:YY)` - 清理记录
4. `[DESD] R1 socket fd XX cleanup complete. Removed N connection(s)` - 完成

### 预期改进
- ❌ 不再出现"connection table full"错误
- ❌ 不再出现"Invalid connection mapping"错误
- ✅ 连接表保持clean状态
- ✅ BGP连接能够正常建立和维持

## 未解决的问题

### 重复CONNECT_REQUEST问题
**当前策略：暂不处理**

发现BIRD在已连接的socket上重复调用`connect()`，导致desd中同一fd有多条client连接记录。这是另一个独立的问题，需要：
- 检测对已连接socket的重复CONNECT_REQUEST
- 拒绝或更新现有连接而非创建新记录

### 为什么不在本次修复？
1. close()通知机制本身已经能缓解问题（清理旧连接）
2. 重复CONNECT_REQUEST需要状态跟踪逻辑（更复杂）
3. 先验证close()修复的效果，再决定是否需要额外处理

## 文件修改清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/common.h` | 新增 | 添加CLOSE_SOCKET_EVENT枚举 |
| `src/common.c` | 修改 | 添加事件类型字符串转换 |
| `src/libdeshook.c` | 修改 | close()函数添加通知逻辑 |
| `src/desd.c` | 新增+修改 | 实现handle_close_socket_event()和事件处理 |

## ⚠️ 重要更新

**本版本的清理逻辑存在问题**：使用了双向立即清理，导致in-flight数据包无法送达。

**已被以下版本替代**：
- `SINGLE_SIDED_CLOSE_FIX.md` - 单向清理修复，正确处理TCP half-close语义

本文档仅作为历史记录保留，实际使用的是单向清理版本。

## 相关文档
- `SINGLE_SIDED_CLOSE_FIX.md` - **[当前版本]** 单向清理修复
- `LISTENING_SOCKET_FIX.md` - listening socket POLLIN事件修复
- `THREAD_SAFETY_CHANGES.md` - libdeshook线程安全修复
