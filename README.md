# DES (Distributed Event Simulator) - 快速开始

## 文档导航

### 📘 必读文档

1. **[PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md)** - **核心文档，新对话必读**
   - 项目架构和设计原理
   - 关键数据结构和事件流程
   - 重要设计决策和解决方案
   - 常见问题排查指南

### 📖 功能特性文档

2. **[POLL_SUPPORT.md](POLL_SUPPORT.md)** - poll() 系统调用支持
   - poll() 实现状态
   - FD 精确匹配机制
   - 与 select() 的对比

3. **[MULTI_LISTEN_IMPLEMENTATION.md](MULTI_LISTEN_IMPLEMENTATION.md)** - 多监听地址支持
   - 路由器监听多个地址的实现
   - 动态路由查找机制

4. **[COMPLETE_IMPLEMENTATION_EXPLANATION.md](COMPLETE_IMPLEMENTATION_EXPLANATION.md)** - 完整实现说明
   - 从 poll() 支持到连接映射修复的完整过程
   - 详细的问题分析和解决方案演进

### 📝 测试文档

5. **[TIMEOUT_TEST_README.md](TIMEOUT_TEST_README.md)** - 超时测试说明
   - 测试程序使用方法
   - 超时机制验证

6. **[MULTI_LISTEN_TEST.md](MULTI_LISTEN_TEST.md)** - 多监听测试
   - 多地址监听测试用例

7. **[run_poll_test_auto_v2.sh](run_poll_test_auto_v2.sh)** - poll 自动化测试脚本

---

## 快速上手

### 1. 编译项目

```bash
make clean
make
```

### 2. 运行基本测试

```bash
# 终端 1：启动调度器
sudo ./desd

# 终端 2：启动服务器（R2）
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r2

# 终端 3：启动客户端（R1）
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r1
# 然后输入消息进行通信
```

### 3. 运行自动化测试

```bash
# poll() 功能测试
sudo ./run_poll_test_auto_v2.sh
```

---

## 项目结构速览

```
核心代码：
  desd.c           - 中央调度器（事件队列、虚拟时间、路由器状态管理）
  libdeshook.c     - API 拦截层（拦截 socket/connect/send/recv 等）
  common.h/c       - 公共定义（消息格式、事件类型、JSON 处理）

测试程序：
  r1.c, r2.c                      - 基本通信测试
  r1_timeout_test.c, r2_timeout_test.c  - 超时机制测试
  r_poll_server_test_v2.c, r_poll_client_test_v2.c  - poll() 精确匹配测试

构建和运行：
  Makefile         - 构建脚本
  *.sh             - 自动化测试脚本
```

---

## 关键概念速查

### 虚拟时间 (Virtual Time)
- 所有路由器在统一的虚拟时间中运行
- 时间只在事件处理时推进
- 独立于真实时间，可以精确控制

### 事件驱动
- 所有操作转换为事件
- 事件按时间戳排序（最小堆）
- desd 依次处理事件

### 双层阻塞
1. **虚拟时间阻塞**：路由器在 desd 中阻塞，等待事件到达
2. **真实系统调用**：desd 允许后，调用真实的系统 API

### 连接映射
```
客户端 R1 (fd:4) <-> 服务器 R2 (fd:5)
         ↑                    ↑
   ConnectionInfo[0]    ConnectionInfo[0]
   peer_router_id: 2    peer_router_id: 1
   peer_socket_fd: 5    peer_socket_fd: 4
```

---

## 重要时序关系

### 连接建立时序
```
VT=0.000: R1 connect()
VT=0.049: R1 CONNECTION_ESTABLISHED (client)  ← 先执行
VT=0.050: R2 CONNECTION_ESTABLISHED (server)  ← 后执行
```
**原因**：确保 real_connect() 先于 real_accept()，避免内核阻塞

### 数据发送时序
```
VT=0.049: R1 connect() 返回
VT=0.049: R1 调用 send()
VT=0.051: PACKET_SEND_EVENT 被处理  ← 延迟 2ms
VT=0.050: R2 accept() 已完成  ← 在 send 之前
```
**原因**：确保发送时服务器连接已注册

---

## 常见问题速查

| 问题 | 可能原因 | 解决方案 |
|------|---------|---------|
| 连接卡死 | 服务器还没 listen | 确保服务器先启动 |
| send 失败 | 连接映射未建立 | 检查 CONNECTION_INFO_EVENT |
| poll 不准确 | socket_fd 未记录 | 检查 PacketBuffer.socket_fd |
| 超时不触发 | 未注册 TIMEOUT_EVENT | 检查 desd 日志 |
| 时间不推进 | 事件队列为空 | 确认路由器正确发送事件 |

详细排查步骤见 [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) 的"常见问题和解决方案"章节。

---

## 关键设计决策

### ✅ PACKET_SEND_EVENT 延迟 2ms
- **问题**：客户端 connect() 后立即 send()，但服务器还没 accept()
- **方案**：延迟 send 事件，确保在 accept() 之后处理
- **位置**：`desd.c:650-655`

### ✅ 客户端不发送 CONNECTION_INFO_EVENT
- **原因**：客户端 socket_fd 已在 CONNECT_REQUEST_EVENT 中发送
- **只有服务器需要发送**：通知 accept() 返回的 fd

### ✅ PacketBuffer 记录 socket_fd
- **目的**：支持 poll() 的精确 FD 匹配
- **机制**：缓冲数据时记录目标 socket_fd

### ✅ ConnectionInfo 包含 peer_socket_fd
- **目的**：支持多连接精确匹配
- **场景**：同一对路由器之间的多个连接

---

## 修改代码指南

### 添加新的系统调用拦截

1. 在 `libdeshook.c` 中添加函数拦截
2. 定义新的事件类型（如果需要）
3. 在 `desd.c` 中添加事件处理函数
4. 更新 `handle_event()` switch-case

### 修改网络延迟参数

```c
// desd.c
double connection_delay = 0.05;      // 连接建立延迟（50ms）
double transmission_delay = 0.1;     // 数据传输延迟（100ms）
double send_event_delay = 0.002;     // send 事件延迟（2ms）
```

### 增加路由器数量

```c
// common.h
#define MAX_ROUTERS 5  // 改为需要的数量

// desd.c - main()
while (connected_routers < MAX_ROUTERS) {
    // 等待所有路由器连接
}
```

---

## 获取帮助

### 查看日志
- **desd 日志**：标准输出，显示所有事件处理
- **libdeshook 日志**：每个路由器的标准输出
- **关键信息**：
  - 事件类型和时间戳
  - 路由器状态变化
  - 连接映射建立
  - 数据包缓冲和传输

### 调试技巧
1. 对比三个终端的输出时序
2. 检查虚拟时间是否正常推进
3. 确认事件队列中的事件顺序
4. 验证连接映射是否正确建立

### 深入理解
- 阅读 [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) 的"事件流程"章节
- 参考 [COMPLETE_IMPLEMENTATION_EXPLANATION.md](COMPLETE_IMPLEMENTATION_EXPLANATION.md) 了解演进过程
- 查看测试程序代码了解 API 使用方式

---

## 贡献和维护

### 代码风格
- 使用 4 空格缩进
- 函数命名：`handle_xxx_event()`, `find_xxx()`
- 变量命名：`router_id`, `socket_fd`, `peer_router_id`

### 注释规范
- 关键设计决策必须注释原因
- 复杂逻辑添加流程说明
- 重要时序关系明确标注

### 提交信息
- 清晰描述修改内容和原因
- 引用相关的问题或需求
- 说明测试情况

---

**开始探索 DES 项目，从 [PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) 开始！**

