#!/bin/bash
# 编译带调试符号的BIRD - 这样gdb就能显示函数名和源代码

set -e

echo "=========================================="
echo " 编译带调试符号的BIRD"
echo "=========================================="
echo ""

cd /tmp

# 1. 下载BIRD源代码
echo "[INFO] 下载BIRD源代码..."
rm -rf bird-build
mkdir -p bird-build
cd bird-build

wget -q https://bird.network.cz/download/bird-2.0.8.tar.gz
tar -xzf bird-2.0.8.tar.gz
cd bird-2.0.8

# 2. 配置编译选项（带调试符号）
echo ""
echo "[INFO] 配置BIRD（启用调试符号）..."
./configure --prefix=/tmp/bird-install CFLAGS="-g -O0" LDFLAGS="-g"

# 3. 编译
echo ""
echo "[INFO] 编译BIRD（这可能需要几分钟）..."
make -j$(nproc)

# 4. 安装到临时目录
echo ""
echo "[INFO] 安装到 /tmp/bird-install..."
make install

echo ""
echo "=========================================="
echo " 编译完成！"
echo "=========================================="
echo ""
echo "带调试符号的BIRD位于: /tmp/bird-install/sbin/bird"
echo ""
echo "如何使用："
echo ""
echo "1. 替换Docker镜像中的BIRD："
echo "   sudo docker cp /tmp/bird-install/sbin/bird r1:/usr/sbin/bird"
echo ""
echo "2. 重新运行GDB追踪，现在会显示函数名和源文件："
echo "   sudo docker exec r1 gdb -p \$(sudo docker exec r1 pidof bird)"
echo ""
echo "3. 在GDB中设置断点："
echo "   (gdb) break clock_gettime"
echo "   (gdb) continue"
echo "   (gdb) backtrace  # 现在会显示函数名！"
echo ""
echo "示例输出："
echo "  #0  clock_gettime() at libdeshook.c:1406"
echo "  #1  ev_run() at sysdep/unix/io.c:123          ← 有函数名和文件！"
echo "  #2  io_loop() at sysdep/unix/io.c:456         ← 有函数名和文件！"
