#!/bin/bash
# test_bird_support.sh - 测试BIRD支持功能（read/write/fcntl）

# 切换到项目根目录
cd "$(dirname "$0")/.." || exit 1

echo "========================================="
echo "BIRD Support Test (read/write/fcntl)"
echo "========================================="

# 创建logs目录
mkdir -p logs

# 清理
pkill -9 desd 2>/dev/null
sudo rm -f /tmp/desd_control_socket /tmp/router_socket 2>/dev/null
sleep 1

# 启动desd
echo ""
echo "Step 1: Starting desd..."
./build/desd > logs/desd.log 2>&1 &
DESD_PID=$!
echo "  desd PID: $DESD_PID"
sleep 2

# 检查desd是否运行
if ! ps -p $DESD_PID > /dev/null; then
    echo "ERROR: desd failed to start"
    cat logs/desd.log
    exit 1
fi

# 启动测试程序（不使用timeout，避免LD_PRELOAD问题）
echo ""
echo "Step 2: Starting test programs..."
echo "  WARNING: Do NOT use 'timeout' command with LD_PRELOAD!"
echo "  R1 (server) will use read/write for I/O"
echo "  R2 (client) will use read/write for I/O"
echo ""

LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 ./build/test_rw_basic_r1 > logs/r1.log 2>&1 &
R1_PID=$!
echo "  R1 PID: $R1_PID"

LD_PRELOAD=./build/libdeshook.so ROUTER_ID=2 ./build/test_rw_basic_r2 > logs/r2.log 2>&1 &
R2_PID=$!
echo "  R2 PID: $R2_PID"

# 等待测试完成
echo ""
echo "Step 3: Waiting for tests to complete..."
sleep 6

# 检查进程状态
if ps -p $R1_PID > /dev/null; then
    echo "  R1 still running, sending SIGTERM..."
    kill $R1_PID 2>/dev/null
fi

if ps -p $R2_PID > /dev/null; then
    echo "  R2 still running, sending SIGTERM..."
    kill $R2_PID 2>/dev/null
fi

wait $R1_PID 2>/dev/null
wait $R2_PID 2>/dev/null

# 显示结果
echo ""
echo "========================================="
echo "Test Results:"
echo "========================================="

echo ""
echo "--- R1 Output (last 20 lines) ---"
tail -20 logs/r1.log

echo ""
echo "--- R2 Output (last 20 lines) ---"
tail -20 logs/r2.log

# 验证
echo ""
echo "========================================="
echo "Verification:"
echo "========================================="

SUCCESS=0

if grep -q "write() on socket.*forwarding to send()" logs/r1.log logs/r2.log; then
    echo "✓ write() interception working"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ write() interception NOT detected"
fi

if grep -q "read() on socket.*forwarding to recv()" logs/r1.log logs/r2.log; then
    echo "✓ read() interception working"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ read() interception NOT detected"
fi

if grep -q "marked.*as DES-managed socket" logs/r1.log logs/r2.log; then
    echo "✓ Socket FD tracking working"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ Socket FD tracking NOT detected"
fi

if grep -q "marked accepted fd.*as DES-managed socket" logs/r1.log; then
    echo "✓ accept() socket marking working"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ accept() socket marking NOT detected"
fi

if grep -q "fcntl(F_GETFL)" logs/r1.log logs/r2.log; then
    echo "✓ fcntl() F_GETFL working"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ fcntl() F_GETFL NOT detected"
fi

if grep -q "fcntl(F_SETFL" logs/r1.log logs/r2.log; then
    echo "✓ fcntl() F_SETFL working"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ fcntl() F_SETFL NOT detected"
fi

if grep -q "clearing tracking for fd" logs/r1.log logs/r2.log; then
    echo "✓ FD cleanup working"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ FD cleanup NOT detected"
fi

if grep -q "Test completed!" logs/r1.log && grep -q "Test completed!" logs/r2.log; then
    echo "✓ Both tests completed successfully"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ One or both tests did not complete"
fi

# 清理
kill $DESD_PID 2>/dev/null
wait $DESD_PID 2>/dev/null

echo ""
echo "========================================="
echo "Summary: $SUCCESS/8 checks passed"
echo "========================================="

if [ $SUCCESS -eq 8 ]; then
    echo "✓✓✓ All tests PASSED! ✓✓✓"
    echo ""
    echo "BIRD support features are working:"
    echo "  - read()/write() system call interception"
    echo "  - fcntl() F_GETFL/F_SETFL interception"
    echo "  - Socket FD tracking and cleanup"
    echo "  - accept() socket marking"
    exit 0
else
    echo "✗✗✗ Some tests FAILED ✗✗✗"
    echo "Check logs/r1.log, logs/r2.log, and logs/desd.log for details"
    exit 1
fi
