#!/bin/bash
# N-Router BIRD + Docker + DES 测试脚本
# 基于test_bird_uds_full.sh扩展到N个路由器

set -e

KEEP_ENV=${KEEP_ENV:-0}

PROJECT_ROOT="/home/hwzhuang/hwzhuang/desTest/des_design"
cd "$PROJECT_ROOT"

# 默认参数
NUM_ROUTERS=${1:-5}
TEST_DURATION=${2:-60}

if [ "$NUM_ROUTERS" -lt 2 ] || [ "$NUM_ROUTERS" -gt 100 ]; then
    echo "[ERROR] NUM_ROUTERS must be between 2 and 100"
    exit 1
fi

echo "=========================================="
echo "N-Router BGP Test with Docker"
echo "Routers: $NUM_ROUTERS"
echo "Test Duration: ${TEST_DURATION}s"
echo "=========================================="

# 日志函数
log_info() {
    echo "[INFO] $1"
}

log_error() {
    echo "[ERROR] $1"
}

log_step() {
    echo ""
    echo "=========================================="
    echo " $1"
    echo "=========================================="
}

# 清理函数
cleanup() {
    log_step "清理环境"
    
    # 停止所有容器
    for i in $(seq 1 $NUM_ROUTERS); do
        sudo docker stop r$i 2>/dev/null || true
        sudo docker rm r$i 2>/dev/null || true
    done
    log_info "容器已清理"
    
    # 停止desd
    if [ -f /tmp/desd.pid ]; then
        sudo kill $(cat /tmp/desd.pid) 2>/dev/null || true
        rm -f /tmp/desd.pid
    fi
    sudo pkill -9 desd 2>/dev/null || true
    log_info "desd已停止"
    
    # 清理socket
    sudo rm -f /tmp/desd_control_socket /tmp/router_socket
    
    # 清理Docker网络
    sudo docker network rm bird_test_net 2>/dev/null || true
    log_info "Docker网络已清理"
    
    # 清理配置文件
    for i in $(seq 1 $NUM_ROUTERS); do
        rm -f /tmp/bird_r${i}.conf
    done
}

if [ "$KEEP_ENV" -eq 0 ]; then
    trap cleanup EXIT
else
    log_info "KEEP_ENV=1: 运行结束不自动清理环境"
fi

log_step "步骤1: 编译DES项目"
make clean && make
log_info "✓ 编译完成"

log_step "步骤2: 生成BIRD配置文件"
for i in $(seq 1 $NUM_ROUTERS); do
    ROUTER_IP="10.0.$i.$i"
    ROUTER_ID="$ROUTER_IP"
    AS_NUMBER="6500$i"
    
    cat > /tmp/bird_r${i}.conf << EOF
log stderr all;
debug protocols { states, routes, filters, interfaces, events };

router id $ROUTER_ID;

protocol device {
    scan time 10;
}

protocol direct {
    ipv4;
    interface "eth*";
}

protocol kernel {
    ipv4 {
        export all;
    };
    merge paths on;
}

protocol static static4 {
    ipv4;
    route 192.168.$i.0/24 blackhole;
}

EOF

    # 添加BGP配置 - 与所有其他路由器建立会话（full mesh）
    for j in $(seq 1 $NUM_ROUTERS); do
        if [ $i -ne $j ]; then
            PEER_IP="10.0.$j.$j"
            PEER_AS="6500$j"
            cat >> /tmp/bird_r${i}.conf << EOF
protocol bgp r$j {
    description "BGP to R$j";
    local $ROUTER_IP as $AS_NUMBER;
    neighbor $PEER_IP as $PEER_AS;
    
    ipv4 {
        import all;
        export where source ~ [ RTS_STATIC, RTS_BGP ];
    };
    
    hold time 180;
    keepalive time 60;
    connect retry time 5;
}

EOF
        fi
    done
    
    log_info "✓ R$i配置已创建 (AS$AS_NUMBER, $ROUTER_IP)"
done

log_step "步骤3: 创建Docker网络"
sudo docker network create --subnet=10.0.0.0/16 bird_test_net 2>/dev/null || log_info "网络已存在"
log_info "✓ Docker网络: bird_test_net (10.0.0.0/16)"

