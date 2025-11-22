#!/bin/bash
# reorganize_project.sh - 重组项目目录结构

echo "========================================="
echo "Reorganizing Project Structure"
echo "========================================="

# 创建新目录结构
echo "Creating directory structure..."
mkdir -p src
mkdir -p tests/basic
mkdir -p tests/bird
mkdir -p tests/poll
mkdir -p tests/tcp
mkdir -p tests/timeout
mkdir -p tests/multi
mkdir -p scripts
mkdir -p docs
mkdir -p build
mkdir -p logs
mkdir -p .vscode

echo "✓ Directories created"

# 移动源代码文件
echo ""
echo "Moving source files..."
mv desd.c src/ 2>/dev/null
mv libdeshook.c src/ 2>/dev/null
mv common.c src/ 2>/dev/null
mv common.h src/ 2>/dev/null
echo "✓ Source files moved to src/"

# 移动文档
echo ""
echo "Moving documentation..."
mv *.md docs/ 2>/dev/null
echo "✓ Documentation moved to docs/"

# 移动脚本
echo ""
echo "Moving scripts..."
mv *.sh scripts/ 2>/dev/null
# 把自己移回来
mv scripts/reorganize_project.sh . 2>/dev/null
echo "✓ Scripts moved to scripts/"

# 移动基础测试
echo ""
echo "Moving basic tests..."
mv r1.c tests/basic/ 2>/dev/null
mv r2.c tests/basic/ 2>/dev/null
echo "✓ Basic tests moved to tests/basic/"

# 移动BIRD测试
echo ""
echo "Moving BIRD tests..."
mv test_rw_basic_r1.c tests/bird/ 2>/dev/null
mv test_rw_basic_r2.c tests/bird/ 2>/dev/null
mv test_readwrite_r1.c tests/bird/ 2>/dev/null
mv test_readwrite_r2.c tests/bird/ 2>/dev/null
mv test_simple_rw_r1.c tests/bird/ 2>/dev/null
mv test_simple_rw_r2.c tests/bird/ 2>/dev/null
mv test_minimal.c tests/bird/ 2>/dev/null
mv test_no_stdio.c tests/bird/ 2>/dev/null
mv debug_libinit.c tests/bird/ 2>/dev/null
echo "✓ BIRD tests moved to tests/bird/"

# 移动poll测试
echo ""
echo "Moving poll tests..."
mv r_poll_*_v2.c tests/poll/ 2>/dev/null
mv r_poll_*_enhanced.c tests/poll/ 2>/dev/null
echo "✓ Poll tests moved to tests/poll/"

# 移动TCP测试
echo ""
echo "Moving TCP tests..."
mv r1_test_tcp.c tests/tcp/ 2>/dev/null
mv r2_test_tcp.c tests/tcp/ 2>/dev/null
echo "✓ TCP tests moved to tests/tcp/"

# 移动超时测试
echo ""
echo "Moving timeout tests..."
mv r1_timeout_test.c tests/timeout/ 2>/dev/null
mv r2_timeout_test.c tests/timeout/ 2>/dev/null
echo "✓ Timeout tests moved to tests/timeout/"

# 移动多连接测试
echo ""
echo "Moving multi-connection tests..."
mv r_multi_*.c tests/multi/ 2>/dev/null
echo "✓ Multi tests moved to tests/multi/"

# 移动可执行文件到build
echo ""
echo "Moving executables to build/..."
mv desd build/ 2>/dev/null
mv libdeshook.so build/ 2>/dev/null
mv r1 build/ 2>/dev/null
mv r2 build/ 2>/dev/null
mv r_* build/ 2>/dev/null
mv test_* build/ 2>/dev/null
mv debug_* build/ 2>/dev/null
echo "✓ Executables moved to build/"

# 移动日志文件
echo ""
echo "Moving log files to logs/..."
mv *.log logs/ 2>/dev/null
mv *.txt logs/ 2>/dev/null
echo "✓ Log files moved to logs/"

# 移动IDE配置
echo ""
echo "Moving IDE config..."
mv launch.json .vscode/ 2>/dev/null
echo "✓ IDE config moved to .vscode/"

# Makefile保持在根目录，但需要更新
echo ""
echo "Note: Makefile remains in root (needs path updates)"

echo ""
echo "========================================="
echo "Project reorganization complete!"
echo "========================================="
echo ""
echo "New structure:"
echo "├── src/          - Source code (desd.c, libdeshook.c, common.*)"
echo "├── tests/        - Test programs"
echo "│   ├── basic/    - Basic r1/r2 tests"
echo "│   ├── bird/     - BIRD support tests"
echo "│   ├── poll/     - Poll enhancement tests"
echo "│   ├── tcp/      - TCP tests"
echo "│   ├── timeout/  - Timeout tests"
echo "│   └── multi/    - Multi-connection tests"
echo "├── scripts/      - Shell scripts"
echo "├── docs/         - Documentation"
echo "├── build/        - Compiled executables"
echo "├── logs/         - Log files"
echo "├── .vscode/      - IDE configuration"
echo "└── Makefile      - Build configuration (needs update)"
echo ""
echo "Next steps:"
echo "1. Update Makefile with new paths"
echo "2. Update scripts with new paths"
echo "3. Add .gitignore for build/ and logs/"
