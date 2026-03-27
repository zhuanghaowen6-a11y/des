#!/bin/bash
# 容器-only BGP 收敛时间测量脚本（无 DES、无 CPU pinning）
# 使用 docker logs -t 获取带时间戳的日志
#
# 用法:
#   ./scripts/test_container_only.sh [TEST_DURATION] [TOPOLOGY_MODE]
#   例如: ./scripts/test_container_only.sh 120 fat-tree-k6
#
# 环境变量:
#   CPU_PINNING=0|1  (默认 0，不绑核)
#   BIRD_IMAGE       (默认 bird:latest)

set -e

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"
SCRIPT_DIR="${PROJECT_ROOT}/scripts"

# 默认参数
TEST_DURATION=${1:-120}
TOPOLOGY_MODE=${TOPOLOGY_MODE:-${2:-fat-tree-k6}}
TOPOLOGY_MODE=${TOPOLOGY_MODE,,}
CPU_PINNING=${CPU_PINNING:-0}
GLOBAL_CPUSET=${GLOBAL_CPUSET:-}
BIRD_IMAGE=${BIRD_IMAGE:-bird:latest}
KEEP_ENV=${KEEP_ENV:-0}
SKIP_ANALYZE=${SKIP_ANALYZE:-0}
PRE_DISABLE_OTHER_UPLINKS=${PRE_DISABLE_OTHER_UPLINKS:-0}
TOR_ID=${TOR_ID:-}
KEEP_AGG_ID=${KEEP_AGG_ID:-}
ENABLE_NETEM_DELAY=${ENABLE_NETEM_DELAY:-0}
NETEM_DELAY=${NETEM_DELAY:-}
NETEM_DELAY_IFACE=${NETEM_DELAY_IFACE:-eth0}

if [ "$ENABLE_NETEM_DELAY" -ne 0 ] && [ -z "$NETEM_DELAY" ]; then
    echo "[ERROR] ENABLE_NETEM_DELAY=1 requires NETEM_DELAY (example: 2ms)"
    exit 1
fi

if [ "$TOPOLOGY_MODE" != "ring" ] && [ "$TOPOLOGY_MODE" != "full-mesh" ] && [ "$TOPOLOGY_MODE" != "fat-tree-k6" ] && [ "$TOPOLOGY_MODE" != "fat-tree-k8-64" ] && [ "$TOPOLOGY_MODE" != "fat-tree-k4" ]; then
    echo "[ERROR] TOPOLOGY_MODE must be 'ring', 'full-mesh', 'fat-tree-k6', 'fat-tree-k8-64', or 'fat-tree-k4'"
    exit 1
fi

if [ "$PRE_DISABLE_OTHER_UPLINKS" -eq 1 ]; then
    if [ "$TOPOLOGY_MODE" != "fat-tree-k8-64" ]; then
        echo "[ERROR] PRE_DISABLE_OTHER_UPLINKS=1 only supported for fat-tree-k8-64"
        exit 1
    fi
    if [ -z "$TOR_ID" ] || [ -z "$KEEP_AGG_ID" ]; then
        echo "[ERROR] PRE_DISABLE_OTHER_UPLINKS=1 requires TOR_ID and KEEP_AGG_ID"
        exit 1
    fi
    if [ "$TOR_ID" -lt 41 ] || [ "$TOR_ID" -gt 64 ]; then
        echo "[ERROR] TOR_ID=$TOR_ID out of range for fat-tree-k8-64 (41..64)"
        exit 1
    fi
    _pod=$(( (TOR_ID - 41) / 4 ))
    _agg0=$((17 + _pod * 4 + 0))
    _agg1=$((17 + _pod * 4 + 1))
    _agg2=$((17 + _pod * 4 + 2))
    _agg3=$((17 + _pod * 4 + 3))
    if [ "$KEEP_AGG_ID" -ne "$_agg0" ] && [ "$KEEP_AGG_ID" -ne "$_agg1" ] && \
       [ "$KEEP_AGG_ID" -ne "$_agg2" ] && [ "$KEEP_AGG_ID" -ne "$_agg3" ]; then
        echo "[ERROR] KEEP_AGG_ID=$KEEP_AGG_ID is not an uplink of TOR_ID=$TOR_ID (uplinks: $_agg0 $_agg1 $_agg2 $_agg3)"
        exit 1
    fi
    SKIP_TOR_AGG_PEERS=""
    for _a in $_agg0 $_agg1 $_agg2 $_agg3; do
        if [ "$_a" -ne "$KEEP_AGG_ID" ]; then
            SKIP_TOR_AGG_PEERS="$SKIP_TOR_AGG_PEERS $_a"
        fi
    done
    echo "[INFO] PRE_DISABLE mode: ToR $TOR_ID will only peer with Agg $KEEP_AGG_ID (skipping:$SKIP_TOR_AGG_PEERS)"
