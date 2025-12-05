#!/bin/bash
# 构建BIRD Docker镜像

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="${IMAGE_NAME:-bird:latest}"

echo "=========================================="
echo " 构建BIRD Docker镜像"
echo "=========================================="
echo ""
echo "镜像名称: $IMAGE_NAME"
echo "Dockerfile: $SCRIPT_DIR/Dockerfile.bird"
echo ""

# 检查Dockerfile是否存在
if [ ! -f "$SCRIPT_DIR/Dockerfile.bird" ]; then
    echo "错误: Dockerfile.bird 不存在"
    exit 1
fi

# 构建镜像
echo "开始构建..."
sudo docker build -f "$SCRIPT_DIR/Dockerfile.bird" -t "$IMAGE_NAME" "$SCRIPT_DIR"

echo ""
echo "=========================================="
echo " 构建完成！"
echo "=========================================="
echo ""

# 验证镜像
sudo docker images | grep "^bird"

echo ""
echo "测试镜像..."
sudo docker run --rm "$IMAGE_NAME" bird --version || \
sudo docker run --rm "$IMAGE_NAME" /usr/sbin/bird --version

echo ""
echo "✓ BIRD镜像构建成功！"
echo ""
echo "使用方法："
echo "  sudo docker run -it $IMAGE_NAME bash"
echo ""
