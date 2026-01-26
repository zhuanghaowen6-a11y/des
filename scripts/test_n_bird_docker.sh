#!/bin/bash
# N-Router BIRD + Docker + DES 测试脚本
# 基于test_bird_uds_full.sh扩展到N个路由器

set -e

KEEP_ENV=${KEEP_ENV:-1}        # 默认 1：运行结束不清理，便于调试
ENABLE_STRACE=${ENABLE_STRACE:-1}  # 默认 1：对每个 BIRD 开启 strace 跟踪

PROJECT_ROOT="/home/hwzhuang/hwzhuang/desTest/des_design"
cd "$PROJECT_ROOT"

# 脚本所在目录（用于定位 analyze_bgp_logs.py 等辅助脚本）
SCRIPT_DIR="${PROJECT_ROOT}/scripts"

# 默认参数
NUM_ROUTERS=${1:-5}
TEST_DURATION=${2:-60}

if [ "$NUM_ROUTERS" -lt 2 ] || [ "$NUM_ROUTERS" -gt 250 ]; then
    echo "[ERROR] NUM_ROUTERS must be between 2 and 250"
    exit 1
fi

echo "=========================================="
echo "N-Router BGP Test with Docker"
echo "Routers: $NUM_ROUTERS"
echo "Test Duration: ${TEST_DURATION}s"
echo "KEEP_ENV: $KEEP_ENV (1=保留环境, 0=自动清理)"
echo "ENABLE_STRACE: $ENABLE_STRACE (1=开启strace, 0=关闭)"
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

