#!/bin/bash

# TCP测试自动化脚本

echo "========================================"
echo "DES TCP Support Test"
echo "========================================"
echo ""

# 检查是否编译了所有必要的文件
if [ ! -f "./desd" ] || [ ! -f "./libdeshook.so" ] || [ ! -f "./r1_test_tcp" ] || [ ! -f "./r2_test_tcp" ]; then
    echo "Error: Missing compiled files. Running 'make test'..."
    make test
    if [ $? -ne 0 ]; then
        echo "Error: Compilation failed."
        exit 1
    fi
fi

echo "Step 1: Cleaning up old socket files..."
sudo rm -f /tmp/desd_control_socket /tmp/router_socket
sleep 1

echo ""
echo "Step 2: Starting desd in background..."
sudo ./desd > /tmp/desd_tcp_test.log 2>&1 &
DESD_PID=$!
echo "desd started with PID: $DESD_PID"
sleep 2

# 检查desd是否成功启动
if ! kill -0 $DESD_PID 2>/dev/null; then
    echo "Error: desd failed to start!"
    cat /tmp/desd_tcp_test.log
    exit 1
fi

echo ""
echo "Step 3: Starting R2 (TCP Server) in background..."
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r2_test_tcp > /tmp/r2_tcp_test.log 2>&1 &
R2_PID=$!
echo "R2 started with PID: $R2_PID"
sleep 2

# 检查R2是否成功启动
if ! kill -0 $R2_PID 2>/dev/null; then
    echo "Error: R2 failed to start!"
    cat /tmp/r2_tcp_test.log
    sudo kill $DESD_PID 2>/dev/null
    exit 1
fi

echo ""
echo "Step 4: Starting R1 (TCP Client) in background..."
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r1_test_tcp > /tmp/r1_tcp_test.log 2>&1 &
R1_PID=$!
echo "R1 started with PID: $R1_PID"

echo ""
echo "Step 5: Waiting for test to complete (15 seconds)..."
sleep 15

echo ""
echo "========================================"
echo "Test Results"
echo "========================================"

echo ""
echo "--- R1 (TCP Client) Output ---"
cat /tmp/r1_tcp_test.log

echo ""
echo "--- R2 (TCP Server) Output ---"
cat /tmp/r2_tcp_test.log

echo ""
echo "--- DESD Output (last 50 lines) ---"
tail -n 50 /tmp/desd_tcp_test.log

echo ""
echo "========================================"
echo "Step 6: Cleaning up processes..."
sudo kill $R1_PID 2>/dev/null
sudo kill $R2_PID 2>/dev/null
sudo kill $DESD_PID 2>/dev/null
sleep 1

echo ""
echo "Test completed!"
echo "========================================"