log_step "步骤4: 创建并启动容器"
for i in $(seq 1 $NUM_ROUTERS); do
    ROUTER_IP="10.0.$i.$i"
    
    sudo docker run -d \
        --name r$i \
        --hostname r$i \
        --network bird_test_net \
        --ip $ROUTER_IP \
        --cap-add=NET_ADMIN \
        --cap-add=NET_RAW \
        --privileged \
        -v /tmp:/tmp \
        -v /tmp/bird_r${i}.conf:/etc/bird/bird.conf:ro \
        ${BIRD_IMAGE:-bird:latest} \
        tail -f /dev/null
    
    log_info "✓ R$i容器已创建 ($ROUTER_IP)"
done

log_step "步骤5: 验证UDS挂载"
touch /tmp/test_uds_mount
ALL_MOUNTED=1
for i in $(seq 1 $NUM_ROUTERS); do
    if ! sudo docker exec r$i test -f /tmp/test_uds_mount; then
        log_error "R$i无法访问宿主机的/tmp"
        ALL_MOUNTED=0
    fi
done
rm -f /tmp/test_uds_mount

if [ $ALL_MOUNTED -eq 1 ]; then
    log_info "✓ 所有容器都可以访问宿主机的/tmp"
else
    log_error "部分容器无法访问宿主机的/tmp，挂载失败！"
    exit 1
fi

log_step "步骤6: 部署libdeshook.so"
for i in $(seq 1 $NUM_ROUTERS); do
    sudo docker cp build/libdeshook.so r$i:/usr/local/lib/libdeshook.so
    sudo docker exec r$i chmod 755 /usr/local/lib/libdeshook.so
done
log_info "✓ libdeshook.so已部署到所有容器"

log_step "步骤6.5: 初始化 core dump 环境（禁用 apport）"

for i in $(seq 1 $NUM_ROUTERS); do
    sudo docker exec r$i bash -c "
        echo core > /proc/sys/kernel/core_pattern
        ulimit -c unlimited
    "
done

log_info "✓ 所有容器已启用裸 core dump"



log_step "步骤7: 启动desd"
sudo rm -f /tmp/desd_control_socket /tmp/router_socket
mkdir -p logs
rm -f logs/desd_n${NUM_ROUTERS}.log

# 启动desd，传入路由器数量
sudo ./build/desd $NUM_ROUTERS > logs/desd_n${NUM_ROUTERS}.log 2>&1 &
DESD_PID=$!
echo $DESD_PID > /tmp/desd.pid
log_info "desd已启动，PID: $DESD_PID，等待 $NUM_ROUTERS 个路由器"

sleep 3

if ! ps -p $DESD_PID > /dev/null; then
    log_error "desd启动失败！查看日志："
    tail -20 logs/desd_n${NUM_ROUTERS}.log
    exit 1
fi

if [ ! -S /tmp/desd_control_socket ]; then
    log_error "desd控制socket未创建！"
    exit 1
fi

sudo chmod 666 /tmp/desd_control_socket
log_info "✓ desd运行中，socket权限已设置"

log_step "步骤8: 并行启动所有BIRD进程（使用FIFO队列匹配）"
log_info "并行启动 $NUM_ROUTERS 个BIRD进程..."

# 并行启动所有BIRD进程，使用FIFO队列确保CONNECTION_INFO匹配正确
for i in $(seq 1 $NUM_ROUTERS); do
    sudo docker exec -d r$i bash -c "
        ulimit -c unlimited
        export LD_PRELOAD=/usr/local/lib/libdeshook.so
        export ROUTER_ID=$i
        bird -f -c /etc/bird/bird.conf > /var/log/bird_r${i}.log 2>&1
    " &
done

# 等待所有启动命令完成
wait

log_info "等待BIRD进程初始化..."
sleep 5

