#!/bin/bash
# test_clock_gettime.sh - 测试clock_gettime虚拟时间功能

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

echo "=================================================="
echo "  Clock GetTime Virtual Time Test"
echo "=================================================="
echo ""

# 1. 编译测试程序
echo "[1/6] Compiling test programs..."
gcc -Wall -g -Isrc tests/clock_time/test_clock_r1.c -o build/test_clock_r1
gcc -Wall -g -Isrc tests/clock_time/test_clock_r2.c -o build/test_clock_r2
echo "✓ Test programs compiled"
echo ""

# 2. 清理旧的日志和socket
echo "[2/6] Cleaning up..."
mkdir -p logs
rm -f /tmp/desd_control_socket /tmp/router_socket 2>/dev/null || true
rm -f logs/desd.log logs/r1.log logs/r2.log 2>/dev/null || true
echo "✓ Cleaned up"
echo ""

# 3. 启动desd
echo "[3/6] Starting desd..."
sudo ./build/desd > logs/desd.log 2>&1 &
DESD_PID=$!
echo "✓ desd started (PID: $DESD_PID)"
sleep 1

# 修改socket权限以便普通用户可以连接
sudo chmod 666 /tmp/desd_control_socket 2>/dev/null || true
echo ""

# 4. 启动R1（服务器）
echo "[4/6] Starting R1 (server) with LD_PRELOAD..."
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=1 \
    ./build/test_clock_r1 > logs/r1.log 2>&1 &
R1_PID=$!
echo "✓ R1 started (PID: $R1_PID)"
sleep 2
echo ""

# 5. 启动R2（客户端）
echo "[5/6] Starting R2 (client) with LD_PRELOAD..."
LD_PRELOAD=./build/libdeshook.so ROUTER_ID=2 \
    ./build/test_clock_r2 > logs/r2.log 2>&1 &
R2_PID=$!
echo "✓ R2 started (PID: $R2_PID)"
echo ""

# 6. 等待测试完成
echo "[6/6] Waiting for tests to complete..."
sleep 5

# 检查进程状态
if ps -p $R1_PID > /dev/null 2>&1; then
    echo "⚠ R1 still running, killing..."
    kill $R1_PID 2>/dev/null || true
fi

if ps -p $R2_PID > /dev/null 2>&1; then
    echo "⚠ R2 still running, killing..."
    kill $R2_PID 2>/dev/null || true
fi

# 停止desd
echo "Stopping desd..."
sudo kill $DESD_PID 2>/dev/null || true
sleep 1
echo ""

echo "=================================================="
echo "  Test Results Analysis"
echo "=================================================="
echo ""

# 分析R1日志
echo "=== R1 (Server) Time Queries ==="
echo "--------------------------------"
if [ -f logs/r1.log ]; then
    grep "Virtual Time:" logs/r1.log | head -20
else
    echo "❌ R1 log not found"
fi
echo ""

# 分析R2日志
echo "=== R2 (Client) Time Queries ==="
echo "--------------------------------"
if [ -f logs/r2.log ]; then
    grep "Virtual Time:" logs/r2.log | head -20
else
    echo "❌ R2 log not found"
fi
echo ""

# 分析desd日志中的虚拟时间事件
echo "=== DESD Virtual Time Events ==="
echo "--------------------------------"
if [ -f logs/desd.log ]; then
    echo "GET_VIRTUAL_TIME_EVENT count:"
    grep "GET_VIRTUAL_TIME_EVENT" logs/desd.log | wc -l
    echo ""
    echo "Sample events:"
    grep "GET_VIRTUAL_TIME_EVENT" logs/desd.log | head -10
else
    echo "❌ desd log not found"
fi
echo ""

# 验证测试结果
echo "=================================================="
echo "  Verification"
echo "=================================================="
echo ""

PASS_COUNT=0
FAIL_COUNT=0

# 检查1：R1是否成功查询虚拟时间
if grep -q "Virtual Time:" logs/r1.log 2>/dev/null; then
    echo "✓ Test 1: R1 successfully queried virtual time"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "✗ Test 1: R1 failed to query virtual time"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# 检查2：R2是否成功查询虚拟时间
if grep -q "Virtual Time:" logs/r2.log 2>/dev/null; then
    echo "✓ Test 2: R2 successfully queried virtual time"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "✗ Test 2: R2 failed to query virtual time"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# 检查3：desd是否处理了GET_VIRTUAL_TIME_EVENT
if grep -q "GET_VIRTUAL_TIME_EVENT" logs/desd.log 2>/dev/null; then
    echo "✓ Test 3: desd processed GET_VIRTUAL_TIME_EVENT"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "✗ Test 3: desd did not process GET_VIRTUAL_TIME_EVENT"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# 检查4：虚拟时间是否从0开始
if grep "Virtual Time: 0\." logs/r1.log 2>/dev/null | head -1 | grep -q "0.000000"; then
    echo "✓ Test 4: Virtual time starts from 0"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "✗ Test 4: Virtual time does not start from 0"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# 检查5：虚拟时间是否推进（有不同的值）
TIME_VALUES=$(grep "Virtual Time:" logs/r1.log 2>/dev/null | awk '{print $5}' | sort -u | wc -l)
if [ "$TIME_VALUES" -gt 1 ]; then
    echo "✓ Test 5: Virtual time progresses (found $TIME_VALUES different values)"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "✗ Test 5: Virtual time does not progress"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

echo ""
echo "=================================================="
echo "  Summary"
echo "=================================================="
echo "Passed: $PASS_COUNT"
echo "Failed: $FAIL_COUNT"
echo ""

if [ $FAIL_COUNT -eq 0 ]; then
    echo "🎉 All tests PASSED!"
    echo ""
    echo "Full logs available at:"
    echo "  - logs/desd.log"
    echo "  - logs/r1.log"
    echo "  - logs/r2.log"
    exit 0
else
    echo "❌ Some tests FAILED"
    echo ""
    echo "Check logs for details:"
    echo "  - logs/desd.log"
    echo "  - logs/r1.log"
    echo "  - logs/r2.log"
    exit 1
fi
