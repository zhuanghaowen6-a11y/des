#!/bin/bash
# BIRD + UDS + DES 完整自动化测试脚本
# 用途：从零开始测试DES在真实BIRD容器上的运行

set -e  # 遇到错误立即退出

PROJECT_ROOT="/home/hwzhuang/hwzhuang/desTest/des_design"
cd "$PROJECT_ROOT"

# 日志函数（简化格式，避免颜色干扰）
log_info() {
    echo "[INFO] $1"
}

log_warn() {
    echo "[WARN] $1"
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

# 检查Docker是否安装
check_docker() {
    if ! command -v docker &> /dev/null; then
        log_error "Docker未安装，请先安装Docker"
        exit 1
    fi
    log_info "Docker版本: $(docker --version)"
}

# 检查并构建BIRD镜像
check_bird_image() {
    BIRD_IMAGE="${BIRD_IMAGE:-bird:latest}"
    
    # 检查镜像是否存在
    if sudo docker images | grep -q "^bird"; then
        log_info "✓ BIRD镜像已存在: $BIRD_IMAGE"
        return 0
    fi
    
    log_warn "未找到BIRD镜像"
    log_info "可以："
    echo "  1. 自动构建BIRD镜像（推荐）"
    echo "  2. 手动指定已有的镜像"
    echo "  3. 退出"
    echo ""
    read -p "请选择 (1/2/3): " -n 1 -r
    echo ""
    
    case $REPLY in
        1)
            log_info "开始构建BIRD镜像..."
            if [ -f "$PROJECT_ROOT/docker/build_bird_image.sh" ]; then
                bash "$PROJECT_ROOT/docker/build_bird_image.sh"
                if sudo docker images | grep -q "^bird"; then
                    log_info "✓ BIRD镜像构建成功"
                else
                    log_error "镜像构建失败"
                    exit 1
                fi
            else
                log_error "构建脚本不存在: docker/build_bird_image.sh"
                exit 1
            fi
            ;;
        2)
            log_info "请输入镜像名称（如 ubuntu:20.04）："
            read BIRD_IMAGE
            export BIRD_IMAGE
            log_info "使用镜像: $BIRD_IMAGE"
            ;;
        3)
            log_info "退出"
            exit 0
            ;;
        *)
            log_error "无效选择"
            exit 1
            ;;
    esac
}

# 清理函数
cleanup() {
    log_step "清理环境"
    
    # 停止容器
    sudo docker stop r1 r2 2>/dev/null || true
    sudo docker rm r1 r2 2>/dev/null || true
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
    log_info "socket已清理"
    
    # 清理Docker网络
    sudo docker network rm bird_test_net 2>/dev/null || true
    log_info "Docker网络已清理"
    
    # 清理配置文件
    rm -f /tmp/bird_r1.conf /tmp/bird_r2.conf /tmp/test_uds_mount
    log_info "临时文件已清理"
}

# 捕获退出信号
trap cleanup EXIT

# ============================================
# 主流程开始
# ============================================

log_step "步骤0: 环境检查"
check_docker
check_bird_image

log_step "步骤1: 编译DES项目"
make clean && make
log_info "✓ 编译完成"
ls -lh build/desd build/libdeshook.so

log_step "步骤2: 准备BIRD配置文件"

# R1配置
cat > /tmp/bird_r1.conf << 'EOF'
log stderr all;
debug protocols { states, routes, filters, interfaces, events };

router id 10.0.1.1;

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
    route 192.168.1.0/24 blackhole;
}

protocol bgp r2 {
    description "BGP to R2";
    local 10.0.1.1 as 65001;
    neighbor 10.0.2.2 as 65002;
    
    ipv4 {
        import all;
        export where source ~ [ RTS_STATIC, RTS_BGP ];
    };
    
    hold time 180;
    keepalive time 60;
    connect retry time 5;
}
EOF
log_info "✓ R1配置已创建"

# R2配置
cat > /tmp/bird_r2.conf << 'EOF'
log stderr all;
debug protocols { states, routes, filters, interfaces, events };

router id 10.0.2.2;

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
    route 192.168.2.0/24 blackhole;
}

protocol bgp r1 {
    description "BGP to R1";
    local 10.0.2.2 as 65002;
    neighbor 10.0.1.1 as 65001;
    
    ipv4 {
        import all;
        export where source ~ [ RTS_STATIC, RTS_BGP ];
    };
    
    hold time 180;
    keepalive time 60;
    connect retry time 5;
}
EOF
log_info "✓ R2配置已创建"