# 启用 core dump（在宿主机上设置，容器通过 -v /tmp:/tmp 共享）
enable_core_dumps() {
    log_step "开启 core dump 支持"

    # 在宿主机上设置 core_pattern，让所有进程（包括容器里的 BIRD）把 core 写到宿主机 /tmp
    if command -v sysctl >/dev/null 2>&1; then
        if ! sudo sysctl -w kernel.core_pattern=/tmp/core.%e.%p >/dev/null 2>&1; then
            log_info "无法设置 kernel.core_pattern（可能需要手动: sudo sysctl -w kernel.core_pattern=/tmp/core.%e.%p）"
        else
            log_info "kernel.core_pattern 已设置为 /tmp/core.%e.%p（core 会生成在宿主机 /tmp 下）"
        fi
    else
        log_info "sysctl 不存在，跳过 kernel.core_pattern 配置"
    fi

    # 放宽 suid 可生成 core
    sudo sysctl -w fs.suid_dumpable=2 >/dev/null 2>&1 || true
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

enable_core_dumps

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
    # 实际上，这里改为环形拓扑：每个路由器只与前后两个邻居建立 BGP 会话

    # 计算环形拓扑中的前后邻居编号
    LEFT=$((i - 1))
    RIGHT=$((i + 1))
    if [ $LEFT -lt 1 ]; then
        LEFT=$NUM_ROUTERS
    fi
    if [ $RIGHT -gt $NUM_ROUTERS ]; then
        RIGHT=1
    fi

    # 构造去重后的邻居列表（NUM_ROUTERS=2 时避免重复）
    if [ "$LEFT" -eq "$RIGHT" ]; then
        NEIGHBORS="$LEFT"
    else
        NEIGHBORS="$LEFT $RIGHT"
    fi

    for j in $NEIGHBORS; do
        # 理论上不会等于自身，但这里防御性跳过
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

    sudo docker rm -f r$i 2>/dev/null || true
    
    sudo docker run -d \
        --name r$i \
        --hostname r$i \
        --network bird_test_net \
        --ip $ROUTER_IP \
        --cap-add=NET_ADMIN \
        --cap-add=NET_RAW \
        --cap-add=SYS_PTRACE \
        --security-opt seccomp=unconfined \
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

log_step "步骤6.5: 初始化 core dump 环境"

for i in $(seq 1 $NUM_ROUTERS); do
    # 只设置 ulimit，core_pattern 已在宿主机统一配置
    sudo docker exec r$i bash -c "ulimit -c unlimited" 2>/dev/null || true
done

log_info "✓ 所有容器已启用 core dump（core 文件写入宿主机 /tmp）"



log_step "步骤7: 启动desd"
sudo rm -f /tmp/desd_control_socket /tmp/router_socket
mkdir -p logs
rm -f logs/desd_n${NUM_ROUTERS}.log

# 启动desd，传入路由器数量
sudo ./build/desd $NUM_ROUTERS > logs/desd_n${NUM_ROUTERS}.log 2>&1 &
DESD_PID=$!
echo $DESD_PID > /tmp/desd.pid
log_info "desd已启动，PID: $DESD_PID，等待 $NUM_ROUTERS 个路由器"

# 等待 desd socket 创建，最多等待 10 秒
for attempt in $(seq 1 20); do
    if [ -S /tmp/desd_control_socket ]; then
        log_info "desd socket 已创建 (尝试 $attempt/20)"
        break
    fi
    sleep 0.5
done

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

# 并行启动所有BIRD进程
# 关键点：
# - 不在 shell 中 export LD_PRELOAD，避免 strace 进程本身也被当成一个"router"去连接 DESD
# - 仅通过 env 为被跟踪的 bird 进程注入 LD_PRELOAD 和 ROUTER_ID
if [ "$ENABLE_STRACE" -eq 1 ]; then
    log_info "strace 已启用，跟踪关键系统调用到 /tmp/bird_r*_strace.log"
    for i in $(seq 1 $NUM_ROUTERS); do
        sudo docker exec -d r$i bash -c '
            ulimit -c unlimited
            strace -f -o /tmp/bird_r'"$i"'_strace.log \
                -e trace=close,exit,exit_group,signal,poll,select,recvfrom,read,write,socket,connect,accept,bind,listen \
                env LD_PRELOAD=/usr/local/lib/libdeshook.so ROUTER_ID='"$i"' \
                bird -f -c /etc/bird/bird.conf > /var/log/bird_r'"$i"'.log 2>&1
        ' &
    done
else
    log_info "strace 已禁用，直接启动 BIRD"
    for i in $(seq 1 $NUM_ROUTERS); do
        sudo docker exec -d r$i bash -c '
            ulimit -c unlimited
            env LD_PRELOAD=/usr/local/lib/libdeshook.so ROUTER_ID='"$i"' \
                bird -f -c /etc/bird/bird.conf > /var/log/bird_r'"$i"'.log 2>&1
        ' &
    done
fi

# 等待所有启动命令完成
wait

log_step "步骤11: 收集最终日志"
echo ""

# 将容器内的 BIRD 日志复制到 logs 目录，供分析脚本使用
for i in $(seq 1 $NUM_ROUTERS); do
    sudo docker cp r$i:/var/log/bird_r${i}.log logs/bird_r${i}.log 2>/dev/null || true
    # 如果启用了 strace，也复制 strace 日志
    if [ "$ENABLE_STRACE" -eq 1 ]; then
        sudo docker cp r$i:/tmp/bird_r${i}_strace.log logs/bird_r${i}_strace.log 2>/dev/null || true
    fi
done
log_info "日志已收集到 logs/ 目录"

log_step "步骤12: 分析测试结果"
echo ""

# 使用 Python 脚本进行全面的日志分析
# 该脚本会检查：
#   1. DESD 是否正常退出（达到事件上限）
#   2. 所有 BGP 会话是否成功建立
#   3. 在 DESD 退出前，是否有任何 BGP 会话被协议层主动断开
ANALYZE_SCRIPT="${SCRIPT_DIR}/analyze_bgp_logs.py"

if [ -f "$ANALYZE_SCRIPT" ]; then
    # 当前脚本生成的是环形拓扑配置，因此这里显式使用 ring 模式进行分析
    python3 "$ANALYZE_SCRIPT" "$NUM_ROUTERS" "logs" ring
    ANALYZE_RESULT=$?
else
    log_error "分析脚本不存在: $ANALYZE_SCRIPT"
    ANALYZE_RESULT=1
fi

echo ""
if [ $ANALYZE_RESULT -eq 0 ]; then
    echo "🎉 测试成功！BGP 会话在整个仿真期间保持健康。"
else
    echo "⚠️  测试发现问题，详细信息见上方分析报告"
    echo "查看desd日志: less logs/desd_n${NUM_ROUTERS}.log"
    echo "查看R1日志: less logs/bird_r1.log"
fi

echo ""
if [ "$KEEP_ENV" -eq 1 ]; then
    echo "测试环境将保持运行（KEEP_ENV=1），按Ctrl+C退出..."
    sleep infinity
else
    echo "测试完成（KEEP_ENV=0），将正常退出（触发trap清理）"
fi
