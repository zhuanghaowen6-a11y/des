#!/bin/bash
# test_rw_final.sh - 正确的测试脚本（确保2个路由器同时启动）

echo "========================================="
echo "Final read/write test"
echo "========================================="

# 清理
pkill -9 desd test_rw_basic 2>/dev/null
sudo rm -f /tmp/desd_control_socket /tmp/router_socket 2>/dev/null
sleep 1

# 启动desd
echo "Starting desd..."
./desd > desd_test.log 2>&1 &
DESD_PID=$!
sleep 2

# 同时启动R1和R2（确保desd不会卡在等待）
echo "Starting R1 and R2 simultaneously..."
LD_PRELOAD=./libdeshook.so ROUTER_ID=1 timeout 10 ./test_rw_basic_r1 > r1_test.log 2>&1 &
R1_PID=$!
sleep 0.5

LD_PRELOAD=./libdeshook.so ROUTER_ID=2 timeout 10 ./test_rw_basic_r2 > r2_test.log 2>&1 &
R2_PID=$!

# 等待测试完成
echo "Waiting for tests to complete..."
wait $R1_PID 2>/dev/null
R1_EXIT=$?
wait $R2_PID 2>/dev/null
R2_EXIT=$?

# 显示结果
echo ""
echo "========================================="
echo "R1 Output:"
echo "========================================="
cat r1_test.log

echo ""
echo "========================================="
echo "R2 Output:"
echo "========================================="
cat r2_test.log

echo ""
echo "========================================="
echo "DESD Output (last 30 lines):"
echo "========================================="
tail -30 desd_test.log

echo ""
echo "========================================="
echo "Exit Codes:"
echo "========================================="
echo "R1: $R1_EXIT (0=success, 124=timeout)"
echo "R2: $R2_EXIT (0=success, 124=timeout)"

# 检查结果
echo ""
echo "========================================="
echo "Verification:"
echo "========================================="

if grep -q "write() returned" r1_test.log && grep -q "read() returned.*bytes: 'Hello" r1_test.log; then
    echo "✓ R1: write/read successful"
else
    echo "✗ R1: write/read failed"
fi

if grep -q "write() returned" r2_test.log && grep -q "read() returned.*bytes: 'Hello" r2_test.log; then
    echo "✓ R2: write/read successful"
else
    echo "✗ R2: write/read failed"
fi

if grep -q "forwarding to recv()" r1_test.log || grep -q "forwarding to recv()" r2_test.log; then
    echo "✓ read() forwarding detected"
else
    echo "✗ read() forwarding NOT detected"
fi

if grep -q "forwarding to send()" r1_test.log || grep -q "forwarding to send()" r2_test.log; then
    echo "✓ write() forwarding detected"
else
    echo "✗ write() forwarding NOT detected"
fi

# 清理
kill $DESD_PID 2>/dev/null
sleep 1

echo ""
echo "Test completed!"
