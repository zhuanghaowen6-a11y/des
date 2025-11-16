#!/bin/bash

echo "Cleaning up..."
echo "20040124" | sudo -S pkill -9 desd r1 r2 2>/dev/null
sleep 1
echo "20040124" | sudo -S rm -f /tmp/desd_control_socket /tmp/router_socket
sleep 1

echo "Starting desd..."
echo "20040124" | sudo -S ./desd &
DESD_PID=$!
sleep 2

echo "Starting r2 (server)..."
echo "20040124" | sudo -S ROUTER_ID=2 LD_PRELOAD=./libdeshook.so ./r2 &
R2_PID=$!
sleep 1

echo "Starting r1 (client) and sending message..."
echo -e "hello_world" | echo "20040124" | sudo -S ROUTER_ID=1 LD_PRELOAD=./libdeshook.so ./r1 &
R1_PID=$!

sleep 5

echo ""
echo "Test completed. Cleaning up..."
echo "20040124" | sudo -S pkill -9 desd r1 r2 2>/dev/null

echo "Done!"