log_step "步骤3: 创建Docker网络"
sudo docker network create --subnet=10.0.0.0/16 bird_test_net 2>/dev/null || log_warn "网络已存在"
log_info "✓ Docker网络: bird_test_net (10.0.0.0/16)"

log_step "步骤4: 创建R1容器"
sudo docker run -d \
    --name r1 \
    --hostname r1 \
    --network bird_test_net \
    --ip 10.0.1.1 \
    --cap-add=NET_ADMIN \
    --cap-add=NET_RAW \
    --privileged \
    -v /tmp:/tmp \
    -v /tmp/bird_r1.conf:/etc/bird/bird.conf:ro \
    ${BIRD_IMAGE:-bird:latest} \
    tail -f /dev/null

log_info "✓ R1容器已创建"
sudo docker ps | grep r1

log_step "步骤5: 创建R2容器"
sudo docker run -d \
    --name r2 \
    --hostname r2 \
    --network bird_test_net \
    --ip 10.0.2.2 \
    --cap-add=NET_ADMIN \
    --cap-add=NET_RAW \
    --privileged \
    -v /tmp:/tmp \
    -v /tmp/bird_r2.conf:/etc/bird/bird.conf:ro \
    ${BIRD_IMAGE:-bird:latest} \
    tail -f /dev/null

log_info "✓ R2容器已创建"
sudo docker ps | grep r2

log_step "步骤6: 验证UDS挂载"
touch /tmp/test_uds_mount
if sudo docker exec r1 test -f /tmp/test_uds_mount && sudo docker exec r2 test -f /tmp/test_uds_mount; then
    log_info "✓ 两个容器都可以访问宿主机的/tmp"
else
    log_error "容器无法访问宿主机的/tmp，挂载失败！"
    exit 1
fi
rm -f /tmp/test_uds_mount

log_step "步骤7: 部署libdeshook.so"
sudo docker cp build/libdeshook.so r1:/usr/local/lib/libdeshook.so
sudo docker exec r1 chmod 755 /usr/local/lib/libdeshook.so
log_info "✓ libdeshook.so已部署到R1"

sudo docker cp build/libdeshook.so r2:/usr/local/lib/libdeshook.so
sudo docker exec r2 chmod 755 /usr/local/lib/libdeshook.so
log_info "✓ libdeshook.so已部署到R2"

log_step "步骤8: 启动desd"
sudo rm -f /tmp/desd_control_socket /tmp/router_socket
mkdir -p logs
rm -f logs/desd_bird.log

sudo ./build/desd > logs/desd_bird.log 2>&1 &
DESD_PID=$!
echo $DESD_PID > /tmp/desd.pid
log_info "desd已启动，PID: $DESD_PID"

sleep 3

if ! ps -p $DESD_PID > /dev/null; then
    log_error "desd启动失败！查看日志："
    tail -20 logs/desd_bird.log
    exit 1
fi

if [ ! -S /tmp/desd_control_socket ]; then
    log_error "desd控制socket未创建！"
    exit 1
fi

sudo chmod 666 /tmp/desd_control_socket
log_info "✓ desd运行中，socket权限已设置"

log_step "步骤9: 同时启动R1和R2的BIRD"
log_info "同时启动两个BIRD进程以确保desd能同时接收连接..."

# 几乎同时启动两个BIRD进程
sudo docker exec -d r1 bash -c '
    export LD_PRELOAD=/usr/local/lib/libdeshook.so
    export ROUTER_ID=1
    bird -f -c /etc/bird/bird.conf > /var/log/bird_r1.log 2>&1
' &

sudo docker exec -d r2 bash -c '
    export LD_PRELOAD=/usr/local/lib/libdeshook.so
    export ROUTER_ID=2
    bird -f -c /etc/bird/bird.conf > /var/log/bird_r2.log 2>&1
' &

# 等待两个启动命令完成
wait

log_info "等待BIRD进程初始化..."
sleep 5

log_step "步骤10: 验证BIRD进程"
R1_BIRD_PID=$(sudo docker exec r1 pidof bird 2>/dev/null || echo "")
if [ -n "$R1_BIRD_PID" ]; then
    log_info "✓ R1的BIRD已启动，PID: $R1_BIRD_PID"
else
    log_error "R1的BIRD启动失败"
    sudo docker exec r1 cat /var/log/bird_r1.log 2>/dev/null || true
    exit 1
fi

R2_BIRD_PID=$(sudo docker exec r2 pidof bird 2>/dev/null || echo "")
if [ -n "$R2_BIRD_PID" ]; then
    log_info "✓ R2的BIRD已启动，PID: $R2_BIRD_PID"
