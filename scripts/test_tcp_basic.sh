#!/bin/bash
# TCP版本基础测试脚本
# 用于测试DES对TCP socket的拦截

set -e

PROJECT_ROOT="/home/hwzhuang/hwzhuang/desTest/des_design"
cd "$PROJECT_ROOT"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}==>${NC} $1"
}

# 清理函数
cleanup() {
    log_warn "清理环境..."
    sudo pkill -9 desd || true
    sudo pkill -9 r1_tcp || true
    sudo pkill -9 r2_tcp || true
    sudo rm -f /tmp/desd_control_socket /tmp/router_socket
    log_info "清理完成"
}

# 捕获退出信号
trap cleanup EXIT

echo "=========================================="
echo " DES TCP 基础测试"
echo "=========================================="
echo ""

# 步骤1: 清理旧环境
log_step "步骤1: 清理旧环境"
cleanup
sleep 1

# 步骤2: 编译程序
log_step "步骤2: 编译程序"
make clean > /dev/null 2>&1 || true
make
log_info "✓ 编译完成"

# 步骤3: 启动desd
log_step "步骤3: 启动desd"
sudo rm -f /tmp/desd_control_socket
mkdir -p logs
rm -f logs/desd_tcp.log

sudo ./build/desd > logs/desd_tcp.log 2>&1 &
DESD_PID=$!
log_info "✓ desd已启动 (PID: $DESD_PID)"

sleep 2

# 检查desd是否运行
if ! ps -p $DESD_PID > /dev/null 2>&1; then
    log_error "desd启动失败，查看日志: logs/desd_tcp.log"
    exit 1
fi

# 检查control socket
if [ ! -S /tmp/desd_control_socket ]; then
    log_error "desd control socket未创建"
    exit 1
fi
log_info "✓ desd control socket已创建"

# 步骤4: 后台启动R2 (服务端)
log_step "步骤4: 启动R2 (TCP服务端)"
sudo ROUTER_ID=2 LD_PRELOAD=./build/libdeshook.so ./build/r2_tcp > logs/r2_tcp.log 2>&1 &
R2_PID=$!
log_info "✓ R2已启动 (PID: $R2_PID)"

sleep 2

# 检查R2是否运行
if ! ps -p $R2_PID > /dev/null 2>&1; then
    log_error "R2启动失败，查看日志: logs/r2_tcp.log"
    exit 1
fi

# 步骤5: 后台启动R1 (客户端)
log_step "步骤5: 启动R1 (TCP客户端)"
sudo ROUTER_ID=1 LD_PRELOAD=./build/libdeshook.so ./build/r1_tcp > logs/r1_tcp.log 2>&1 &
R1_PID=$!
log_info "✓ R1已启动 (PID: $R1_PID)"

sleep 2

# 检查R1是否运行
if ! ps -p $R1_PID > /dev/null 2>&1; then
    log_error "R1启动失败，查看日志: logs/r1_tcp.log"
    exit 1
fi

# 步骤6: 等待连接建立
log_step "步骤6: 等待连接建立"
sleep 3

# 检查desd日志中的连接信息
if grep -q "CONNECTION_ESTABLISHED" logs/desd_tcp.log; then
    log_info "✓ TCP连接已建立"
else
    log_warn "未检测到连接建立事件"
fi

# 步骤7: 测试数据传输
log_step "步骤7: 测试数据传输"
log_info "向R1发送测试消息..."

# 等待一段时间让程序处理
sleep 2

# 步骤8: 检查进程状态
log_step "步骤8: 检查进程状态"
echo ""
echo "进程状态："
echo "  desd (PID: $DESD_PID): $(ps -p $DESD_PID > /dev/null 2>&1 && echo '✓ 运行中' || echo '✗ 已停止')"
echo "  R1   (PID: $R1_PID):   $(ps -p $R1_PID > /dev/null 2>&1 && echo '✓ 运行中' || echo '✗ 已停止')"
echo "  R2   (PID: $R2_PID):   $(ps -p $R2_PID > /dev/null 2>&1 && echo '✓ 运行中' || echo '✗ 已停止')"
echo ""

# 步骤9: 显示日志统计
log_step "步骤9: 日志统计"
echo ""
echo "desd事件统计："
echo "  ROUTER_START事件:     $(grep -c "Processing event ROUTER_START" logs/desd_tcp.log || echo 0)"
echo "  CONNECT_REQUEST事件:  $(grep -c "CONNECT_REQUEST_EVENT" logs/desd_tcp.log || echo 0)"
echo "  LISTEN事件:           $(grep -c "LISTEN_EVENT" logs/desd_tcp.log || echo 0)"
echo "  CONNECTION_ESTABLISHED: $(grep -c "CONNECTION_ESTABLISHED" logs/desd_tcp.log || echo 0)"
echo "  PACKET_SEND事件:      $(grep -c "PACKET_SEND_EVENT" logs/desd_tcp.log || echo 0)"
echo "  PACKET_RECEIVE事件:   $(grep -c "Processing event PACKET_RECEIVE_EVENT" logs/desd_tcp.log || echo 0)"
echo ""

log_info "查看详细日志:"
echo "  desd: tail -f logs/desd_tcp.log"
echo "  R1:   tail -f logs/r1_tcp.log"
echo "  R2:   tail -f logs/r2_tcp.log"
echo ""

log_info "测试程序将继续运行，按Ctrl+C停止"
log_info "您可以手动向R1发送消息进行测试"
echo ""

# 保持运行直到用户中断
sleep infinity
