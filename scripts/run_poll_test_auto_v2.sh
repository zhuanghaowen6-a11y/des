#!/bin/bash

# poll() 精确 FD 匹配自动化测试脚本 v2
# 简化版：只监听一个地址，接受 3 个连接

echo "=========================================="
echo "poll() 精确 FD 匹配自动化测试 v2"
echo "=========================================="
echo ""

# 清理旧的 socket 文件
rm -f /tmp/desd_control_socket
rm -f /tmp/poll_test_socket

# 检查是否以 root 运行
if [ "$EUID" -ne 0 ]; then 
    echo "错误：此脚本需要 root 权限"
    echo "请使用: sudo $0"
    exit 1
fi

# 检查程序是否存在
if [ ! -f "./desd" ] || [ ! -f "./libdeshook.so" ] || [ ! -f "./r_poll_server_test_v2" ] || [ ! -f "./r_poll_client_test_v2" ]; then
    echo "错误：缺少必要的程序文件"
    echo "请先运行: make all && gcc -o r_poll_server_test_v2 r_poll_server_test_v2.c && gcc -o r_poll_client_test_v2 r_poll_client_test_v2.c"
    exit 1
fi

# 启动 desd
echo "[自动测试] 启动 desd..."
./desd > desd_poll_test_v2.log 2>&1 &
DESD_PID=$!
echo "[自动测试] desd PID: $DESD_PID"

# 等待 desd 启动
sleep 1

# 启动服务端 (R1)
echo "[自动测试] 启动服务端 (R1)..."
ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r_poll_server_test_v2 > server_poll_test_v2.log 2>&1 &
SERVER_PID=$!
echo "[自动测试] 服务端 PID: $SERVER_PID"

# 等待服务端启动
sleep 2

# 启动客户端 (R2)
echo "[自动测试] 启动客户端 (R2)..."
ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r_poll_client_test_v2 > client_poll_test_v2.log 2>&1 &
CLIENT_PID=$!
echo "[自动测试] 客户端 PID: $CLIENT_PID"

# 等待测试完成
echo ""
echo "[自动测试] 等待测试完成（约 10 秒）..."
sleep 10

# 显示结果
echo ""
echo "=========================================="
echo "测试结果"
echo "=========================================="
echo ""

echo "=== 服务端输出 ==="
cat server_poll_test_v2.log
echo ""

echo "=== 客户端输出 ==="
cat client_poll_test_v2.log
echo ""

echo "=== desd 输出（关键部分）==="
grep -E "(select.*unblocked|unique FDs|poll|ready|pending packet)" desd_poll_test_v2.log | tail -15
echo ""

# 检查测试是否通过
if grep -q "测试通过" server_poll_test_v2.log; then
    echo "=========================================="
    echo "✓✓✓ 测试通过！"
    echo "=========================================="
    TEST_RESULT=0
else
    echo "=========================================="
    echo "✗✗✗ 测试失败！"
    echo "=========================================="
    TEST_RESULT=1
fi

# 清理进程
echo ""
echo "[自动测试] 清理进程..."
kill $CLIENT_PID 2>/dev/null
kill $SERVER_PID 2>/dev/null
kill $DESD_PID 2>/dev/null

# 等待进程结束
sleep 1

echo "[自动测试] 日志文件:"
echo "  - desd: desd_poll_test_v2.log"
echo "  - 服务端: server_poll_test_v2.log"
echo "  - 客户端: client_poll_test_v2.log"
echo ""

exit $TEST_RESULT

