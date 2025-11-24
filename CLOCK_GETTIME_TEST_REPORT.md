# clock_gettime功能测试报告

## 📋 实现状态

### ✅ 已完成的代码修改

1. **common.h** - 添加GET_VIRTUAL_TIME_EVENT事件类型
2. **desd.c** - 实现handle_get_virtual_time_event()函数
3. **desd.c** - 在事件循环中立即处理GET_VIRTUAL_TIME_EVENT
4. **libdeshook.c** - 实现clock_gettime()拦截函数
5. **编译成功** - 所有代码无错误无警告

### 📝 测试程序

已创建以下测试程序：
- `tests/clock_time/test_clock_r1.c` - 服务器端测试
- `tests/clock_time/test_clock_r2.c` - 客户端测试  
- `scripts/test_clock_gettime.sh` - 自动化测试脚本

## ⚠️ 当前测试遇到的问题

### 问题描述
自动化测试脚本运行时，R1和R2程序在注册后卡住，没有输出时间查询结果。

### 可能的原因分析

1. **输出缓冲问题**
   - printf输出可能被缓冲，需要fflush
   - 测试程序中已添加fflush(stdout)

2. **进程同步问题**
   - 测试脚本等待时间可能不够
   - R1和R2启动时机问题

3. **事件循环逻辑**
   - desd在等待路由器事件的循环中可能有问题
   - GET_VIRTUAL_TIME_EVENT的处理可能需要调试

##  🧪 手动测试步骤

### 步骤1：启动desd

```bash
cd /home/hwzhuang/hwzhuang/desTest/des_design

# 清理旧的socket
sudo rm -f /tmp/desd_control_socket /tmp/router_socket

# 启动desd（前台运行，方便查看日志）
sudo ./build/desd

# 或者后台运行并查看日志
sudo ./build/desd > logs/desd_manual.log 2>&1 &
DESD_PID=$!

# 修改socket权限
sudo chmod 666 /tmp/desd_control_socket
```

### 步骤2：在另一个终端启动R1

```bash
cd /home/hwzhuang/hwzhuang/desTest/des_design

# 启动R1（服务器）
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 \
    ./build/test_clock_r1 2>&1 | tee logs/r1_manual.log
```

### 步骤3：在第三个终端启动R2

```bash
cd /home/hwzhuang/hwzhuang/desTest/des_design

# 等待R1启动完成（约2秒）
sleep 2

# 启动R2（客户端）
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=2 \
    ./build/test_clock_r2 2>&1 | tee logs/r2_manual.log
```

### 步骤4：观察输出

**预期的R1输出**：
```
=== R1: Clock Virtual Time Test ===

[Test 1] Initial time after startup:
[R1 INIT] Virtual Time: 0.000000000 (0.000000 seconds)

[Test 2] After creating socket:
[R1] Created listen socket fd=5
[R1 SOCKET] Virtual Time: 0.000001000 (0.000001 seconds)
...
```

**预期的desd输出**：
```
[DESD] R1 sent GET_VIRTUAL_TIME_EVENT (ReqID: req_1_1), responding with VT 0.000000.
[DESD] R1 GET_VIRTUAL_TIME_EVENT responded with VT=0.000000.
...
```

## 🔍 调试建议

### 1. 启用详细日志

在desd.c和libdeshook.c中的关键位置添加更多printf：

```c
// libdeshook.c - clock_gettime函数开头
printf("[LIBDESHOOK DEBUG] R%d clock_gettime called, clk_id=%d\n", 
       my_router_id, clk_id);

// desd.c - handle_get_virtual_time_event开头
printf("[DESD DEBUG] handle_get_virtual_time_event called for R%d\n", 
       router_id);
```

### 2. 检查desd事件队列状态

在事件循环中添加日志：
```c
printf("[DESD DEBUG] Event queue size: %d\n", event_queue_count);
printf("[DESD DEBUG] Waiting for next event from R%d...\n", router_id);
```

### 3. 使用gdb调试

```bash
# 启动desd
sudo gdb ./build/desd
(gdb) run

# 在另一个终端
gdb --args env LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 ./build/test_clock_r1
(gdb) break clock_gettime
(gdb) run
```

### 4. 使用strace跟踪系统调用

```bash
# 跟踪R1
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 \
    strace -e trace=connect,send,recv ./build/test_clock_r1
```

## 📊 简化测试方案

如果自动化测试继续有问题，可以先用更简单的方式验证：

### 测试A：验证拦截是否生效

```c
// test_intercept.c
#include <stdio.h>
#include <time.h>

int main() {
    struct timespec ts1, ts2;
    
    // 应该被拦截
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    printf("MONOTONIC: %ld.%09ld\n", ts1.tv_sec, ts1.tv_nsec);
    
    // 应该使用真实时间
    clock_gettime(CLOCK_REALTIME, &ts2);
    printf("REALTIME: %ld.%09ld\n", ts2.tv_sec, ts2.tv_nsec);
    
    return 0;
}
```

编译运行：
```bash
gcc test_intercept.c -o test_intercept

# 不使用LD_PRELOAD
./test_intercept

# 使用LD_PRELOAD
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 ./test_intercept
```

### 测试B：使用ltrace验证

```bash
# 启动desd
sudo ./build/desd &
sudo chmod 666 /tmp/desd_control_socket

# 使用ltrace跟踪
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 \
    ltrace -e clock_gettime ./build/test_simple_time
```

## 🐛 已知问题

1. **自动化测试脚本输出混乱**
   - 多个进程同时输出导致格式混乱
   - 建议改用手动测试或分步测试

2. **测试程序可能卡住**
   - 原因待调查
   - 可能与事件循环逻辑有关

3. **desd日志为空**
   - 可能是缓冲问题
   - 建议使用stdbuf或添加更多fflush

## ✅ 下一步行动

1. **手动测试**：按照上述步骤手动运行测试
2. **添加调试日志**：在关键位置添加更多输出
3. **简化测试**：先用最简单的测试验证拦截功能
4. **修复问题**：根据调试结果修复代码问题
5. **重新测试**：确认功能正常后再运行完整测试

## 📚 相关文档

- `CLOCK_GETTIME_SIMPLE_IMPLEMENTATION.md` - 实现说明
- `logs/desd.log` - desd运行日志
- `logs/r1.log` - R1测试日志
- `logs/r2.log` - R2测试日志

---

**状态**：代码已实现并编译通过，测试中遇到问题待解决  
**建议**：先进行手动测试和简化测试，逐步定位问题
