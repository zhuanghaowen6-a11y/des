# N-Router扩展测试总结

**日期**: 2025-12-16 20:00
**目标**: 从成功的2-router Docker版本扩展到N=5

---

## 📋 完成的工作

### 1. 代码修改

#### `src/desd.c`
- ✅ 修改`main()`支持命令行参数指定路由器数量
- ✅ 将`MAX_ROUTERS`从2改为100
- ✅ 将`MAX_CONNECTIONS_PER_ROUTER`从10改为50
- ✅ **关键修复**：移除`handle_connection_info_event()`中硬编码的R1/R2逻辑
  - 客户端连接：不再假设peer是固定的另一个路由器
  - 服务端连接：遍历所有路由器查找客户端连接

#### `scripts/test_n_bird_docker.sh`
- ✅ 创建新的N-router测试脚本
- ✅ 支持命令行参数：`./test_n_bird_docker.sh <N> <duration>`
- ✅ 自动生成N个BIRD配置（full mesh BGP）
- ✅ 并行启动所有BIRD进程
- ✅ 自动清理功能

---

## 🎯 测试结果

### ✅ N=2测试 - **成功**

```bash
sudo bash scripts/test_n_bird_docker.sh 2 60
```

**结果**：
- 2个容器全部启动并运行
- 2个路由器成功注册到DESD
- **BGP会话已建立**：R1 ↔ R2
- 虚拟时间正常推进：VT达到13000+秒
- 连接映射正确建立

**日志证据**：
```
bird: 2025-12-16 11:51:22.554 [0002] <TRACE> r2: BGP session established
[DESD] Router 1 registered with fd 4. (1/2).
[DESD] Router 2 registered with fd 5. (2/2).
[DESD] Registered connection: R1 (fd:13) <-> R2 (fd:13)
```

---

### ❌ N=5测试 - **失败**

```bash
sudo bash scripts/test_n_bird_docker.sh 5 90
```

**结果**：
- 5个容器全部启动
- 5个路由器成功注册到DESD
- BIRD进程启动后很快失败
- **BGP会话部分建立**：R1, R3, R4各建立1个会话
- **DESD最终崩溃**：BIRD报告"DESD disconnected"

**关键问题**：
```
[DESD] R1 has 4251 pending connection(s) (connection established but accept not called yet)
[LIBDESHOOK ERROR] DESD disconnected during response wait
```

**pending connections数量对比**：
- N=2: 少量pending，能及时accept()
- N=5: 4251个pending，严重堆积，accept()未调用

---

## 🔍 问题分析

### 根本原因
**accept()在多路由器场景下未被正确调用**

N=5时有10个BGP连接需要建立（5路由器full mesh），每个连接：
1. 客户端发起连接 → `CONNECT_REQUEST`
2. DESD调度 → `CONNECTION_ESTABLISHED` 
3. 服务端应该调用`accept()` → **但未发生**
4. pending connections持续堆积 → DESD负载过高 → 崩溃

### 为什么N=2成功，N=5失败？

**N=2时**：
- 只有2个BGP连接
- pending connections少
- accept()能及时处理

**N=5时**：
- 10个BGP连接同时建立
- pending connections爆炸式增长
- accept()调用被阻塞或延迟
- DESD处理不过来

这与之前namespace方案遇到的问题**完全相同**：poll()对listening socket的POLLIN事件处理有问题。

---

## 💡 下一步建议

### 选项1：深入调试accept()机制（预计2-3天）
- 检查poll()为何不通知listening socket就绪
- 分析pending_accepts队列处理逻辑
- 可能需要修改poll拦截或accept拦截逻辑

### 选项2：限制并发连接建立（快速解决，预计2小时）
- 修改BIRD配置，错开BGP连接时间
- 每个路由器延迟启动（如R1立即，R2延迟5秒，R3延迟10秒）
- 避免同时建立大量连接

### 选项3：增加accept()轮询（实验性，预计1小时）
- 在DESD中主动触发listening socket的poll事件
- 强制BIRD调用accept()
- 可能不是根本解决方案

---

## 📊 性能数据

| 指标 | N=2 | N=5 |
|------|-----|-----|
| 容器启动 | 2/2 ✅ | 5/5 ✅ |
| 路由器注册 | 2/2 ✅ | 5/5 ✅ |
| BIRD进程 | 2/2 ✅ | 0/5 ❌ |
| BGP会话 | 2/2 ✅ | 3/20 ⚠️ |
| 虚拟时间 | 13000+s ✅ | 11000s ⚠️ |
| Pending connections | <10 ✅ | 4251 ❌ |
| DESD日志大小 | 13MB | 14MB |

---

## 📁 关键文件

### 修改的文件
- `src/desd.c` - 支持N-router，修复硬编码
- `src/common.h` - 清理TCP控制相关定义
- `scripts/test_n_bird_docker.sh` - 新的N-router测试脚本

### 测试日志
- `logs/desd_n2.log` - N=2成功日志（13MB）
- `logs/desd_n5.log` - N=5失败日志（14MB）

### BIRD配置
- 自动生成在`/tmp/bird_rX.conf`
- Full mesh BGP配置
- AS号：65001-65005
- IP：10.0.X.X

---

## ✅ 验证N=2仍然工作

如果需要验证修改后的代码N=2仍然正常：

```bash
# 清理环境
sudo pkill -9 desd
sudo docker rm -f $(sudo docker ps -aq)
sudo docker network rm bird_test_net

# 运行N=2测试
sudo bash scripts/test_n_bird_docker.sh 2 60
```

预期：BGP会话成功建立。

---

## 🎯 结论

**成功完成**：
- ✅ 代码通用化（支持任意N）
- ✅ N=2测试完全成功
- ✅ 创建了完整的N-router测试框架

**待解决**：
- ❌ N=5的accept()调用问题
- ❌ pending connections堆积
- ❌ DESD在高负载下的稳定性

**建议**：
根据时间和优先级选择上述3个选项之一继续。如果时间紧迫，建议选项2（错开连接时间）最快见效。
