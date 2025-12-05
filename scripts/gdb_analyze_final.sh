#!/bin/bash
# 一键完成：启动环境 → 部署调试版BIRD → GDB分析

set -e

cd /home/hwzhuang/hwzhuang/desTest/des_design

echo "=========================================="
echo " 清理并启动测试环境"
echo "=========================================="

# 完全清理
sudo pkill -9 desd 2>/dev/null || true
sudo docker stop r1 r2 2>/dev/null || true  
sudo docker rm r1 r2 2>/dev/null || true
sudo rm -f /tmp/desd_control_socket
sleep 2

# 启动desd
./build/desd > logs/desd.log 2>&1 &
DESD_PID=$!
sleep 3

if ! pgrep -x desd > /dev/null; then
    echo "❌ desd启动失败"
    cat logs/desd.log
    exit 1
fi

sudo chmod 666 /tmp/desd_control_socket
echo "✅ desd运行中 (PID: $DESD_PID)"

# 创建网络
sudo docker network create --subnet=10.0.0.0/16 bird_test_net 2>/dev/null || true

# 创建容器  
sudo docker run -d --name r1 --network bird_test_net --ip 10.0.1.1 \
    -v /tmp:/tmp bird:latest tail -f /dev/null
sudo docker run -d --name r2 --network bird_test_net --ip 10.0.2.2 \
    -v /tmp:/tmp bird:latest tail -f /dev/null

echo "✅ 容器已创建"

# 部署libdeshook和配置
sudo docker cp build/libdeshook.so r1:/usr/local/lib/
sudo docker cp build/libdeshook.so r2:/usr/local/lib/

# 创建BIRD配置
cat > /tmp/bird_r1.conf << 'BIREOF'
router id 10.0.1.1;

protocol device {}
protocol kernel { ipv4 { export all; }; }

protocol bgp {
    local as 65001;
    neighbor 10.0.2.2 as 65002;
    ipv4 { import all; export all; };
}
BIREOF

sudo docker cp /tmp/bird_r1.conf r1:/etc/bird/bird.conf

echo "=========================================="
echo " 部署并启动调试版BIRD"
echo "=========================================="

# 复制调试版BIRD
sudo docker cp /tmp/bird/bird r1:/usr/sbin/bird-debug
echo "✅ 调试版BIRD已部署"

# 启动BIRD  
sudo docker exec -d r1 bash -c 'LD_PRELOAD=/usr/local/lib/libdeshook.so ROUTER_ID=1 /usr/sbin/bird-debug -f -c /etc/bird/bird.conf > /var/log/bird_debug.log 2>&1'

sleep 5

BIRD_PID=$(sudo docker exec r1 pgrep bird-debug 2>/dev/null | tail -1)
if [ -z "$BIRD_PID" ]; then
    echo "❌ BIRD未启动"
    sudo docker exec r1 cat /var/log/bird_debug.log
    exit 1
fi

echo "✅ BIRD-debug运行中 (PID: $BIRD_PID)"

echo ""
echo "=========================================="
echo " GDB分析clock_gettime调用栈"
echo "=========================================="

# 创建GDB脚本
cat > /tmp/gdb_trace.gdb << 'GDBEOF'
set pagination off
set confirm off

set $count = 0

break clock_gettime
commands
  silent
  set $count = $count + 1
  
  printf "\n========================================== 调用 #%d ==========================================\n", $count
  
  # 显示完整调用栈
  printf "\n📍 完整调用栈:\n"
  backtrace 10
  
  printf "\n"
  
  # 显示每个栈帧的详细信息
  printf "【栈帧0】clock_gettime\n"
  frame 0
  info args
  
  printf "\n【栈帧1】\n"
  frame 1
  info symbol $pc
  info args
  
  printf "\n【栈帧2】\n"
  frame 2  
  info symbol $pc
  info args
  
  printf "\n【栈帧3】\n"
  frame 3
  info symbol $pc
  
  printf "\n========================================== END #%d ==========================================\n\n", $count
  
  if $count >= 5
    printf "\n✅ 已捕获5次调用，分析完成\n\n"
    quit
  end
  
  continue
end

continue

GDBEOF

echo "开始GDB追踪（捕获5次clock_gettime调用）..."
sudo docker exec r1 gdb -batch -p $BIRD_PID -x /tmp/gdb_trace.gdb 2>&1 | tee logs/gdb_trace_with_symbols.log

echo ""
echo "=========================================="
echo " 分析结果"
echo "=========================================="

echo ""
echo "📊 完整日志保存在: logs/gdb_trace_with_symbols.log"
echo ""
echo "提取调用栈模式..."
grep -E "^#[0-9]|in |at " logs/gdb_trace_with_symbols.log | head -60

echo ""
echo "清理环境..."
sudo pkill -9 desd 2>/dev/null || true
sudo docker stop r1 r2 2>/dev/null || true
sudo docker rm r1 r2 2>/dev/null || true

echo ""
echo "✅ 分析完成！请查看 logs/gdb_trace_with_symbols.log"
