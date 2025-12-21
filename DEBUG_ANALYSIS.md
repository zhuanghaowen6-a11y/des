# N=5调试分析报告

**日期**: 2025-12-16 20:10

## 添加的调试信息

### 1. CONNECTION_ESTABLISHED_EVENT处理
```c
[DEBUG-CONN] R%d: connection arrived, pending_count=%d, status=%s, blocked_on=%s, listen_count=%d
[DEBUG-WAKEUP] R%d awakened from SELECT, pending_connections=%d, ready_fds_count=%zu
[DEBUG-NO-WAKEUP] R%d NOT on SELECT when connection arrived (status=%s, pending=%d)
```

### 2. SELECT_CALL处理
```c
[DEBUG-SELECT] R%d SELECT_CALL: timeout=%dms, monitoring %zu fd(s), pending_connections=%d
[DEBUG-LISTEN] R%d fd=%d is listening socket (listen_idx=%d), pending=%d
[DEBUG-LISTEN] R%d fd=%d set POLLIN due to %d pending connection(s)
```

## 预期分析方向

### 如果是时序问题
**预期看到**：
- 大量`DEBUG-NO-WAKEUP`，状态为`RUNNING`
- 少量`DEBUG-WAKEUP`
- `DEBUG-SELECT`显示监控listening socket时pending_connections>0，应该设置POLLIN

### 如果是listening socket识别问题
**预期看到**：
- `DEBUG-SELECT`中monitoring了listening socket
- 但`DEBUG-LISTEN`没有识别出fd是listening socket
- 或者listen_count=0

### 如果是其他问题
需要从日志中发现新的线索。

## 测试执行状态

测试脚本正在运行，等待日志生成...
