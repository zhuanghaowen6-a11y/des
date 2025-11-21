# Poll功能使用指南

## 快速开始

DES项目现已支持完整的poll系统调用，包括POLLIN、POLLOUT、POLLERR、POLLHUP等事件类型。

## 支持的事件类型

| 事件 | 值 | 说明 | 支持状态 |
|-----|-----|-----|---------|
| POLLIN | 0x001 | 有数据可读 | ✅ 完全支持 |
| POLLOUT | 0x004 | 可以写入数据 | ✅ 完全支持 |
| POLLERR | 0x008 | 发生错误 | ✅ 部分支持 |
| POLLHUP | 0x010 | 连接挂起 | ✅ 完全支持 |
| POLLNVAL | 0x020 | 无效请求 | ⚠️ 基础支持 |

## 使用示例

### 示例1: 监听数据可读（POLLIN）

```c
#include <poll.h>

int sockfd = socket(...);
connect(sockfd, ...);

struct pollfd fds[1];
fds[0].fd = sockfd;
fds[0].events = POLLIN;  // 监听可读事件
fds[0].revents = 0;

int ret = poll(fds, 1, 5000);  // 5秒超时
if (ret > 0 && (fds[0].revents & POLLIN)) {
    // 有数据可读
    char buffer[1024];
    recv(sockfd, buffer, sizeof(buffer), 0);
}
```

### 示例2: 监听Socket可写（POLLOUT）

```c
struct pollfd fds[1];
fds[0].fd = sockfd;
fds[0].events = POLLOUT;  // 监听可写事件
fds[0].revents = 0;

int ret = poll(fds, 1, 2000);  // 2秒超时
if (ret > 0 && (fds[0].revents & POLLOUT)) {
    // Socket可写
    const char *msg = "Hello";
    send(sockfd, msg, strlen(msg), 0);
}
```

### 示例3: 同时监听多个事件

```c
struct pollfd fds[1];
fds[0].fd = sockfd;
fds[0].events = POLLIN | POLLOUT;  // 同时监听可读和可写
fds[0].revents = 0;

int ret = poll(fds, 1, 5000);
if (ret > 0) {
    if (fds[0].revents & POLLIN) {
        // 有数据可读
        recv(sockfd, buffer, sizeof(buffer), 0);
    }
    if (fds[0].revents & POLLOUT) {
        // Socket可写
        send(sockfd, data, len, 0);
    }
    if (fds[0].revents & POLLHUP) {
        // 连接已断开
        printf("Connection closed\n");
    }
}
```

### 示例4: 监听多个Socket

```c
struct pollfd fds[3];

// Socket 1
fds[0].fd = sock1;
fds[0].events = POLLIN;
fds[0].revents = 0;

// Socket 2
fds[1].fd = sock2;
fds[1].events = POLLIN | POLLOUT;
fds[1].revents = 0;

// Socket 3
fds[2].fd = sock3;
fds[2].events = POLLIN;
fds[2].revents = 0;

int ret = poll(fds, 3, 10000);  // 10秒超时
if (ret > 0) {
    for (int i = 0; i < 3; i++) {
        if (fds[i].revents & POLLIN) {
            // fds[i].fd有数据可读
            recv(fds[i].fd, buffer, sizeof(buffer), 0);
        }
        if (fds[i].revents & POLLOUT) {
            // fds[i].fd可写
            send(fds[i].fd, data, len, 0);
        }
    }
}
```

## 运行测试程序

### 测试环境准备

```bash
# 1. 编译项目
make

# 2. 编译增强poll测试程序
make r_poll_server_enhanced r_poll_client_enhanced
```

### 手动测试

打开三个终端：

**终端1：启动desd**
```bash
sudo ./desd
```

**终端2：启动服务器（R2）**
```bash
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r_poll_server_enhanced
```

**终端3：启动客户端（R1）**
```bash
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r_poll_client_enhanced
```

### 自动化测试

```bash
sudo ./run_poll_enhanced_test.sh
```

## 常见模式

### 模式1: 非阻塞轮询

```c
struct pollfd fds[1];
fds[0].fd = sockfd;
fds[0].events = POLLIN;

int ret = poll(fds, 1, 0);  // timeout=0，立即返回
if (ret > 0) {
    // 立即有数据
    recv(sockfd, buffer, sizeof(buffer), 0);
} else {
    // 当前无数据
}
```

