#!/bin/bash

# 清理旧进程和文件
echo "20040124" | sudo -S pkill -9 desd 2>/dev/null
echo "20040124" | sudo -S pkill -9 r1 2>/dev/null
echo "20040124" | sudo -S pkill -9 r2 2>/dev/null
sleep 1
echo "20040124" | sudo -S rm -f /tmp/desd_control_socket /tmp/router_socket
sleep 1

# 启动 desd
echo "Starting desd..."
echo "20040124" | sudo -S ./desd > /tmp/desd_r1r2_test.log 2>&1 &
DESD_PID=$!
sleep 2

# 启动 r2
echo "Starting r2..."
echo "20040124" | sudo -S ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r2 > /tmp/r2_r1r2_test.log 2>&1 &
R2_PID=$!
sleep 1

# 启动 r1 并发送消息
echo "Starting r1..."
echo "test_message" | sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r1 > /tmp/r1_r1r2_test.log 2>&1 &
R1_PID=$!

# 等待测试完成
sleep 5

# 输出结果
echo ""
echo "====== DESD Output ======"
cat /tmp/desd_r1r2_test.log | tail -30
echo ""
echo "====== R1 Output ======"
cat /tmp/r1_r1r2_test.log
echo ""
echo "====== R2 Output ======"
cat /tmp/r2_r1r2_test.log

# 清理
echo ""
echo "Cleaning up..."
echo "20040124" | sudo -S pkill -9 desd 2>/dev/null
echo "20040124" | sudo -S pkill -9 r1 2>/dev/null
echo "20040124" | sudo -S pkill -9 r2 2>/dev/null

echo "Test completed!"

