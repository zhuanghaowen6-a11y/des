#!/bin/bash
# 快速GDB追踪 - 在现有环境中运行

set -e
cd /home/hwzhuang/hwzhuang/desTest/des_design

echo "检查环境..."

# 检查容器是否运行
if ! sudo docker ps | grep -q "r1"; then
    echo "[ERROR] r1容器未运行，请先运行 test_bird_uds_full.sh"
    exit 1
fi

# 获取BIRD PID
BIRD_PID=$(sudo docker exec r1 pidof bird 2>/dev/null || echo "")
if [ -z "$BIRD_PID" ]; then
    echo "[ERROR] BIRD未在r1中运行"
    exit 1
fi

echo "[INFO] 找到BIRD进程 (PID: $BIRD_PID)"

# 安装gdb（如果需要）
echo "[INFO] 确保gdb已安装..."
sudo docker exec r1 apt-get update -qq 2>/dev/null || true
sudo docker exec r1 apt-get install -y gdb -qq 2>&1 | grep -E "already|Setting" || true

# 创建简化的GDB脚本
echo "[INFO] 创建GDB脚本..."
sudo docker exec r1 bash -c 'cat > /tmp/simple_trace.gdb << "EOF"
set pagination off

attach '"$BIRD_PID"'

# 在clock_gettime设置断点
break clock_gettime

# 记录前10次调用的栈
commands
  silent
  set $n = $n + 1
  printf "\n===== Call #%d =====\n", $n
  bt 5
  if $n >= 10
    printf "\n===== 已捕获10次，停止 =====\n"
  else
    continue
  end
end

set $n = 0
continue
detach
quit
EOF
'

# 运行gdb
echo ""
echo "[INFO] 开始追踪clock_gettime调用..."
echo "[INFO] (这将捕获接下来的10次调用)"
echo ""

timeout 30 sudo docker exec r1 gdb -batch -x /tmp/simple_trace.gdb /usr/sbin/bird 2>&1 | tee /tmp/gdb_quick.log

echo ""
echo "=========================================="
echo " 追踪完成"
echo "=========================================="
echo ""
echo "分析调用栈（查找重复模式）:"
grep "^#[0-2]" /tmp/gdb_quick.log | sort | uniq -c | sort -rn

echo ""
echo "完整日志:"
cat /tmp/gdb_quick.log
