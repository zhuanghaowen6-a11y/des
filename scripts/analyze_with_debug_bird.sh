#!/bin/bash
# 完整流程：编译调试版BIRD → 部署 → GDB分析

set -e

echo "=========================================="
echo " Step 1: 编译带调试符号的BIRD"
echo "=========================================="

cd /tmp/bird

# 清理
make clean 2>/dev/null || true

# 生成configure（如果需要）
if [ ! -f configure ]; then
    echo "生成configure..."
    autoreconf -i
fi

# 配置：关闭优化，启用调试符号
echo "配置编译选项..."
./configure \
    CFLAGS="-g -O0 -DDEBUG" \
    --enable-debug \
    --prefix=/usr \
    2>&1 | tail -10

# 编译
echo ""
echo "编译中（这可能需要几分钟）..."
make -j$(nproc) 2>&1 | tail -20

if [ ! -f bird ]; then
    echo "❌ 编译失败"
    exit 1
fi

echo ""
echo "✅ 编译成功！"
file bird
ls -lh bird

echo ""
echo "=========================================="
echo " Step 2: 启动测试环境"
echo "=========================================="

cd /home/hwzhuang/hwzhuang/desTest/des_design

# 清理旧环境
sudo pkill -9 desd 2>/dev/null || true
sudo docker stop r1 r2 2>/dev/null || true
sudo docker rm r1 r2 2>/dev/null || true

# 启动测试
timeout 60 sudo bash scripts/test_bird_uds_full.sh > /dev/null 2>&1 &
TEST_PID=$!

echo "等待容器启动..."
sleep 8

# 检查容器
if ! sudo docker ps | grep -q r1; then
    echo "❌ 容器未启动"
    sudo kill -9 $TEST_PID 2>/dev/null || true
    exit 1
fi

echo "✅ 容器运行中"

echo ""
echo "=========================================="
echo " Step 3: 部署调试版BIRD"
echo "=========================================="

# 停止原BIRD
sudo docker exec r1 pkill bird 2>/dev/null || true
sleep 2

# 部署调试版
sudo docker cp /tmp/bird/bird r1:/usr/sbin/bird-debug
echo "✅ 调试版BIRD已部署"

# 启动调试版BIRD
echo "启动带调试符号的BIRD..."
sudo docker exec -d r1 bash -c 'LD_PRELOAD=/usr/local/lib/libdeshook.so ROUTER_ID=1 /usr/sbin/bird-debug -f -c /etc/bird/bird.conf > /var/log/bird_r1_debug.log 2>&1'

sleep 3

BIRD_PID=$(sudo docker exec r1 pgrep bird-debug 2>/dev/null | tail -1)
if [ -z "$BIRD_PID" ]; then
    echo "❌ BIRD-debug未启动"
    sudo docker exec r1 cat /var/log/bird_r1_debug.log 2>/dev/null | tail -20
    sudo kill -9 $TEST_PID 2>/dev/null || true
    exit 1
fi

echo "✅ BIRD-debug运行中，PID: $BIRD_PID"

echo ""
echo "=========================================="
echo " Step 4: GDB分析clock_gettime调用栈"
echo "=========================================="

# 创建GDB脚本
cat > /tmp/analyze_clock_gettime.gdb << 'GDBEOF'
set pagination off
set confirm off

# 在clock_gettime设置断点
break clock_gettime
commands
  silent
  printf "\n==========================================\n"
  printf "🔍 clock_gettime 调用 #%d\n", ++$call_count
  printf "==========================================\n"
  
  # 显示完整调用栈（带函数名）
  printf "\n完整调用栈:\n"
  backtrace
  
  # 显示关键栈帧的详细信息
  printf "\n详细信息:\n"
  
  # 栈帧1：调用clock_gettime的函数
  printf "\n【栈帧1】直接调用者:\n"
  frame 1
  info symbol $pc
  
  # 栈帧2：上层调用者
  printf "\n【栈帧2】上层调用者:\n"
  frame 2
  info symbol $pc
  
  # 栈帧3：再上层
  printf "\n【栈帧3】再上层调用者:\n"
  frame 3
  info symbol $pc
  
  printf "\n==========================================\n"
  
  # 捕获5次后停止
  if $call_count >= 5
    printf "\n已捕获5次调用，停止追踪\n"
    quit
  end
  
  continue
end

# 初始化计数器
set $call_count = 0

# 开始运行
continue

GDBEOF

echo "设置GDB断点在clock_gettime，捕获5次调用..."
sudo docker exec r1 gdb -batch -p $BIRD_PID \
    -x /tmp/analyze_clock_gettime.gdb 2>&1 | \
    tee ../logs/gdb_debug_trace.log

echo ""
echo "=========================================="
echo " Step 5: 分析结果"
echo "=========================================="

echo ""
echo "📊 调用栈分析保存在: logs/gdb_debug_trace.log"
echo ""
echo "提取关键信息："
grep -E "调用|栈帧|in |at " ../logs/gdb_debug_trace.log | head -40

echo ""
echo "=========================================="
echo " 清理"
echo "=========================================="

sudo kill -9 $TEST_PID 2>/dev/null || true
sleep 2
sudo pkill -9 desd 2>/dev/null || true

echo "✅ 完成！"
