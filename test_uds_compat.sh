#!/bin/bash

echo "========================================"
echo "UDS Compatibility Test (验证TCP改动不影响UDS)"
echo "========================================"

# 清理
sudo rm -f /tmp/desd_control_socket /tmp/router_socket 2>/dev/null
sudo pkill -9 desd r1 r2 2>/dev/null
sleep 1

# 启动 desd
echo "Starting desd..."
sudo ./desd > /tmp/test_uds_desd.log 2>&1 &
DESD_PID=$!
sleep 2

# 启动 R2 (UDS 服务器)
echo "Starting R2 (UDS Server)..."
(echo "test message" | sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so timeout 5 ./r2) > /tmp/test_uds_r2.log 2>&1 &
R2_PID=$!
sleep 2

# 启动 R1 (UDS 客户端)
echo "Starting R1 (UDS Client)..."
(echo "Hello from R1" | sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so timeout 5 ./r1) > /tmp/test_uds_r1.log 2>&1 &
R1_PID=$!

# 等待
sleep 6

echo ""
echo "========================================"
echo "R1 Output:"
cat /tmp/test_uds_r1.log
echo ""
echo "R2 Output:"
cat /tmp/test_uds_r2.log
echo "========================================"

# 清理
sudo kill $DESD_PID $R1_PID $R2_PID 2>/dev/null
echo ""
echo "UDS compatibility test completed!"

