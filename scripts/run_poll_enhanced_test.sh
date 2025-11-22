#!/bin/bash

# 增强poll功能测试脚本
# 测试POLLIN/POLLOUT/POLLERR等事件支持

echo "=========================================="
echo "Enhanced Poll Feature Test"
echo "Testing POLLIN/POLLOUT/POLLERR support"
echo "=========================================="

# 清理之前的socket文件
sudo rm -f /tmp/desd_control_socket /tmp/router_socket 2>/dev/null

# 启动desd
echo ""
echo "[STEP 1] Starting desd daemon..."
sudo ./desd > desd_output.log 2>&1 &
DESD_PID=$!
sleep 2

if ! ps -p $DESD_PID > /dev/null; then
    echo "ERROR: desd failed to start"
    cat desd_output.log
    exit 1
fi
echo "✓ desd started (PID: $DESD_PID)"

# 启动服务器（R2）
echo ""
echo "[STEP 2] Starting poll server (R2)..."
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r_poll_server_enhanced > r2_output.log 2>&1 &
R2_PID=$!
sleep 2

if ! ps -p $R2_PID > /dev/null; then
    echo "ERROR: R2 failed to start"
    cat r2_output.log
    sudo kill $DESD_PID 2>/dev/null
    exit 1
fi
echo "✓ R2 started (PID: $R2_PID)"

# 启动客户端（R1）
echo ""
echo "[STEP 3] Starting poll client (R1)..."
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r_poll_client_enhanced > r1_output.log 2>&1 &
R1_PID=$!

# 等待测试完成
echo ""
echo "[STEP 4] Waiting for tests to complete..."
sleep 15

# 检查进程状态
echo ""
echo "[STEP 5] Checking test results..."

# 终止进程
sudo kill $R1_PID 2>/dev/null
sudo kill $R2_PID 2>/dev/null
sudo kill $DESD_PID 2>/dev/null
sleep 1

# 显示结果
echo ""
echo "=========================================="
echo "Test Results"
echo "=========================================="

echo ""
echo "--- DESD Output ---"
tail -50 desd_output.log

echo ""
echo "--- R2 (Server) Output ---"
cat r2_output.log

echo ""
echo "--- R1 (Client) Output ---"
cat r1_output.log

# 分析结果
echo ""
echo "=========================================="
echo "Test Summary"
echo "=========================================="

SUCCESS_COUNT=0
FAIL_COUNT=0

# 检查POLLOUT测试
if grep -q "POLLOUT detected" r2_output.log && grep -q "POLLOUT detected" r1_output.log; then
    echo "✓ POLLOUT test PASSED"
    ((SUCCESS_COUNT++))
else
    echo "✗ POLLOUT test FAILED"
    ((FAIL_COUNT++))
fi

# 检查POLLIN测试
if grep -q "POLLIN detected" r2_output.log && grep -q "POLLIN detected" r1_output.log; then
    echo "✓ POLLIN test PASSED"
    ((SUCCESS_COUNT++))
else
    echo "✗ POLLIN test FAILED"
    ((FAIL_COUNT++))
fi

# 检查组合测试
if grep -q "POLLIN | POLLOUT" r2_output.log && grep -q "POLLIN | POLLOUT" r1_output.log; then
    echo "✓ Combined events test PASSED"
    ((SUCCESS_COUNT++))
else
    echo "✗ Combined events test FAILED"
    ((FAIL_COUNT++))
fi

echo ""
echo "=========================================="
echo "Final Score: $SUCCESS_COUNT passed, $FAIL_COUNT failed"
echo "=========================================="

# 清理
rm -f desd_output.log r1_output.log r2_output.log

if [ $FAIL_COUNT -eq 0 ]; then
    echo "✓ All tests PASSED!"
    exit 0
else
    echo "✗ Some tests FAILED"
    exit 1
fi
