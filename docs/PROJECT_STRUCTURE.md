# 项目目录结构说明

## 📁 目录结构

```
des_design/
├── src/                    # 源代码
│   ├── desd.c             # DES调度器
│   ├── libdeshook.c       # LD_PRELOAD拦截库
│   ├── common.c           # 公共函数实现
│   └── common.h           # 公共头文件
│
├── tests/                  # 测试程序
│   ├── basic/             # 基础测试
│   │   ├── r1.c          # 基础路由器1（服务器）
│   │   └── r2.c          # 基础路由器2（客户端）
│   │
│   ├── bird/              # BIRD支持测试
│   │   ├── test_rw_basic_r1.c     # read/write基础测试（服务器）
│   │   ├── test_rw_basic_r2.c     # read/write基础测试（客户端）
│   │   ├── test_readwrite_r1.c    # read/write+poll测试（服务器）
│   │   ├── test_readwrite_r2.c    # read/write+poll测试（客户端）
│   │   ├── test_simple_rw_r1.c    # 简化read/write测试（服务器）
│   │   ├── test_simple_rw_r2.c    # 简化read/write测试（客户端）
│   │   ├── test_minimal.c         # 最小测试程序
│   │   ├── test_no_stdio.c        # 无stdio测试
│   │   └── debug_libinit.c        # lib_init调试程序
│   │
│   ├── poll/              # Poll增强测试
│   │   ├── r_poll_server_enhanced.c   # Poll服务器（增强版）
│   │   ├── r_poll_client_enhanced.c   # Poll客户端（增强版）
│   │   ├── r_poll_server_test_v2.c    # Poll服务器（v2）
│   │   └── r_poll_client_test_v2.c    # Poll客户端（v2）
│   │
│   ├── tcp/               # TCP测试
│   │   ├── r1_test_tcp.c  # TCP测试路由器1
│   │   └── r2_test_tcp.c  # TCP测试路由器2
│   │
│   ├── timeout/           # 超时测试
│   │   ├── r1_timeout_test.c  # 超时测试路由器1
│   │   └── r2_timeout_test.c  # 超时测试路由器2
│   │
│   └── multi/             # 多连接/多监听测试
│       ├── r_multi_connect_test.c  # 多连接测试
│       └── r_multi_listen_test.c   # 多监听测试
│
├── scripts/                # 脚本文件
│   ├── test_bird_support.sh        # BIRD支持测试脚本
│   ├── run_poll_enhanced_test.sh   # Poll增强测试脚本
│   ├── run_poll_test_auto_v2.sh    # Poll自动化测试v2
│   ├── run_tcp_test.sh             # TCP测试脚本
│   ├── test_r1_r2.sh               # 基础测试脚本
│   └── ...                         # 其他测试脚本
│
├── docs/                   # 文档
│   ├── README.md                           # 项目总览
│   ├── PROJECT_OVERVIEW.md                 # 项目详细说明
│   ├── PROJECT_STRUCTURE.md                # 本文档
│   ├── BIRD_SUPPORT_SUMMARY.md             # BIRD支持总结
│   ├── FINAL_SUMMARY.md                    # 最终完成总结
│   ├── POLL_ENHANCEMENT_REPORT.md          # Poll增强报告
│   ├── POLL_USAGE_GUIDE.md                 # Poll使用指南
│   ├── TCP_SUPPORT.md                      # TCP支持文档
│   ├── TCP_TEST_REPORT.md                  # TCP测试报告
│   ├── TIMEOUT_TEST_README.md              # 超时测试说明
│   ├── MULTI_LISTEN_IMPLEMENTATION.md      # 多监听实现
│   ├── MULTI_LISTEN_TEST.md                # 多监听测试
│   └── COMPLETE_IMPLEMENTATION_EXPLANATION.md  # 完整实现说明
│
├── build/                  # 编译输出（.gitignore）
│   ├── desd               # DES调度器可执行文件
│   ├── libdeshook.so      # 拦截库
│   ├── r1, r2             # 基础测试程序
│   ├── test_rw_basic_r1   # BIRD测试程序
│   └── ...                # 其他编译产物
│
├── logs/                   # 日志文件（.gitignore）
│   ├── desd.log           # desd日志
│   ├── r1.log             # r1日志
│   ├── r2.log             # r2日志
│   └── ...                # 其他日志
│
├── .vscode/                # IDE配置
│   └── launch.json        # VSCode调试配置
│
├── Makefile                # 构建配置
├── .gitignore              # Git忽略配置
└── reorganize_project.sh   # 项目重组脚本

```

## 🛠️ Makefile 使用