### 模式2: 带超时的阻塞等待

```c
struct pollfd fds[1];
fds[0].fd = sockfd;
fds[0].events = POLLIN;

int ret = poll(fds, 1, 5000);  // 等待最多5秒
if (ret > 0) {
    // 数据到达
} else if (ret == 0) {
    // 超时
    printf("Timeout\n");
} else {
    // 错误
    perror("poll");
}
```

### 模式3: 无限等待

```c
struct pollfd fds[1];
fds[0].fd = sockfd;
fds[0].events = POLLIN;

int ret = poll(fds, 1, -1);  // timeout=-1，无限等待
if (ret > 0) {
    recv(sockfd, buffer, sizeof(buffer), 0);
}
```

## 与select()的对比

| 特性 | select() | poll() |
|------|----------|--------|
| **FD限制** | 有限制（通常1024） | 无限制 |
| **API风格** | fd_set位图 | pollfd数组 |
| **修改参数** | 会修改fd_set | 不修改events，只修改revents |
| **超时精度** | 微秒级 | 毫秒级 |
| **DES支持** | ✅ 完全支持 | ✅ 完全支持 |

## 注意事项

### 1. revents字段

poll()返回后，检查`revents`字段而不是`events`字段：

```c
// ✅ 正确
if (fds[0].revents & POLLIN) { ... }

// ❌ 错误
if (fds[0].events & POLLIN) { ... }
```

### 2. 错误处理

始终检查poll()的返回值：

```c
int ret = poll(fds, nfds, timeout);
if (ret < 0) {
    // poll()调用失败
    perror("poll");
} else if (ret == 0) {
    // 超时
} else {
    // ret > 0: 有fd就绪
}
```

### 3. POLLERR/POLLHUP

这些标志无需在`events`中设置，poll()会自动检测并在`revents`中返回：

```c
struct pollfd fds[1];
fds[0].fd = sockfd;
fds[0].events = POLLIN;  // 不需要加POLLERR/POLLHUP

poll(fds, 1, -1);

// poll()会自动检测错误和挂起
if (fds[0].revents & POLLERR) {
    printf("Error on socket\n");
}
if (fds[0].revents & POLLHUP) {
    printf("Connection hung up\n");
}
```

## 性能提示

1. **尽量复用pollfd数组**：避免频繁分配和释放
2. **合理设置超时**：根据应用需求选择合适的超时时间
3. **及时处理就绪的FD**：避免在poll()循环中积压数据

## 调试技巧

### 打印revents

```c
void print_revents(short revents) {
    printf("revents = 0x%04x (", revents);
    if (revents & POLLIN)  printf("POLLIN ");
    if (revents & POLLOUT) printf("POLLOUT ");
    if (revents & POLLERR) printf("POLLERR ");
    if (revents & POLLHUP) printf("POLLHUP ");
    if (revents & POLLNVAL) printf("POLLNVAL ");
    printf(")\n");
}

// 使用
poll(fds, nfds, timeout);
print_revents(fds[0].revents);
```

### 查看desd日志

desd会输出详细的事件处理日志：

```bash
sudo ./desd 2>&1 | grep "select/poll"
```

## 故障排查

### 问题1: poll()总是立即返回

**可能原因：**
- Socket实际上已就绪
- 使用了timeout=0

**解决方法：**
检查socket状态和timeout设置

### 问题2: poll()永不返回

**可能原因：**
- 未设置超时（timeout=-1）且无事件发生
- desd未正确处理事件

**解决方法：**
- 设置合理的超时时间
- 检查desd日志

### 问题3: revents不正确

**可能原因：**
- 连接状态不正确
- events设置错误

**解决方法：**
- 检查连接是否建立
- 确认events字段设置正确

## 更多示例

完整的测试程序请参考：
- `r_poll_server_enhanced.c` - 服务器示例
- `r_poll_client_enhanced.c` - 客户端示例
- `r_poll_server_test_v2.c` - 多连接示例

## 相关文档

- `PROJECT_OVERVIEW.md` - 项目总体架构
- `POLL_SUPPORT.md` - Poll支持说明
- `POLL_ENHANCEMENT_REPORT.md` - 增强功能报告

---

有问题？查看项目文档或检查desd日志获取更多信息。