fi

# 根据拓扑确定路由器数量
if [ "$TOPOLOGY_MODE" = "fat-tree-k6" ]; then
    NUM_ROUTERS=45
elif [ "$TOPOLOGY_MODE" = "fat-tree-k8-64" ]; then
    NUM_ROUTERS=64
elif [ "$TOPOLOGY_MODE" = "fat-tree-k4" ]; then
    NUM_ROUTERS=20
elif [ "$TOPOLOGY_MODE" = "ring" ]; then
    NUM_ROUTERS=${NUM_ROUTERS:-10}
else
    NUM_ROUTERS=${NUM_ROUTERS:-5}
fi

# 创建结果目录
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
if [ -n "${RESULT_DIR_OVERRIDE:-}" ]; then
    RESULT_DIR="$RESULT_DIR_OVERRIDE"
else
    if [ "$CPU_PINNING" -eq 1 ]; then
        RESULT_DIR="results/container_only/with_pinning/${TOPOLOGY_MODE}_n${NUM_ROUTERS}_${TIMESTAMP}"
    else
        RESULT_DIR="results/container_only/no_pinning/${TOPOLOGY_MODE}_n${NUM_ROUTERS}_${TIMESTAMP}"
    fi
fi
mkdir -p "$RESULT_DIR/logs" "$RESULT_DIR/configs" "$RESULT_DIR/meta"

echo "=========================================="
echo "Container-Only BGP Convergence Test"
echo "=========================================="
echo "Routers: $NUM_ROUTERS"
echo "Test Duration: ${TEST_DURATION}s"
echo "Topology: $TOPOLOGY_MODE"
echo "CPU Pinning: $CPU_PINNING"
echo "Global CPUSet: ${GLOBAL_CPUSET:-<none>}"
echo "BIRD Image: $BIRD_IMAGE"
echo "ENABLE_NETEM_DELAY: $ENABLE_NETEM_DELAY"
if [ "$ENABLE_NETEM_DELAY" -ne 0 ]; then
    echo "NETEM_DELAY: $NETEM_DELAY"
    echo "NETEM_DELAY_IFACE: $NETEM_DELAY_IFACE"
fi
echo "PRE_DISABLE_OTHER_UPLINKS: $PRE_DISABLE_OTHER_UPLINKS"
if [ "$PRE_DISABLE_OTHER_UPLINKS" -eq 1 ]; then
    echo "  TOR_ID: $TOR_ID, KEEP_AGG_ID: $KEEP_AGG_ID"
fi
echo "Result Dir: $RESULT_DIR"
echo "=========================================="

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

apply_container_netem_delay() {
    local container_name=$1
    if [ "$ENABLE_NETEM_DELAY" -eq 0 ]; then
        return 0
    fi
    sudo docker exec "$container_name" sh -c "command -v tc >/dev/null 2>&1"
    sudo docker exec "$container_name" tc qdisc replace dev "$NETEM_DELAY_IFACE" root netem delay "$NETEM_DELAY"
    log_info "✓ $container_name netem delay on $NETEM_DELAY_IFACE: $NETEM_DELAY"
}

# 清理函数
cleanup() {
    log_step "清理环境"
    sudo docker stop $(seq -f r%g 1 $NUM_ROUTERS) || true
    sudo docker rm $(seq -f r%g 1 $NUM_ROUTERS) || true
    sudo docker network rm bird_test_net 2>/dev/null || true
    for i in $(seq 1 $NUM_ROUTERS); do
        rm -f /tmp/bird_r${i}.conf
    done
    log_info "环境已清理"
}

trap cleanup EXIT

# ============================================================
# Fat-tree k=6 辅助函数
# ============================================================
get_fat_tree_k6_neighbors() {
    local router_id=$1
    local neighbors=""

    if [ $router_id -le 9 ]; then
        local group=$(( (router_id - 1) / 3 ))
        for pod in $(seq 0 5); do
            local agg_id=$((10 + pod * 3 + group))
            neighbors="$neighbors $agg_id"
        done
    elif [ $router_id -le 27 ]; then
        local pod=$(( (router_id - 10) / 3 ))
        local a=$(( (router_id - 10) % 3 ))
        for idx in $(seq 0 2); do
            local core_id=$((1 + a * 3 + idx))
            neighbors="$neighbors $core_id"
        done
        for e in $(seq 0 2); do
            local tor_id=$((28 + pod * 3 + e))
            neighbors="$neighbors $tor_id"
        done
    else
        local pod=$(( (router_id - 28) / 3 ))
        for a in $(seq 0 2); do
            local agg_id=$((10 + pod * 3 + a))
            neighbors="$neighbors $agg_id"
        done
    fi
    echo $neighbors
}

