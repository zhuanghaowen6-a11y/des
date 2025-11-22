# DES (Distributed Event Simulator) 项目

分布式事件模拟器 - 用于模拟路由器控制平面通信

## 📁 项目结构

```
des_design/
├── src/          # 源代码
├── tests/        # 测试程序（按类别分类）
├── scripts/      # 测试脚本
├── docs/         # 完整文档
├── build/        # 编译输出（自动生成）
├── logs/         # 日志文件（自动生成）
└── Makefile      # 构建配置
```

详细结构说明见 [docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md)

## 🚀 快速开始

### 1. 编译项目

```bash
# 编译核心程序
make

# 或编译所有测试
make full
```

### 2. 运行BIRD支持测试

```bash
sudo ./scripts/test_bird_support.sh
```

### 3. 查看文档

```bash
# 主要文档在docs/目录
ls docs/

# 推荐阅读顺序：
cat docs/README.md                    # 项目总览
cat docs/PROJECT_OVERVIEW.md          # 详细设计
cat docs/BIRD_SUPPORT_SUMMARY.md      # BIRD支持
```

## 📖 主要文档

| 文档 | 说明 |
|------|------|
| [docs/README.md](docs/README.md) | 项目总览和快速开始 |
| [docs/PROJECT_OVERVIEW.md](docs/PROJECT_OVERVIEW.md) | 架构设计和实现细节 |
| [docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md) | 目录结构说明 |
| [docs/BIRD_SUPPORT_SUMMARY.md](docs/BIRD_SUPPORT_SUMMARY.md) | BIRD BGP支持 |
| [docs/FINAL_SUMMARY.md](docs/FINAL_SUMMARY.md) | BIRD支持完成总结 |

## 🎯 核心功能

### 已实现功能

✅ **基础功能**
- DES调度器（虚拟时间管理）
- Socket API拦截（LD_PRELOAD）
- 事件驱动模拟

✅ **网络功能**
- TCP/IP (AF_INET) 通信
- Unix Domain Socket (UDS)
- Poll多路复用（完整事件支持）
- 连接管理和数据传输

✅ **BIRD BGP支持** (最新)
- read()/write() 系统调用拦截
- fcntl() 非阻塞标志管理
- Socket FD智能跟踪
- 非阻塞socket EAGAIN支持

## 🛠️ Makefile 使用

```bash
# 编译
make              # 编译核心程序
make full         # 编译所有程序
make bird-tests   # 编译BIRD测试
make poll-tests   # 编译Poll测试

# 清理
make clean        # 清理编译产物
make clean-all    # 完全清理

# 帮助
make help         # 显示帮助
```

## 🧪 测试

### 运行测试脚本

所有测试脚本在 `scripts/` 目录：

```bash
# BIRD支持测试（8项检查）
sudo ./scripts/test_bird_support.sh

# 非阻塞I/O测试（4项检查）- 新增
sudo ./scripts/test_nonblocking_simple.sh

# Poll增强测试
./scripts/run_poll_enhanced_test.sh

# TCP测试
./scripts/run_tcp_test.sh

# 基础测试
./scripts/test_r1_r2.sh
```

### 测试分类

- **basic/** - 基础r1/r2测试
- **bird/** - BIRD支持测试（read/write/fcntl）
- **poll/** - Poll增强测试
- **tcp/** - TCP通信测试
- **timeout/** - 超时机制测试
- **multi/** - 多连接/多监听测试

## 📊 BIRD支持状态

| 功能 | 状态 | 说明 |
|------|------|------|
| read()/write() | ✅ 完全支持 | 智能转发到recv/send |
| fcntl() | ✅ 完全支持 | F_GETFL/F_SETFL跟踪 |
| Socket FD跟踪 | ✅ 完全支持 | 自动标记和清理 |
| 非阻塞EAGAIN | ⚠️ 部分支持 | libdeshook已实现 |

**测试结果**: 8/8 全部通过 ✅

## 🔧 开发

### 修改源代码

```bash
# 1. 编辑源文件
vim src/libdeshook.c

# 2. 重新编译
make

# 3. 测试
sudo ./scripts/test_bird_support.sh

# 4. 查看日志
cat logs/desd.log
```

### 添加新测试

```bash
# 1. 创建测试文件
vim tests/bird/my_test.c

# 2. 更新Makefile（可选）
vim Makefile

# 3. 编译
make bird-tests

# 4. 运行
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 ./build/my_test
```

## 📝 日志

所有日志文件在 `logs/` 目录（自动创建）：

```bash
logs/
├── desd.log    # DES调度器日志
├── r1.log      # 路由器1日志
└── r2.log      # 路由器2日志
```

## ⚠️ 重要提示

1. **使用LD_PRELOAD时不要用timeout命令**
   ```bash
   # ❌ 错误
   LD_PRELOAD=./build/libdeshook.so timeout 5 ./program
   
   # ✅ 正确
   LD_PRELOAD=./build/libdeshook.so ./program
   ```

2. **运行脚本前确保在正确目录**
   ```bash
   # 脚本会自动切换到项目根目录
   sudo ./scripts/test_bird_support.sh
   ```

3. **查看完整文档了解详情**
   ```bash
   cat docs/PROJECT_OVERVIEW.md
   cat docs/BIRD_SUPPORT_SUMMARY.md
   ```

## 🎉 最近更新

### 2024-11-22: 非阻塞I/O支持完成
- ✅ desd支持非阻塞recv处理
- ✅ 无数据时立即返回EAGAIN
- ✅ 修复connect()未标记socket的bug  
- ✅ 4/4非阻塞测试全部通过
- ✅ 8/8 BIRD基础测试持续通过

### 2024-11-22: BIRD BGP支持完成
- ✅ 实现read()/write()拦截
- ✅ 实现fcntl()非阻塞管理
- ✅ 修复accept()未标记socket的bug
- ✅ 8/8测试全部通过
- ✅ 项目结构重组（清晰的目录分类）

## 📚 更多信息

- **架构设计**: [docs/PROJECT_OVERVIEW.md](docs/PROJECT_OVERVIEW.md)
- **BIRD支持**: [docs/BIRD_SUPPORT_SUMMARY.md](docs/BIRD_SUPPORT_SUMMARY.md)
- **目录结构**: [docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md)
- **完整文档**: [docs/](docs/)

## 🤝 贡献

如需修改代码或添加功能，请参考：
- [docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md) - 目录结构
- [docs/PROJECT_OVERVIEW.md](docs/PROJECT_OVERVIEW.md) - 代码架构

---

**注意**: 本项目使用虚拟时间模拟，所有socket通信都通过DES调度器协调。