else
    log_error "R2的BIRD启动失败"
    sudo docker exec r2 cat /var/log/bird_r2.log 2>/dev/null || true
    exit 1
fi

log_step "步骤11: 验证虚拟时间功能"
log_info "等待5秒让BIRD运行..."
sleep 5

VT_COUNT=$(grep -c "GET_VIRTUAL_TIME_EVENT" logs/desd_bird.log 2>/dev/null || echo "0")
log_info "GET_VIRTUAL_TIME_EVENT 事件数: $VT_COUNT"

if [ "$VT_COUNT" -gt 0 ]; then
    log_info "✓ 虚拟时间功能正常工作！"
    echo ""
    echo "最近5次虚拟时间响应："
    grep "responding with VT" logs/desd_bird.log | tail -5
else
    log_warn "未检测到虚拟时间事件（可能BIRD尚未调用clock_gettime）"
fi

log_step "步骤12: 使用ltrace验证拦截"
if command -v ltrace &> /dev/null; then
    log_info "跟踪R1的clock_gettime调用（10秒）..."
    timeout 10 sudo docker exec r1 ltrace -e 'clock_gettime' -p $R1_BIRD_PID 2>&1 | head -15 || true
else
    log_warn "ltrace未安装，跳过验证"
fi

log_step "步骤13: 检查BGP会话状态"
echo ""
echo "R1的BGP协议状态："
sudo docker exec r1 birdc show protocols 2>/dev/null || log_warn "birdc命令失败"

echo ""
echo "R2的BGP协议状态："
sudo docker exec r2 birdc show protocols 2>/dev/null || log_warn "birdc命令失败"

log_step "步骤14: 监控虚拟时间推进"
log_info "观察30秒内虚拟时间的变化..."
echo ""

for i in {1..6}; do
    VT=$(grep "responding with VT" logs/desd_bird.log | tail -1 | grep -oP 'VT=\K[0-9.]+' 2>/dev/null || echo "N/A")
    EVENT_COUNT=$(wc -l < logs/desd_bird.log 2>/dev/null || echo "0")
    echo "[$i/6] 虚拟时间: ${VT}s, 事件总数: ${EVENT_COUNT}"
    sleep 5
done

log_step "测试完成！"

echo ""
echo "=========================================="
echo " 测试结果总结"
echo "=========================================="
echo ""

# 统计结果
VT_FINAL_COUNT=$(grep -c "GET_VIRTUAL_TIME_EVENT" logs/desd_bird.log 2>/dev/null || echo "0")
R1_RUNNING=$(sudo docker exec r1 pidof bird 2>/dev/null && echo "✓" || echo "✗")
R2_RUNNING=$(sudo docker exec r2 pidof bird 2>/dev/null && echo "✓" || echo "✗")
DESD_RUNNING=$(ps -p $DESD_PID > /dev/null && echo "✓" || echo "✗")

echo "1. desd状态:               $DESD_RUNNING"
echo "2. R1 BIRD状态:            $R1_RUNNING"
echo "3. R2 BIRD状态:            $R2_RUNNING"
echo "4. 虚拟时间事件数:         $VT_FINAL_COUNT"
echo ""

if [ "$VT_FINAL_COUNT" -gt 50 ] && [ "$R1_RUNNING" = "✓" ] && [ "$R2_RUNNING" = "✓" ]; then
    log_info "🎉 测试成功！DES可以在真实BIRD容器上运行！"
else
    log_warn "⚠️  测试部分成功，但可能需要进一步调试"
fi

echo ""
echo "=========================================="
echo " 后续操作"
echo "=========================================="
echo ""
echo "查看desd日志:        tail -f logs/desd_bird.log"
echo "查看R1 BIRD日志:     sudo docker exec r1 cat /var/log/bird_r1.log"
echo "查看R2 BIRD日志:     sudo docker exec r2 cat /var/log/bird_r2.log"
echo "检查BGP状态:         sudo docker exec r1 birdc show protocols all"
echo "进入R1容器:          sudo docker exec -it r1 bash"
echo "进入R2容器:          sudo docker exec -it r2 bash"
echo ""
echo "停止测试:            按Ctrl+C（会自动清理环境）"
echo "                    或运行: sudo docker stop r1 r2 && sudo kill $DESD_PID"
echo ""

# 保持运行，便于观察
log_info "测试环境将保持运行，按Ctrl+C退出并清理..."
sleep infinity
