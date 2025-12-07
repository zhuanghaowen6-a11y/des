# Listening Socket POLLIN事件修复

## 问题描述

**现象**：
在DES环境下，BIRD路由器双方都通过`connect()`主动发起连接，没有一方调用`accept()`，导致"Invalid connection mapping"错误。

**根本原因**：
desd的`SELECT_CALL`（poll/select）处理逻辑中，`POLLIN`事件检查只考虑了"数据包到达"，没有考虑"listening socket有新连接待accept"。这导致BIRD的poll()永远检测不到listening socket就绪，从而无法调用accept()。

## strace验证

### 真实环境下的BIRD行为（非DES）

**R1（客户端）**:
```
bind(11, 0.0.0.0:179)       # 创建listening socket
listen(11, 8)               # 但不会用它accept
socket(12)
connect(12 → R2:179)        # 主动连接
```

**R2（服务器）**:
```
bind(11, 0.0.0.0:179)
listen(11, 8)
poll([fd=11, ...]) = POLLIN  # ✅ 检测到listening socket就绪！
accept(11) → fd=12           # ✅ 接受连接
```

### DES环境下的BIRD行为（修复前）

**R1和R2都是**:
```
listen(fd=12)
poll([fd=12, ...]) = timeout  # ❌ listening socket永远不就绪
connect()                     # 只能主动连接
```

## 技术细节

### POLLIN事件的完整语义

| Socket类型 | POLLIN含义 |
|-----------|-----------|
| 已连接socket | 有数据可读（recv/read不会阻塞） |
| **Listening socket** | **有新连接待accept（accept不会阻塞）** ← 缺失！ |
| 管道/FIFO | 有数据可读 |

### desd之前的实现（片面）

```c
// 检查 POLLIN
if (events & 0x001) {
    // ❌ 只检查数据包
    for (int j = 0; j < pending_packets_count; j++) {
        if (packet matches this fd) {
            revents |= 0x001;  // POLLIN
        }
    }
}
```

## 修复方案

### 修改内容

#### 1. RouterInfo结构体 (`src/desd.c` 第64行)
```c
typedef struct {
    // ...
    int listening_socket_fds[10]; // 新增：跟踪listening socket的fd
    // ...
} RouterInfo;
```

#### 2. libdeshook的listen() (`src/libdeshook.c` 第1279行)
```c
// 在LISTEN_EVENT的payload中添加socket_fd
json_object_set_new(payload_obj, "socket_fd", json_integer(sockfd));
```

#### 3. handle_listen_event() (`src/desd.c` 第826、852行)
```c
// 从payload中提取socket_fd
int socket_fd = json_integer_value(json_object_get(payload_obj, "socket_fd"));

// 记录listening socket的fd
router_states[router_id].listening_socket_fds[index] = socket_fd;
```

#### 4. handle_router_block_request() - SELECT_CALL (`src/desd.c` 第1330-1359行)
```c
// 检查 POLLIN：是否有数据可读或有新连接待accept
if (events & 0x001) {
    // 1. 检查数据包（原有逻辑）
    for (int j = 0; j < pending_packets_count; j++) {
        if (packet matches this fd) {
            revents |= 0x001;  // POLLIN - 数据可读
        }
    }
    
    // 2. 检查listening socket（新增逻辑）✅
    if (!(revents & 0x001)) {
        for (int j = 0; j < listen_count; j++) {
            if (listening_socket_fds[j] == fd) {
                if (pending_connections_count > 0) {
                    revents |= 0x001;  // POLLIN - 新连接待accept
                }
            }
        }
    }
}
```

### 关键流程

```
时间T1: R1发送CONNECT_REQUEST到R2
        ↓
时间T2: desd调度CONNECTION_ESTABLISHED_EVENT给R2（服务器）
        ↓
        handle_connection_established_event():
          - 检测到R2不在ACCEPT_CALL阻塞中
          - pending_connections_count++ ✅（这部分原本就有）
        ↓
时间T3: R2调用poll(listening_socket_fd)
        ↓
        desd的SELECT_CALL处理:
          - 检测到fd是listening socket ✅（新增）
          - 检测到pending_connections_count > 0 ✅（新增）
          - 返回revents = POLLIN ✅（新增）
        ↓
时间T4: R2的poll()返回listening socket就绪
        ↓
时间T5: R2调用accept()
        ↓
        desd的ACCEPT_CALL处理:
          - pending_connections_count-- ✅（原本就有）
          - 返回成功 ✅
        ↓
时间T6: 标准的客户端-服务器连接建立完成！
```

## 预期效果

### 修复前
```
[DESD] R1 (client) connect() completed with R2
[DESD] R2 (client) connect() completed with R1  ← 错误：双方都是客户端
[DESD ERROR] Invalid connection mapping
```

### 修复后
```
[DESD] R1 sent CONNECT_REQUEST
[DESD] R2 has 1 pending connection(s)
[DESD] R2 poll() returns POLLIN for listening socket  ← 新增
[DESD] R2 (server) accept() completed  ← 期望
[DESD] R1 (client) connect() completed
✅ 标准的客户端-服务器连接
```

## 测试建议

1. 运行BIRD测试：`./scripts/test_bird_uds_full.sh`
2. 检查日志：
   - 应该看到`accept() completed`
   - 不应该再看到双方都是`(client) connect() completed`
   - 不应该再看到`Invalid connection mapping`

## 相关文件

- `src/desd.c` - desd主逻辑，RouterInfo结构体、handle_listen_event、handle_router_block_request
- `src/libdeshook.c` - libdeshook的listen()函数
- `logs/bird_r1_strace.log` - 真实环境strace验证日志（R1客户端）
- `logs/bird_r2_strace.log` - 真实环境strace验证日志（R2服务器，有accept）

## 总结

这个修复补全了desd对TCP socket语义的完整支持，使其能够正确模拟listening socket的POLLIN事件，从而让BIRD能够像在真实环境中一样，通过poll()检测到新连接并调用accept()，建立标准的客户端-服务器连接模式。