get_fat_tree_k8_64_neighbors() {
    local router_id=$1
    local neighbors=""

    if [ $router_id -le 16 ]; then
        local group=$(( (router_id - 1) / 4 ))
        for pod in $(seq 0 5); do
            local agg_id=$((17 + pod * 4 + group))
            neighbors="$neighbors $agg_id"
        done
    elif [ $router_id -le 40 ]; then
        local idx=$((router_id - 17))
        local pod=$(( idx / 4 ))
        local a=$(( idx % 4 ))
        for core_idx in $(seq 0 3); do
            local core_id=$((1 + a * 4 + core_idx))
            neighbors="$neighbors $core_id"
        done
        for e in $(seq 0 3); do
            local tor_id=$((41 + pod * 4 + e))
            neighbors="$neighbors $tor_id"
        done
    else
        local idx=$((router_id - 41))
        local pod=$(( idx / 4 ))
        for a in $(seq 0 3); do
            local agg_id=$((17 + pod * 4 + a))
            neighbors="$neighbors $agg_id"
        done
    fi

    echo $neighbors
}

get_fat_tree_k4_neighbors() {
    local router_id=$1
    local neighbors=""
    if [ $router_id -le 4 ]; then
        local group=$(( (router_id - 1) / 2 ))
        for pod in $(seq 0 3); do
            local agg_id=$((5 + pod * 2 + group))
            neighbors="$neighbors $agg_id"
        done
    elif [ $router_id -le 12 ]; then
        local pod=$(( (router_id - 5) / 2 ))
        local a=$(( (router_id - 5) % 2 ))
        for idx in $(seq 0 1); do
            local core_id=$((1 + a * 2 + idx))
            neighbors="$neighbors $core_id"
        done
        for e in $(seq 0 1); do
            local tor_id=$((13 + pod * 2 + e))
            neighbors="$neighbors $tor_id"
        done
    else
        local pod=$(( (router_id - 13) / 2 ))
        for a in $(seq 0 1); do
            local agg_id=$((5 + pod * 2 + a))
            neighbors="$neighbors $agg_id"
        done
    fi
    echo $neighbors
}

is_tor_router() {
    local router_id=$1
    if [ "$TOPOLOGY_MODE" = "fat-tree-k6" ]; then
        [ $router_id -ge 28 ] && [ $router_id -le 45 ]
    elif [ "$TOPOLOGY_MODE" = "fat-tree-k8-64" ]; then
        [ $router_id -ge 41 ] && [ $router_id -le 64 ]
    else
        [ $router_id -ge 13 ] && [ $router_id -le 20 ]
    fi
}

SKIP_PEER_SET=""
if [ "$PRE_DISABLE_OTHER_UPLINKS" -eq 1 ] && [ "$TOPOLOGY_MODE" = "fat-tree-k8-64" ]; then
    _TOR_ID=${TOR_ID:-41}
    _KEEP_AGG=${KEEP_AGG_ID:-17}
    _pod=$(( (_TOR_ID - 41) / 4 ))
    _agg0=$((17 + _pod * 4 + 0))
    _agg1=$((17 + _pod * 4 + 1))
    _agg2=$((17 + _pod * 4 + 2))
    _agg3=$((17 + _pod * 4 + 3))
    for _a in $_agg0 $_agg1 $_agg2 $_agg3; do
        if [ "$_a" -ne "$_KEEP_AGG" ]; then
            SKIP_PEER_SET="$SKIP_PEER_SET ${_TOR_ID}:${_a} ${_a}:${_TOR_ID}"
        fi
    done
fi

should_skip_peer() {
    local src=$1
    local dst=$2
    case " $SKIP_PEER_SET " in
        *" ${src}:${dst} "*) return 0 ;;
    esac
    return 1
}

# ============================================================
# 步骤 1: 记录元数据
# ============================================================
log_step "步骤 1: 记录实验元数据"

