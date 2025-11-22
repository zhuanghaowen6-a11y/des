#!/bin/bash

echo "清理环境..."
sudo pkill -9 -f 'desd|r_poll' 2>/dev/null
sudo rm -f /tmp/desd_control_socket /tmp/router_socket
rm -f desd.log r1.log r2.log

echo "启动desd..."
sudo ./desd > desd.log 2>&1 &
DESD_PID=$!
sleep 2

echo "启动R2..."
sudo ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r_poll_server_enhanced > r2.log 2>&1 &
R2_PID=$!
sleep 2

echo "启动R1..."
sudo ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r_poll_client_enhanced > r1.log 2>&1 &
R1_PID=$!

echo "等待测试完成..."
sleep 15

echo ""
echo "========== R2 输出 =========="
cat r2.log

echo ""
echo "========== R1 输出 =========="
cat r1.log

echo ""
echo "========== DESD 最后50行 =========="
tail -50 desd.log

# 清理
sudo pkill -9 -f 'desd|r_poll' 2>/dev/null

echo ""
echo "测试完成"
