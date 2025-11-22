#!/bin/bash
# test_readwrite.sh - 测试read/write和非阻塞socket支持

echo "========================================="
echo "Testing read/write and non-blocking socket support"
echo "========================================="

# 清理之前的测试
rm -f /tmp/desd_control_socket /tmp/router_socket
pkill -9 desd 2>/dev/null
sleep 1

# 启动desd
echo ""
echo "Step 1: Starting desd..."
./desd &
DESD_PID=$!
echo "desd started with PID: $DESD_PID"
sleep 2

# 检查desd是否运行
if ! ps -p $DESD_PID > /dev/null; then
    echo "ERROR: desd failed to start"
    exit 1
fi

echo ""
echo "Step 2: Starting test programs..."
echo "  - R1 (server) will listen and use read/write + fcntl"
echo "  - R2 (client) will connect and use read/write + fcntl"
echo ""

# 启动R1
LD_PRELOAD=./libdeshook.so ROUTER_ID=1 ./test_readwrite_r1 > r1_output.txt 2>&1 &
R1_PID=$!
echo "R1 started with PID: $R1_PID"

# 等待R1连接到desd
sleep 1

# 启动R2
LD_PRELOAD=./libdeshook.so ROUTER_ID=2 ./test_readwrite_r2 > r2_output.txt 2>&1 &
R2_PID=$!
echo "R2 started with PID: $R2_PID"

# 等待测试完成
echo ""
echo "Waiting for tests to complete (max 15 seconds)..."
COUNTER=0
while [ $COUNTER -lt 15 ]; do
    if ! ps -p $R1_PID > /dev/null && ! ps -p $R2_PID > /dev/null; then
        echo "Both test programs completed"
        break
    fi
    sleep 1
    COUNTER=$((COUNTER + 1))
    echo -n "."
done
echo ""

# 显示结果
echo ""
echo "========================================="
echo "R1 Output:"
echo "========================================="
cat r1_output.txt

echo ""
echo "========================================="
echo "R2 Output:"
echo "========================================="
cat r2_output.txt

echo ""
echo "========================================="
echo "Verification:"
echo "========================================="

# 检查关键功能
echo "Checking for key features..."

if grep -q "marked fd .* as DES-managed socket" r1_output.txt && \
   grep -q "marked fd .* as DES-managed socket" r2_output.txt; then
    echo "✓ Socket FD tracking: PASS"
else
    echo "✗ Socket FD tracking: FAIL"
fi

if grep -q "fcntl(F_SETFL)" r1_output.txt && \
   grep -q "marked fd .* as NON-BLOCKING" r1_output.txt; then
    echo "✓ fcntl non-blocking flag: PASS"
else
    echo "✗ fcntl non-blocking flag: FAIL"
fi

if grep -q "read() on socket fd=.*, forwarding to recv()" r1_output.txt && \
   grep -q "read() on socket fd=.*, forwarding to recv()" r2_output.txt; then
    echo "✓ read() to recv() forwarding: PASS"
else
    echo "✗ read() to recv() forwarding: FAIL"
fi

if grep -q "write() on socket fd=.*, forwarding to send()" r1_output.txt && \
   grep -q "write() on socket fd=.*, forwarding to send()" r2_output.txt; then
    echo "✓ write() to send() forwarding: PASS"
else
    echo "✗ write() to send() forwarding: FAIL"
fi

if grep -q "read() returned EAGAIN as expected" r1_output.txt || \
   grep -q "read() returned EAGAIN as expected" r2_output.txt; then
    echo "✓ Non-blocking EAGAIN behavior: PASS"
else
    echo "✗ Non-blocking EAGAIN behavior: FAIL (might be timing)"
fi

if grep -q "Test completed successfully" r1_output.txt && \
   grep -q "Test completed successfully" r2_output.txt; then
    echo "✓ Overall test completion: PASS"
else
    echo "✗ Overall test completion: FAIL"
fi

# 清理
echo ""
echo "Cleaning up..."
kill $DESD_PID 2>/dev/null
wait $DESD_PID 2>/dev/null

echo ""
echo "Test completed!"
echo "Detailed logs saved to r1_output.txt and r2_output.txt"