# 记录环境信息
cat > "$RESULT_DIR/meta/env.txt" << EOF
TOPOLOGY_MODE=$TOPOLOGY_MODE
NUM_ROUTERS=$NUM_ROUTERS
TEST_DURATION=$TEST_DURATION
CPU_PINNING=$CPU_PINNING
GLOBAL_CPUSET=$GLOBAL_CPUSET
BIRD_IMAGE=$BIRD_IMAGE
PRE_DISABLE_OTHER_UPLINKS=$PRE_DISABLE_OTHER_UPLINKS
TOR_ID=$TOR_ID
KEEP_AGG_ID=$KEEP_AGG_ID
TIMESTAMP=$TIMESTAMP
EOF

# 记录宿主机信息
uname -a > "$RESULT_DIR/meta/host_uname.txt"
lscpu > "$RESULT_DIR/meta/host_lscpu.txt" 2>/dev/null || echo "lscpu not available" > "$RESULT_DIR/meta/host_lscpu.txt"
docker version > "$RESULT_DIR/meta/docker_version.txt" 2>/dev/null || echo "docker version failed" > "$RESULT_DIR/meta/docker_version.txt"

log_info "元数据已记录到 $RESULT_DIR/meta/"

# ============================================================
# 步骤 2: 生成 BIRD 配置文件
# ============================================================
log_step "步骤 2: 生成 BIRD 配置文件"

for i in $(seq 1 $NUM_ROUTERS); do
    ROUTER_IP="10.0.$i.$i"
    ROUTER_ID="$ROUTER_IP"
    AS_NUMBER=$((65000 + i))
    
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

EOF

    # 只有 ToR 路由器起源前缀（fat-tree-k6 模式）
    if [ "$TOPOLOGY_MODE" = "fat-tree-k6" ] || [ "$TOPOLOGY_MODE" = "fat-tree-k8-64" ]; then
        if is_tor_router $i; then
            cat >> /tmp/bird_r${i}.conf << EOF
protocol static static4 {
    ipv4;
    route 192.168.$i.0/24 blackhole;
}

EOF
        fi
    else
        cat >> /tmp/bird_r${i}.conf << EOF
protocol static static4 {
    ipv4;
    route 192.168.$i.0/24 blackhole;
}

EOF
    fi

    # 计算邻居列表
    if [ "$TOPOLOGY_MODE" = "ring" ]; then
        LEFT=$((i - 1))
        RIGHT=$((i + 1))
        if [ $LEFT -lt 1 ]; then LEFT=$NUM_ROUTERS; fi
        if [ $RIGHT -gt $NUM_ROUTERS ]; then RIGHT=1; fi
        if [ "$LEFT" -eq "$RIGHT" ]; then
            NEIGHBORS="$LEFT"
        else
            NEIGHBORS="$LEFT $RIGHT"
        fi
    elif [ "$TOPOLOGY_MODE" = "fat-tree-k6" ]; then
        NEIGHBORS=$(get_fat_tree_k6_neighbors $i)
    elif [ "$TOPOLOGY_MODE" = "fat-tree-k8-64" ]; then
        NEIGHBORS=$(get_fat_tree_k8_64_neighbors $i)
    elif [ "$TOPOLOGY_MODE" = "fat-tree-k4" ]; then
        NEIGHBORS=$(get_fat_tree_k4_neighbors $i)
    else
        NEIGHBORS=$(seq 1 $NUM_ROUTERS)
    fi

    for j in $NEIGHBORS; do
        if [ $i -ne $j ]; then
            if should_skip_peer $i $j; then
                log_info "PRE_DISABLE: skipping protocol bgp r$j in R$i config"
                continue
            fi
            PEER_IP="10.0.$j.$j"
            PEER_AS=$((65000 + j))
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
    
    # 复制配置到结果目录
    cp /tmp/bird_r${i}.conf "$RESULT_DIR/configs/"
done

log_info "✓ 配置文件已生成并存档"

# ============================================================
# 步骤 3: 创建 Docker 网络
# ============================================================
log_step "步骤 3: 创建 Docker 网络"
sudo docker network rm bird_test_net 2>/dev/null || true
sudo docker network create --subnet=10.0.0.0/16 bird_test_net
log_info "✓ Docker 网络: bird_test_net (10.0.0.0/16)"

# ============================================================
# 步骤 4: 创建容器（等待模式，不启动 BIRD）
# ============================================================
log_step "步骤 4: 创建容器（等待模式，不启动 BIRD）"

