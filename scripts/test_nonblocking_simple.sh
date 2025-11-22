#!/bin/bash
# 简单的非阻塞测试（不使用poll）

cd "$(dirname "$0")/.." || exit 1

echo "========================================="
echo "Simple Non-Blocking Test (no poll)"
echo "========================================="

# 编译
gcc -Wall -g -Isrc tests/bird/test_nonblock_simple_r1.c -o build/test_nonblock_simple_r1 || exit 1
gcc -Wall -g -Isrc tests/bird/test_nonblock_simple_r2.c -o build/test_nonblock_simple_r2 || exit 1
echo "✓ Compiled"

# 清理旧日志
mkdir -p logs
rm -f logs/*.log

# 启动desd
./build/desd > logs/desd.log 2>&1 &
DESD_PID=$!
sleep 1
echo "✓ desd started (PID: $DESD_PID)"

# 运行测试
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 ./build/test_nonblock_simple_r1 > logs/r1.log 2>&1 &
R1_PID=$!

LD_PRELOAD=./build/libdeshook.so ROUTER_ID=2 ./build/test_nonblock_simple_r2 > logs/r2.log 2>&1 &
R2_PID=$!

echo "✓ Tests started (R1: $R1_PID, R2: $R2_PID)"
echo ""

# 等待完成（最多10秒）
for i in {1..10}; do
    if ! ps -p $R1_PID > /dev/null 2>&1 && ! ps -p $R2_PID > /dev/null 2>&1; then
        echo "✓ Tests completed"
        break
    fi
    sleep 1
done

# 显示结果
echo ""
echo "========================================="
echo "R1 Output:"
echo "========================================="
cat logs/r1.log

echo ""
echo "========================================="
echo "R2 Output:"
echo "========================================="
cat logs/r2.log

# 验证
echo ""
echo "========================================="
echo "Verification:"
echo "========================================="
SUCCESS=0

if grep -q "✓ Test 1 PASSED" logs/r1.log; then
    echo "✓ Test 1: Non-blocking EAGAIN when no data"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ Test 1 FAILED"
fi

if grep -q "✓ Test 2 PASSED" logs/r1.log; then
    echo "✓ Test 2: Non-blocking read succeeds with data"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ Test 2 FAILED"
fi

if grep -q "✓ Test 3 PASSED" logs/r1.log; then
    echo "✓ Test 3: Non-blocking EAGAIN after reading all data"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ Test 3 FAILED"
fi

if grep -q "recv() on non-blocking socket, no data available, returning EAGAIN" logs/desd.log; then
    echo "✓ DESD: Correctly handles non-blocking recv"
    SUCCESS=$((SUCCESS + 1))
else
    echo "✗ DESD handling NOT detected"
fi

# 清理
kill $DESD_PID 2>/dev/null

echo ""
echo "========================================="
echo "Result: $SUCCESS/4 tests passed"
echo "========================================="

if [ $SUCCESS -eq 4 ]; then
    echo "✓✓✓ ALL TESTS PASSED ✓✓✓"
    exit 0
else
    echo "✗✗✗ SOME TESTS FAILED ✗✗✗"
    exit 1
fi
