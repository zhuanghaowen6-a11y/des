#!/bin/bash
# test_nonblocking.sh - 测试非阻塞socket支持

# 切换到项目根目录
cd "$(dirname "$0")/.." || exit 1

echo "========================================="
echo "Non-Blocking Socket Support Test"
echo "========================================="
echo ""

# 清理旧日志
mkdir -p logs
rm -f logs/desd.log logs/r1.log logs/r2.log

# 编译测试程序
echo "Step 1: Compiling test programs..."
gcc -Wall -g -Isrc tests/bird/test_nonblocking_r1.c -o build/test_nonblocking_r1
gcc -Wall -g -Isrc tests/bird/test_nonblocking_r2.c -o build/test_nonblocking_r2

if [ ! -f build/test_nonblocking_r1 ] || [ ! -f build/test_nonblocking_r2 ]; then
    echo "✗ Compilation failed"
    exit 1
fi
echo "✓ Compilation successful"
echo ""

# 启动desd
echo "Step 2: Starting desd..."
./build/desd > logs/desd.log 2>&1 &
DESD_PID=$!
echo "  desd PID: $DESD_PID"
sleep 1

# 检查desd是否运行
if ! kill -0 $DESD_PID 2>/dev/null; then
    echo "✗ desd failed to start"
    cat logs/desd.log
    exit 1
fi
echo ""

# 启动测试程序
echo "Step 3: Starting test programs..."
echo "  WARNING: Do NOT use 'timeout' command with LD_PRELOAD!"
echo "  R1 (server) will test non-blocking read() behavior"
echo "  R2 (client) will send data after delay"
echo ""

# 启动R1（服务端）
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 ./build/test_nonblocking_r1 > logs/r1.log 2>&1 &
R1_PID=$!
echo "  R1 PID: $R1_PID"

# 启动R2（客户端）
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=2 ./build/test_nonblocking_r2 > logs/r2.log 2>&1 &
R2_PID=$!
echo "  R2 PID: $R2_PID"
echo ""

# 等待测试完成
echo "Step 4: Waiting for tests to complete..."
wait $R1_PID 2>/dev/null
wait $R2_PID 2>/dev/null
echo ""

# 显示输出
echo "========================================="
echo "Test Results:"
echo "========================================="
echo ""

echo "--- R1 Output (Server) ---"
cat logs/r1.log
echo ""

echo "--- R2 Output (Client) ---"
cat logs/r2.log
echo ""

# 验证测试结果
echo "========================================="
echo "Verification:"
echo "========================================="

SUCCESS=0

# 检查非阻塞EAGAIN（无数据时）
if grep -q "correctly returned EAGAIN (no data available)" logs/r1.log; then
    echo "✓ Test 1: Non-blocking read() returns EAGAIN when no data"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ Test 1: Non-blocking read() EAGAIN test FAILED"
fi

# 检查非阻塞读取成功（有数据时）
if grep -q "read() success, received .* bytes: 'Hello from R2'" logs/r1.log; then
    echo "✓ Test 2: Non-blocking read() succeeds when data available"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ Test 2: Non-blocking read() with data FAILED"
fi

# 检查读完后再次EAGAIN
if grep -q "correctly returned EAGAIN (no more data)" logs/r1.log; then
    echo "✓ Test 3: Non-blocking read() returns EAGAIN after reading all data"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ Test 3: Second EAGAIN test FAILED"
fi

# 检查非阻塞模式切换
if grep -q "Switched back to blocking mode" logs/r1.log; then
    echo "✓ Test 4: Switch between blocking/non-blocking modes works"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ Test 4: Mode switching test FAILED"
fi

# 检查desd日志中的非阻塞处理
if grep -q "recv() on non-blocking socket, no data available, returning EAGAIN" logs/desd.log; then
    echo "✓ DESD: Correctly handles non-blocking recv() requests"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ DESD: Non-blocking handling NOT detected in desd.log"
fi

# 清理
kill $DESD_PID 2>/dev/null
wait $DESD_PID 2>/dev/null

echo ""
echo "========================================="
echo "Summary: $SUCCESS/5 checks passed"
echo "========================================="

if [ $SUCCESS -eq 5 ]; then
    echo "✓✓✓ All tests PASSED! ✓✓✓"
    echo ""
    echo "Non-blocking socket support is working correctly:"
    echo "  - read() returns EAGAIN when no data (non-blocking)"
    echo "  - read() succeeds when data is available"
    echo "  - fcntl() F_GETFL/F_SETFL correctly track mode"
    echo "  - desd correctly handles nonblocking flag"
    exit 0
else
    echo "✗✗✗ Some tests FAILED ✗✗✗"
    echo "Check logs/r1.log, logs/r2.log, and logs/desd.log for details"
    exit 1
fi
