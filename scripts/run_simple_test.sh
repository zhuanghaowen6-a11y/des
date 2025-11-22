#!/bin/bash
# run_simple_test.sh - 运行简化版read/write测试

echo "========================================="
echo "Simple read/write test"
echo "========================================="

# 清理
pkill -9 desd 2>/dev/null
sudo rm -f /tmp/desd_control_socket /tmp/router_socket 2>/dev/null
sleep 1

# 启动desd
echo "Starting desd..."
./desd &
DESD_PID=$!
sleep 2

# 启动测试
echo "Starting test programs..."
LD_PRELOAD=./libdeshook.so ROUTER_ID=1 ./test_simple_rw_r1 &
R1_PID=$!
sleep 1

LD_PRELOAD=./libdeshook.so ROUTER_ID=2 ./test_simple_rw_r2 &
R2_PID=$!

# 等待完成
wait $R1_PID
wait $R2_PID

# 清理
kill $DESD_PID 2>/dev/null
echo "Test completed!"
