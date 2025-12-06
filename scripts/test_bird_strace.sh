#!/bin/bash

# 测试脚本：使用strace捕获BIRD 3的系统调用，验证是否调用accept()
# 这是在非DES环境下的测试

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOGS_DIR="$PROJECT_ROOT/logs"

# 创建日志目录
mkdir -p "$LOGS_DIR"

echo "======================================"
echo "BIRD 3 strace测试（非DES环境）"
echo "======================================"
echo ""

# 清理旧容器
echo "[1] 清理旧容器..."
sudo docker rm -f bird_strace_r1 bird_strace_r2 2>/dev/null || true
sleep 1

# 创建BIRD配置文件
echo "[2] 创建BIRD配置..."

# R1配置
cat > /tmp/bird_r1_strace.conf << 'EOF'
log stderr all;

router id 192.168.1.1;

protocol device {
}

protocol kernel {
    ipv4 {
        export all;
    };
}

protocol static {
    ipv4;
    route 192.168.1.0/24 blackhole;
}

protocol direct {
    ipv4;
}

protocol bgp r2 {
    local 192.168.1.1 as 65001;
    neighbor 192.168.2.1 as 65002;
    multihop;
    
    ipv4 {
        import all;
        export all;
    };
}
EOF

# R2配置
cat > /tmp/bird_r2_strace.conf << 'EOF'
log stderr all;

router id 192.168.2.1;

protocol device {
}

protocol kernel {
    ipv4 {
        export all;
    };
}

protocol static {
    ipv4;
    route 192.168.2.0/24 blackhole;
}

protocol direct {
    ipv4;
}

protocol bgp r1 {
    local 192.168.2.1 as 65002;
    neighbor 192.168.1.1 as 65001;
    multihop;
    
    ipv4 {
        import all;
        export all;
    };
}
EOF

echo "[3] 启动R1容器..."
sudo docker run -d \
    --name bird_strace_r1 \
    --hostname r1 \
    --cap-add=NET_ADMIN \
    --cap-add=SYS_PTRACE \
    bird:latest \
    tail -f /dev/null

echo "[4] 启动R2容器..."
sudo docker run -d \
    --name bird_strace_r2 \
    --hostname r2 \
    --cap-add=NET_ADMIN \
    --cap-add=SYS_PTRACE \
    bird:latest \
    tail -f /dev/null

sleep 2

echo "[5] 配置网络..."
# 获取容器在docker bridge上的IP
R1_IP=$(sudo docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' bird_strace_r1)
R2_IP=$(sudo docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' bird_strace_r2)

echo "  R1容器IP: $R1_IP"
echo "  R2容器IP: $R2_IP"

# 在容器内添加虚拟IP（用于BGP）
sudo docker exec bird_strace_r1 ip addr add 192.168.1.1/32 dev lo || true
sudo docker exec bird_strace_r2 ip addr add 192.168.2.1/32 dev lo || true

# 添加到对端的路由
sudo docker exec bird_strace_r1 ip route add 192.168.2.1/32 via $R2_IP || true
sudo docker exec bird_strace_r2 ip route add 192.168.1.1/32 via $R1_IP || true

echo "[6] 复制配置文件到容器..."
sudo docker cp /tmp/bird_r1_strace.conf bird_strace_r1:/etc/bird.conf
sudo docker cp /tmp/bird_r2_strace.conf bird_strace_r2:/etc/bird.conf

echo "[7] 启动BIRD并使用strace捕获系统调用..."
echo "  启动R1 BIRD (带strace)..."
sudo docker exec -d bird_strace_r1 \
    strace -f -e trace=socket,bind,listen,accept,accept4,connect,send,recv,poll,epoll_wait -o /var/log/bird_r1_strace.log \
    bird -c /etc/bird.conf -d 2>&1 | tee /var/log/bird_r1.log &

sleep 2

echo "  启动R2 BIRD (带strace)..."
sudo docker exec -d bird_strace_r2 \
    strace -f -e trace=socket,bind,listen,accept,accept4,connect,send,recv,poll,epoll_wait -o /var/log/bird_r2_strace.log \
    bird -c /etc/bird.conf -d 2>&1 | tee /var/log/bird_r2.log &

echo ""
echo "[8] 等待30秒让BGP建立连接..."
for i in {30..1}; do
    echo -ne "\r  剩余 $i 秒...   "
    sleep 1
done
echo ""

echo "[9] 复制strace日志..."
sudo docker cp bird_strace_r1:/var/log/bird_r1_strace.log "$LOGS_DIR/bird_r1_strace.log" || echo "警告：无法复制R1 strace日志"
sudo docker cp bird_strace_r2:/var/log/bird_r2_strace.log "$LOGS_DIR/bird_r2_strace.log" || echo "警告：无法复制R2 strace日志"

echo "[10] 检查strace日志中的accept调用..."
echo ""
echo "======================================"
echo "R1的accept调用："
echo "======================================"
grep -i "accept" "$LOGS_DIR/bird_r1_strace.log" || echo "未找到accept调用"

echo ""
echo "======================================"
echo "R2的accept调用："
echo "======================================"
grep -i "accept" "$LOGS_DIR/bird_r2_strace.log" || echo "未找到accept调用"

echo ""
echo "======================================"
echo "R1的connect调用："
echo "======================================"
grep "connect(" "$LOGS_DIR/bird_r1_strace.log" | head -20

echo ""
echo "======================================"
echo "R2的connect调用："
echo "======================================"
grep "connect(" "$LOGS_DIR/bird_r2_strace.log" | head -20

echo ""
echo "======================================"
echo "完整的strace日志已保存到："
echo "  $LOGS_DIR/bird_r1_strace.log"
echo "  $LOGS_DIR/bird_r2_strace.log"
echo ""
echo "查看BGP状态："
echo "  sudo docker exec bird_strace_r1 birdc show protocols"
echo "  sudo docker exec bird_strace_r2 birdc show protocols"
echo ""
echo "清理容器："
echo "  sudo docker rm -f bird_strace_r1 bird_strace_r2"
echo "======================================"