for i in $(seq 1 $NUM_ROUTERS); do
    ROUTER_IP="10.0.$i.$i"
    sudo docker rm -f r$i 2>/dev/null || true
    
    # CPU pinning 选项
    CPUSET_OPT=""
    if [ -n "${GLOBAL_CPUSET:-}" ]; then
        CPUSET_OPT="--cpuset-cpus=$GLOBAL_CPUSET"
    else
        if [ "$CPU_PINNING" -eq 1 ]; then
            # 每个路由器绑定一个核（简单轮询分配）
            CORE_ID=$(( (i - 1) % $(nproc) ))
            CPUSET_OPT="--cpuset-cpus=$CORE_ID"
        fi
    fi
    
    sudo docker run -d \
        --name r$i \
        --hostname r$i \
        --network bird_test_net \
        --ip $ROUTER_IP \
        --cap-add=NET_ADMIN \
        --cap-add=NET_RAW \
        --privileged \
        $CPUSET_OPT \
        -v /tmp/bird_r${i}.conf:/etc/bird/bird.conf:ro \
        ${BIRD_IMAGE} \
        sh -c 'tail -f /dev/null'

    apply_container_netem_delay "r$i"
    
    log_info "✓ R$i 容器已创建 ($ROUTER_IP)"
done

# ============================================================
# 步骤 5: 记录 t0 并启动 BIRD（docker exec + 重定向到 PID1 stdout/stderr）
# ============================================================
T0_EPOCH=$(date +%s.%N)
echo "$T0_EPOCH" > "$RESULT_DIR/meta/t0_epoch.txt"
log_info "t0 = $T0_EPOCH (已记录)"

start_fail=0
pids=()
for i in $(seq 1 $NUM_ROUTERS); do
    sudo docker exec -d r$i sh -c 'bird -f -c /etc/bird/bird.conf > /proc/1/fd/1 2> /proc/1/fd/2' &
    pids+=("$!")
done

for pid in "${pids[@]}"; do
    if ! wait "$pid"; then
        start_fail=1
    fi
done

if [ "$start_fail" -ne 0 ]; then
    log_error "启动 BIRD 失败（docker exec 返回非 0）。请检查镜像/容器状态。"
    exit 1
fi

# ============================================================
# 步骤 6: 等待测试时长
# ============================================================
log_step "步骤 6: 等待收敛 (${TEST_DURATION}s)"

# 简单等待固定时长（后续可优化为检测静默）
sleep $TEST_DURATION

# ============================================================
# 步骤 7: 收集日志（使用 docker logs -t）
# ============================================================
log_step "步骤 7: 收集日志"

for i in $(seq 1 $NUM_ROUTERS); do
    # 使用 docker logs -t 获取带时间戳的日志
    sudo docker logs -t r$i > "$RESULT_DIR/logs/bird_r${i}.log" 2>&1
done

# 记录 BIRD 版本
sudo docker exec r1 bird --version > "$RESULT_DIR/meta/bird_version.txt" 2>&1 || echo "bird --version failed" > "$RESULT_DIR/meta/bird_version.txt"

log_info "✓ 日志已收集到 $RESULT_DIR/logs/"

# ============================================================
# 步骤 8: 分析结果
# ============================================================
log_step "步骤 8: 分析结果"

ANALYZE_SCRIPT="${SCRIPT_DIR}/analyze_bgp_logs.py"

if [ "$SKIP_ANALYZE" -eq 1 ]; then
    log_info "跳过 analyze_bgp_logs.py（SKIP_ANALYZE=1）"
    ANALYZE_RESULT=0
else
    if [ -f "$ANALYZE_SCRIPT" ]; then
        # 设置环境变量告诉分析脚本使用 wall-clock 模式
        T0_FILE="$RESULT_DIR/meta/t0_epoch.txt" \
        TIME_MODE=wallclock \
        python3 "$ANALYZE_SCRIPT" "$NUM_ROUTERS" "$RESULT_DIR/logs" "$TOPOLOGY_MODE"
        ANALYZE_RESULT=$?
    else
        log_error "分析脚本不存在: $ANALYZE_SCRIPT"
        ANALYZE_RESULT=1
    fi
fi

# ============================================================
# 步骤 9: 输出结果摘要
# ============================================================
log_step "实验完成"

echo ""
echo "结果目录: $RESULT_DIR"
echo "  - logs/     : BIRD 日志（带 docker 时间戳）"
echo "  - configs/  : BIRD 配置文件"
echo "  - meta/     : 元数据（t0、环境、宿主机信息）"
echo ""

if [ $ANALYZE_RESULT -eq 0 ]; then
    echo "🎉 实验成功！"
else
    echo "⚠️ 实验发现问题，详见上方分析报告"
fi

if [ "$KEEP_ENV" -eq 1 ]; then
    trap - EXIT
fi