log_step "步骤9: 验证BIRD进程"
ALL_RUNNING=1
mkdir -p logs/container_bird_logs 2>/dev/null || true
for i in $(seq 1 $NUM_ROUTERS); do
    BIRD_PID=$(sudo docker exec r$i pidof bird 2>/dev/null || echo "")
    if [ -n "$BIRD_PID" ]; then
        log_info "✓ R$i的BIRD已启动，PID: $BIRD_PID"
    else
        log_error "R$i的BIRD启动失败，收集容器日志"
        sudo docker cp r$i:/var/log/bird_r${i}.log logs/container_bird_logs/ 2>/dev/null || true
        sudo docker exec r$i cat /var/log/bird_r${i}.log 2>/dev/null || true
        ALL_RUNNING=0
    fi
done

if [ $ALL_RUNNING -eq 0 ]; then
    log_error "部分BIRD进程启动失败（已保留环境用于调试），不退出，继续观察并收集更多信息"
fi

log_step "步骤10: 运行测试 (${TEST_DURATION}秒)"
log_info "等待BGP会话建立..."

CHECKS=$((TEST_DURATION / 10))
mkdir -p logs/container_bird_logs 2>/dev/null || true
for i in $(seq 1 $CHECKS); do
    sleep 10
    
    # 检查desd是否仍在运行
    if ! ps -p $DESD_PID > /dev/null; then
        log_error "desd进程已退出！"
        break
    fi
    
    RUNNING_COUNT=0
    for j in $(seq 1 $NUM_ROUTERS); do
        if sudo docker exec r$j pidof bird > /dev/null 2>&1; then
            RUNNING_COUNT=$((RUNNING_COUNT + 1))
        fi
        sudo docker cp r$j:/var/log/bird_r${j}.log logs/container_bird_logs/bird_r${j}.log.iter${i} 2>/dev/null || true
    done
    
    VT=$(grep "responding with VT" logs/desd_n${NUM_ROUTERS}.log 2>/dev/null | tail -1 | grep -oP 'VT=\K[0-9.]+' || echo "N/A")
    echo "[$((i*10))s/${TEST_DURATION}s] DESD: running, BIRD: $RUNNING_COUNT/$NUM_ROUTERS, VT: ${VT}s"
done

log_step "步骤11: 检查BGP会话状态"
echo ""
TOTAL_SESSIONS=$((NUM_ROUTERS * (NUM_ROUTERS - 1)))
ESTABLISHED=0

for i in $(seq 1 $NUM_ROUTERS); do
    echo "R$i BGP状态："
    BGP_OUTPUT=$(sudo docker exec r$i birdc show protocols 2>/dev/null || echo "ERROR")
    echo "$BGP_OUTPUT"
    
    # 统计Established会话
    EST_COUNT=$(echo "$BGP_OUTPUT" | grep -c "Established" || echo "0")
    ESTABLISHED=$((ESTABLISHED + EST_COUNT))
    echo ""
done

log_step "测试结果"
echo "预期BGP会话数: $TOTAL_SESSIONS (${NUM_ROUTERS}路由器 full mesh)"
echo "已建立会话数: $ESTABLISHED"
echo ""

VT_FINAL=$(grep "responding with VT" logs/desd_n${NUM_ROUTERS}.log 2>/dev/null | tail -1 | grep -oP 'VT=\K[0-9.]+' || echo "N/A")
EVENT_COUNT=$(wc -l < logs/desd_n${NUM_ROUTERS}.log 2>/dev/null || echo "0")

echo "最终虚拟时间: ${VT_FINAL}s"
echo "DESD事件总数: $EVENT_COUNT"
echo ""

if [ "$ESTABLISHED" -eq "$TOTAL_SESSIONS" ]; then
    echo "🎉 测试成功！所有BGP会话已建立！"
else
    echo "⚠️  部分会话未建立，详细信息见日志"
    echo "查看desd日志: tail -f logs/desd_n${NUM_ROUTERS}.log"
    echo "查看R1日志: sudo docker exec r1 cat /var/log/bird_r1.log"
fi

echo ""
if [ "$KEEP_ENV" -eq 1 ]; then
    echo "测试环境将保持运行（KEEP_ENV=1），按Ctrl+C退出..."
    sleep infinity
else
    echo "测试完成（KEEP_ENV=0），将正常退出（触发trap清理）"
fi
