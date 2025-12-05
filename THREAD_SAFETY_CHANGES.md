# libdeshook 线程安全修改总结

## 📋 问题背景

**BIRD 3引入了多线程架构**，但DES的设计假设每个路由器是单线程的。这导致了以下问题：

1. **响应混乱** - 多个线程同时与desd通信，线程A收到线程B的响应
2. **request_id重复** - 非原子递增导致ID冲突
3. **数据竞争** - 多个线程同时读写全局变量
4. **时钟错误** - 收到错误的虚拟时间数据导致崩溃

## 🔧 修改内容

### 1. 添加pthread支持

**文件**: `src/libdeshook.c` 第21行

```c
#include <pthread.h> // For thread safety
```

### 2. 添加三个互斥锁

**文件**: `src/libdeshook.c` 第32-39行

```c
// --- Thread Safety ---
// 全局互斥锁：确保同一时刻只有一个线程与desd通信
// 这是关键：BIRD 3使用多线程，但DES假设每个路由器是单线程的
static pthread_mutex_t desd_comm_mutex = PTHREAD_MUTEX_INITIALIZER;
// 保护request_counter的原子性
static pthread_mutex_t request_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
// 保护socket_fds数组的并发访问
static pthread_mutex_t socket_fds_mutex = PTHREAD_MUTEX_INITIALIZER;
```

### 3. 修改 generate_request_id() 函数

**文件**: `src/libdeshook.c` 第166-173行

**修改前**:
```c
void generate_request_id(char* id_buf) {
    snprintf(id_buf, 64, "req_%lu_%d", next_request_id_val++, my_router_id);
}
```

**修改后**:
```c
void generate_request_id(char* id_buf) {
    // 🔒 线程安全：保护request_counter的原子递增
    pthread_mutex_lock(&request_counter_mutex);
    unsigned long current_id = next_request_id_val++;
    pthread_mutex_unlock(&request_counter_mutex);
    
    snprintf(id_buf, 64, "req_%lu_%d", current_id, my_router_id);
}
```

**效果**: 确保request_id生成是原子操作，避免ID重复。

### 4. 修改 send_msg_to_desd_and_wait_for_response() 函数

**文件**: `src/libdeshook.c` 第177-251行

**核心修改**: 在整个发送-接收过程中持有互斥锁

```c
int send_msg_to_desd_and_wait_for_response(const Message* req_msg, Message* resp_msg_out) {
    // 🔒 关键线程安全机制：确保同一时刻只有一个线程与desd通信
    // 这实现了DES的单线程假设：每个路由器在同一时刻只有一个活跃的执行流
    pthread_mutex_lock(&desd_comm_mutex);
    
    // ... 发送请求 ...
    // ... 等待响应 ...
    
    // 🔓 成功完成通信，释放互斥锁
    pthread_mutex_unlock(&desd_comm_mutex);
    return 1;
}
```

**效果**: 
- 保证发送和接收是原子的
- 避免响应混乱（线程A收到线程B的响应）
- 实现了DES的单线程假设

### 5. 修改 socket() 函数

**文件**: `src/libdeshook.c` 第1323-1326行

```c
// 🔒 线程安全：标记AF_INET/AF_INET6的socket（需要DES管理）
if (fd >= 0 && fd < MAX_TRACKED_FDS && (domain == AF_INET || domain == AF_INET6)) {
    pthread_mutex_lock(&socket_fds_mutex);
    socket_fds[fd] = 1;
    pthread_mutex_unlock(&socket_fds_mutex);
    printf("[LIBDESHOOK] R%d marked fd %d as DES-managed socket.\n", my_router_id, fd);
}
```

**效果**: 保护socket_fds数组的写操作。

### 6. 修改 poll_internal() 函数

**文件**: `src/libdeshook.c` 第745-750行

```c
// 🔒 线程安全：保护socket_fds数组的读取，创建一个本地副本
// 避免在整个poll过程中持有锁，因为poll可能会阻塞很长时间
int local_socket_fds[MAX_TRACKED_FDS];
pthread_mutex_lock(&socket_fds_mutex);
memcpy(local_socket_fds, socket_fds, sizeof(socket_fds));
pthread_mutex_unlock(&socket_fds_mutex);
```

**效果**: 
- 创建本地副本，避免长时间持有锁
- 保护socket_fds数组的读操作
- 后续所有对socket_fds的引用都改为local_socket_fds

### 7. 更新 Makefile

**文件**: `Makefile` 第57行

**修改前**:
```makefile
$(CC) $(CFLAGS) -shared -fPIC $(LIBDESHOOK_SRC) $(COMMON_SRC) -o $(LIBDESHOOK) $(LDFLAGS) -ldl
```

**修改后**:
```makefile
$(CC) $(CFLAGS) -shared -fPIC $(LIBDESHOOK_SRC) $(COMMON_SRC) -o $(LIBDESHOOK) $(LDFLAGS) -ldl -lpthread
```

**效果**: 链接pthread库以支持互斥锁。

## 🎯 设计原理

### 问题根源

```
BIRD 3多线程:
┌─────────────────────┐
│  R1进程              │
│  ├─ 线程1 [0001]     │  同时与desd通信
│  │   └─ 发送 req_32 │ ─┐
│  └─ 线程2 [0002]     │  ├─→ desd收到混乱的消息
│      └─ 发送 req_33 │ ─┘
└─────────────────────┘
```

### 解决方案

```
加互斥锁后:
┌─────────────────────┐
│  R1进程              │
│  ├─ 线程1 [0001]     │
│  │   🔒 持有锁       │ → 独占与desd通信
│  │   └─ 发送 req_32  │
│  └─ 线程2 [0002]     │
│      ⏸️ 等待锁       │   (阻塞)
└─────────────────────┘

线程1完成后:
│  ├─ 线程1 [0001]     │
│  │   🔓 释放锁       │
│  └─ 线程2 [0002]     │
│      🔒 获得锁       │ → 现在可以通信了
│      └─ 发送 req_33  │
```

## ✅ 验证效果

### 修改前的问题日志

```
[LIBDESHOOK WARNING] Response request_id mismatch! Expected req_33_1, Got req_32_1.
[LIBDESHOOK WARNING] Response request_id mismatch! Expected req_32_1, Got req_33_1.
[LIBDESHOOK ERROR] R2 clock_gettime() no virtual time in response.
bird: 2025-12-05 09:31:22.626 [0002] <ERR> Monotonic clock is broken
```

### 修改后期望结果

- ✅ 不再有request_id mismatch警告
- ✅ 不再有响应混乱
- ✅ request_id严格递增，无重复
- ✅ 时钟数据正确
- ✅ BIRD 3稳定运行

## 📝 核心思想

**实现DES的单线程假设**：虽然BIRD 3内部是多线程的，但通过互斥锁确保在任意时刻，每个路由器只有一个线程在与desd通信，从而符合DES的设计假设：

```
一个路由器 = 一个执行流（对desd而言）
```

这样，BIRD 3的多线程可以继续处理内部任务（如协议处理、路由计算），但与desd的交互是串行化的，避免了并发冲突。

## 🚀 下一步

1. 清理容器和进程
2. 重新运行测试
3. 验证日志中不再有线程安全问题
4. 验证BGP连接能够正常建立
