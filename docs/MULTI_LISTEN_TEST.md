# 多地址监听功能测试说明

## 功能概述

改进后的架构支持一个路由器同时监听多个 socket 地址（最多 10 个）。

### 改进内容

1. **数据结构改进**
   - `RouterInfo` 中的 `listen_address[256]` → `listen_addresses[10][256]`
   - 新增 `listen_count` 字段记录监听地址数量
   - 移除了 `is_listening` 单一标志

2. **函数改进**
   - `find_router_by_listen_address()`: 遍历所有监听地址进行匹配
   - `handle_listen_event()`: 支持追加地址，检查重复，上限 10 个
   - `init_desd()`: 初始化监听地址数组

3. **新功能**
   - ✅ 支持同一路由器监听多个地址
   - ✅ 自动检测并忽略重复的监听地址
   - ✅ 监听地址上限保护（10 个）
   - ✅ 详细的日志输出

## 测试程序

### 1. r_multi_listen_test (监听端)

**功能：** 测试一个路由器监听 3 个不同地址

**监听地址：**
- `/tmp/router_socket1`
- `/tmp/router_socket2`
- `/tmp/router_socket3`

### 2. r_multi_connect_test (连接端)

**功能：** 依次连接到上述 3 个地址，验证都能成功连接

## 测试步骤

### 基本测试（不使用 desd，验证程序逻辑）

```bash
# 终端 1: 启动监听端
./r_multi_listen_test

# 终端 2: 启动连接端
./r_multi_connect_test
```

**预期结果：**
- 监听端成功监听 3 个地址
- 连接端成功连接到所有 3 个地址

---

### 完整测试（使用 desd 和 LD_PRELOAD）

```bash
# 终端 1: 启动 desd
sudo ./desd

# 终端 2: 使用 LD_PRELOAD 启动监听端（ROUTER_ID=1）
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r_multi_listen_test

# 终端 3: 使用 LD_PRELOAD 启动连接端（ROUTER_ID=2）
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r_multi_connect_test
```

**预期 desd 日志：**
```
[DESD] R1 is now listening on /tmp/router_socket1 (total: 1 address).
[DESD] R1 is now listening on /tmp/router_socket2 (total: 2 addresses).
[DESD] R1 is now listening on /tmp/router_socket3 (total: 3 addresses).
[DESD] R1 already listening on /tmp/router_socket1, ignoring duplicate.
...
[DESD] R2 sent CONNECT_REQUEST (fd:4). Scheduled CONNECTION_ESTABLISHED_EVENT for R1 (server) ...
[DESD] R2 sent CONNECT_REQUEST (fd:5). Scheduled CONNECTION_ESTABLISHED_EVENT for R1 (server) ...
[DESD] R2 sent CONNECT_REQUEST (fd:6). Scheduled CONNECTION_ESTABLISHED_EVENT for R1 (server) ...
```

**关键观察点：**
1. ✅ R1 成功注册 3 个监听地址
2. ✅ 重复监听被正确忽略
3. ✅ R2 能够连接到 R1 的不同监听地址
4. ✅ `find_router_by_listen_address()` 能正确找到 R1

## 验证要点

### 1. 多地址支持
```
router_states[1].listen_count = 3
router_states[1].listen_addresses[0] = "/tmp/router_socket1"
router_states[1].listen_addresses[1] = "/tmp/router_socket2"
router_states[1].listen_addresses[2] = "/tmp/router_socket3"
```

### 2. 地址查找
```c
find_router_by_listen_address("/tmp/router_socket1") → 1 ✓
find_router_by_listen_address("/tmp/router_socket2") → 1 ✓
find_router_by_listen_address("/tmp/router_socket3") → 1 ✓
find_router_by_listen_address("/tmp/nonexistent") → -1 ✓
```

### 3. 重复检测
```
第一次 listen("/tmp/router_socket1") → 成功，count=1
第二次 listen("/tmp/router_socket1") → 忽略，count=1
```

### 4. 上限保护
```
listen 第 1-10 个地址 → 成功
listen 第 11 个地址 → 错误："Too many listen addresses"
```

## 实际应用场景

### 场景 1: 多协议路由器
```c
// R1 同时提供不同服务
listen("/tmp/r1_data");      // 数据传输端口
listen("/tmp/r1_control");   // 控制管理端口
listen("/tmp/r1_debug");     // 调试接口
```

### 场景 2: 多网络接口
```c
// R1 连接多个网络
listen("/tmp/r1_lan1");      // LAN1 接口
listen("/tmp/r1_lan2");      // LAN2 接口
listen("/tmp/r1_wan");       // WAN 接口
```

### 场景 3: 服务分离
```c
// R1 提供不同服务
listen("/tmp/r1_http");      // HTTP 服务
listen("/tmp/r1_https");     // HTTPS 服务
listen("/tmp/r1_ssh");       // SSH 服务
```

## 性能影响

### 查找复杂度
- **旧版本**: O(n)，n = 路由器数量
- **新版本**: O(n × m)，m = 每个路由器的监听地址数量
- **实际影响**: 由于 n 和 m 都很小（通常 < 10），影响可忽略

### 内存占用
- **旧版本**: 256 字节/路由器
- **新版本**: 2560 字节/路由器 (10 × 256)
- **总增量**: 约 2.3 KB/路由器

## 兼容性

✅ **完全向后兼容**
- 只监听 1 个地址的路由器无需修改代码
- 现有测试程序 (r1, r2, r1_timeout_test, r2_timeout_test) 无需改动
- 行为与旧版本完全一致

## 总结

改进后的架构成功支持一个路由器监听多个 socket，具有：
- ✅ 灵活性：最多 10 个监听地址
- ✅ 安全性：上限保护、重复检测
- ✅ 兼容性：完全向后兼容
- ✅ 高效性：性能影响可忽略
- ✅ 可扩展性：适应复杂的网络拓扑