### 基础编译
```bash
make           # 编译核心程序（desd, libdeshook.so, r1, r2）
make all       # 同上
make full      # 编译所有程序（包括所有测试）
```

### 分类编译
```bash
make bird-tests      # 编译BIRD测试程序
make poll-tests      # 编译Poll测试程序
make tcp-tests       # 编译TCP测试程序
make timeout-tests   # 编译超时测试程序
make multi-tests     # 编译多连接测试程序
```

### 清理
```bash
make clean      # 清理build/和logs/的内容
make clean-all  # 删除build/和logs/目录
```

### 帮助
```bash
make help       # 显示帮助信息
```

## 📝 运行测试

### BIRD支持测试
```bash
cd scripts
sudo ./test_bird_support.sh
```

### Poll增强测试
```bash
cd scripts
./run_poll_enhanced_test.sh
```

### TCP测试
```bash
cd scripts
./run_tcp_test.sh
```

### 基础测试
```bash
cd scripts
./test_r1_r2.sh
```

## 📖 文档导航

### 核心文档
1. **README.md** - 项目总览，快速开始
2. **PROJECT_OVERVIEW.md** - 架构和设计细节
3. **PROJECT_STRUCTURE.md** - 本文档，目录结构说明

### 功能文档
4. **BIRD_SUPPORT_SUMMARY.md** - BIRD支持详细说明
5. **POLL_ENHANCEMENT_REPORT.md** - Poll增强功能报告
6. **TCP_SUPPORT.md** - TCP通信支持
7. **MULTI_LISTEN_IMPLEMENTATION.md** - 多监听地址实现

### 测试文档
8. **TIMEOUT_TEST_README.md** - 超时测试说明
9. **MULTI_LISTEN_TEST.md** - 多监听测试
10. **TCP_TEST_REPORT.md** - TCP测试报告

### 总结文档
11. **FINAL_SUMMARY.md** - BIRD支持完成总结
12. **COMPLETE_IMPLEMENTATION_EXPLANATION.md** - 完整实现说明

## 🔧 开发工作流

### 1. 修改源代码
```bash
# 编辑源代码
vim src/libdeshook.c

# 重新编译
make

# 测试
cd scripts
sudo ./test_bird_support.sh
```

### 2. 添加新测试
```bash
# 创建测试文件
vim tests/bird/my_new_test.c

# 在Makefile中添加编译规则
vim Makefile

# 编译
make bird-tests

# 运行
cd build
LD_PRELOAD=./libdeshook.so ROUTER_ID=1 ./my_new_test
```

### 3. 查看日志
```bash
# 所有日志都在logs/目录
ls -la logs/

# 查看特定日志
cat logs/desd.log
cat logs/r1.log
```

## 📦 版本控制

### .gitignore 配置
以下目录/文件已被忽略：
- `build/` - 编译产物
- `logs/` - 日志文件
- `.vscode/` - IDE配置
- `*.o`, `*.so` - 中间文件
- `*.old`, `*.bak` - 备份文件

### 建议的Git工作流
```bash
# 添加源代码和文档
git add src/ docs/ tests/ scripts/ Makefile

# 不要添加build和logs
# （已在.gitignore中配置）

# 提交
git commit -m "Update BIRD support features"
```

## 🎯 重要文件说明

### 核心源代码
- **src/desd.c** (89KB) - DES调度器，事件队列管理
- **src/libdeshook.c** (49KB) - LD_PRELOAD拦截库，系统调用拦截
- **src/common.c/h** - 公共数据结构和函数

### 关键测试
- **tests/bird/test_rw_basic_*.c** - BIRD支持的主要测试程序
- **tests/poll/r_poll_*_enhanced.c** - Poll增强功能测试
- **tests/basic/r1.c, r2.c** - 最基础的测试程序

### 重要脚本
- **scripts/test_bird_support.sh** - BIRD支持自动化测试（8项检查）
- **scripts/run_poll_enhanced_test.sh** - Poll增强自动化测试

## 💡 提示

1. **编译前**：确保在项目根目录（有Makefile的地方）
2. **运行脚本前**：进入scripts/目录
3. **查看日志**：检查logs/目录
4. **清理**：定期运行`make clean`清理编译产物

## 🔄 项目重组

如果需要重新整理项目结构：
```bash
# 备份当前状态
cp -r . ../des_design_backup

# 运行重组脚本
./reorganize_project.sh

# 更新Makefile（已自动完成）
make clean
make all
```

---

**注意**：本文档描述的是重组后的目录结构。如果你的项目还是旧结构，请运行`./reorganize_project.sh`进行重组。
