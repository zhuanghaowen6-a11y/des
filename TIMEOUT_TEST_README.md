# 超时和事件取消机制测试指南

## 📋 功能说明

本测试用于验证 DES (Discrete Event Simulation) 系统中的**事件取消机制**，特别是**带超时的I/O操作**。

### 核心机制

1. **超时事件注册**：路由器调用 `select()` 带超时参数时，desd 注册一个 `TIMEOUT_EVENT`
2. **事件取消**：如果在超时前数据到达（`PACKET_RECEIVE_EVENT`），取消超时事件
3. **超时触发**：如果超时事件先到达，向路由器返回超时响应

### 关键数据结构

```c
// RouterInfo 中添加的字段
unsigned long pending_timeout_event_id;  // 记录等待的超时事件ID

// 事件状态数组
int event_active_status[MAX_ACTIVE_EVENTS];  // 0: inactive, 1: active
```

## 🧪 测试场景

### 场景 1: 数据在超时前到达 ✅
- R2 设置 5 秒超时等待数据
- R1 在 1 秒后发送数据
- **预期结果**：
  - R2 在数据到达时被唤醒
  - 超时事件被取消
  - `select()` 正常返回就绪的FD

### 场景 2: 超时发生 ⏰
- R2 设置 5 秒超时等待数据
- R1 在 7 秒后才发送数据
- **预期结果**：
  - 5秒后超时事件触发
  - R2 的 `select()` 返回 0（超时）
  - 7秒后的延迟消息可以被后续 `recv()` 接收

### 场景 3: 正常通信（无超时）
- 验证超时机制不影响正常通信

## 🚀 使用方法

### 编译测试程序

```bash
cd /home/hwzhuang/hwzhuang/desTest/des_design
gcc -Wall -g r1_timeout_test.c -o r1_timeout_test
gcc -Wall -g r2_timeout_test.c -o r2_timeout_test
```

### 运行测试

**终端 1 - 启动 desd:**
```bash
sudo ./desd
```

**终端 2 - 启动 R2 (服务器):**
```bash
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r2_timeout_test
```

**终端 3 - 启动 R1 (客户端):**
```bash
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r1_timeout_test
```

### 观察输出

#### DESD 输出关键信息：
```
[DESD] R2 blocked on SELECT_CALL (ReqID: req_X) with 5000ms timeout.
[DESD] Registered TIMEOUT_EVENT (ID: Y) at VT=5.000.
[DESD] R2 received PACKET_RECEIVE_EVENT.
[DESD] Canceled timeout event Y for R2.
[DESD] R2 was blocked on select and now awakened by PACKET_RECEIVE_EVENT.
```

或超时情况：
```
[DESD] Processing event TIMEOUT_EVENT for R2 at VT=5.000.
[DESD] R2 timed out for request req_X (Type: SELECT_CALL).
```

#### R2 输出关键信息：
```
[R2 TEST] Waiting for data with 5 seconds timeout...
[R2 TEST] ✓ Data available before timeout, receiving...
[R2 TEST] ✓ Received before timeout: Message before timeout
```

或超时情况：
```
[R2 TEST] ⏰ TIMEOUT! No data received within 5 seconds.
```

## 📊 验证要点

### 1. 超时事件正确注册
- [ ] desd 收到 SELECT_CALL 请求
- [ ] 创建并调度 TIMEOUT_EVENT到正确的虚拟时间
- [ ] `pending_timeout_event_id` 被记录

### 2. 事件取消机制
- [ ] PACKET_RECEIVE_EVENT 到达时调用 `cancel_event()`
- [ ] `event_active_status[event_id]` 设置为 0
- [ ] TIMEOUT_EVENT 被跳过（"Skipped cancelled or inactive event"）

### 3. 超时触发
- [ ] 超时事件在正确的虚拟时间触发
- [ ] 路由器收到 TIMEOUT 响应
- [ ] `select()` 返回 0

### 4. 虚拟时间推进
- [ ] 等待5秒的虚拟时间正确推进
- [ ] 虚拟时间与实际时间解耦

## 🔧 实现细节

###  handle_router_block_request() 对 SELECT_CALL 的处理

```c
void handle_router_block_request(Event event) {
    // ...
    if (strcmp(blocked_function, "SELECT_CALL") == 0) {
        int timeout_ms = json_integer_value(json_object_get(payload_obj, "timeout_ms"));
        
        if (timeout_ms > 0) {
            // 注册超时事件
            double timeout_time = current_virtual_time + (timeout_ms / 1000.0);
            Event timeout_event = { ... };
            push_event(timeout_event);
            
            // 记录超时事件ID，以便取消
            router_states[router_id].pending_timeout_event_id = timeout_event.event_id;
        }
        
        // 阻塞路由器
        router_states[router_id].status = BLOCKED;
        // ...
    }
}
```

### handle_packet_receive_event() 中的事件取消

```c
void handle_packet_receive_event(Event event) {
    // ...
    if (router_states[target_router_id].status == BLOCKED &&
        strcmp(router_states[target_router_id].blocked_on_function, "SELECT_CALL") == 0) {
        
        // 取消超时事件
        if (router_states[target_router_id].pending_timeout_event_id > 0) {
            cancel_event(router_states[target_router_id].pending_timeout_event_id);
            router_states[target_router_id].pending_timeout_event_id = 0;
        }
        
        // 唤醒路由器
        router_states[target_router_id].status = RUNNING;
        send_success_response(...);
    }
}
```

## 🎯 预期行为

| 场景 | R1发送时间 | R2超时设置 | 预期结果 |
|------|-----------|-----------|---------|
| 1    | 1秒       | 5秒       | 正常接收 |
| 2    | 7秒       | 5秒       | 超时返回0 |
| 3    | 立即      | 5秒       | 正常接收 |

## ⚠️ 注意事项

1. **事件ID范围**：已将 `MAX_ACTIVE_EVENTS` 扩大到 100000，足够长时间运行
2. **虚拟时间**：超时基于虚拟时间，不是实际时间
3. **连接状态**：确保 R1 和 R2 已经建立连接后再测试

## 🐛 调试技巧

### 启用详细日志
在 desd.c 中取消注释调试输出：
```c
printf("[DESD DEBUG] Checking for timeout: pending_id=%lu\n", 
       router_states[router_id].pending_timeout_event_id);
```

### 检查事件状态
```c
printf("[DESD DEBUG] Event %lu status: %d\n", 
       event_id, event_active_status[event_id]);
```

## 📚 相关代码文件

- `libdeshook.c`: `select()` 函数拦截 (585-669行)
- `desd.c`: 
  - `handle_router_block_request()` - SELECT_CALL 处理
  - `handle_packet_receive_event()` - 事件取消逻辑
  - `handle_timeout_event()` - 超时事件处理
  - `cancel_event()` - 事件取消函数 (366-373行)
- `r1_timeout_test.c`: 测试客户端
- `r2_timeout_test.c`: 测试服务器（带select超时）

