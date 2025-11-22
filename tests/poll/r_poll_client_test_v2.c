/*
 * poll() 精确 FD 匹配测试 - 客户端 v2
 * 
 * 简化版本：连接到同一个地址 3 次
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCKET_PATH "/tmp/poll_test_socket"

int main() {
    printf("\n========================================\n");
    printf("poll() 精确 FD 匹配测试 - 客户端 v2\n");
    printf("========================================\n\n");

    // 创建 3 个 socket并连接
    int sockfd1 = socket(AF_UNIX, SOCK_STREAM, 0);
    int sockfd2 = socket(AF_UNIX, SOCK_STREAM, 0);
    int sockfd3 = socket(AF_UNIX, SOCK_STREAM, 0);

    if (sockfd1 < 0 || sockfd2 < 0 || sockfd3 < 0) {
        perror("[CLIENT] socket creation failed");
        return 1;
    }

    printf("[CLIENT] 创建了 3 个 socket: fd=%d, %d, %d\n\n", sockfd1, sockfd2, sockfd3);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // 连接 1
    printf("[CLIENT] 连接 1 到 %s...\n", SOCKET_PATH);
    if (connect(sockfd1, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[CLIENT] connect 1");
        return 1;
    }
    printf("[CLIENT] ✓ 连接 1 成功 (fd=%d)\n", sockfd1);

    // 连接 2
    printf("[CLIENT] 连接 2 到 %s...\n", SOCKET_PATH);
    if (connect(sockfd2, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[CLIENT] connect 2");
        return 1;
    }
    printf("[CLIENT] ✓ 连接 2 成功 (fd=%d)\n", sockfd2);

    // 连接 3
    printf("[CLIENT] 连接 3 到 %s...\n", SOCKET_PATH);
    if (connect(sockfd3, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[CLIENT] connect 3");
        return 1;
    }
    printf("[CLIENT] ✓ 连接 3 成功 (fd=%d)\n", sockfd3);

    printf("\n========================================\n");
    printf("测试场景：选择性发送数据\n");
    printf("========================================\n\n");

    // 等待一下，确保服务端准备好
    sleep(1);

    // 只向连接 1 和 3 发送数据，不向连接 2 发送
    const char *msg1 = "Data for connection 1";
    const char *msg3 = "Data for connection 3";

    printf("[CLIENT] 向连接 1 (fd=%d) 发送数据: \"%s\"\n", sockfd1, msg1);
    if (send(sockfd1, msg1, strlen(msg1), 0) < 0) {
        perror("[CLIENT] send 1");
        return 1;
    }
    printf("[CLIENT] ✓ 连接 1 发送成功\n");

    printf("\n[CLIENT] 🚫 不向连接 2 (fd=%d) 发送数据（故意跳过）\n", sockfd2);
    printf("[CLIENT]    这是测试的关键：验证 poll() 不会标记 FD2\n");

    printf("\n[CLIENT] 向连接 3 (fd=%d) 发送数据: \"%s\"\n", sockfd3, msg3);
    if (send(sockfd3, msg3, strlen(msg3), 0) < 0) {
        perror("[CLIENT] send 3");
        return 1;
    }
    printf("[CLIENT] ✓ 连接 3 发送成功\n");

    printf("\n========================================\n");
    printf("发送完成\n");
    printf("========================================\n\n");
    printf("[CLIENT] 数据发送总结:\n");
    printf("  连接 1: ✓ 已发送数据\n");
    printf("  连接 2: ✗ 未发送数据 (测试重点)\n");
    printf("  连接 3: ✓ 已发送数据\n");
    printf("\n[CLIENT] 期望服务端 poll() 结果:\n");
    printf("  FD1: 应该被标记为可读 (POLLIN)\n");
    printf("  FD2: 不应该被标记 (revents=0)\n");
    printf("  FD3: 应该被标记为可读 (POLLIN)\n");
    printf("  返回值: 2 (只有 2 个 FD 就绪)\n");

    printf("\n[CLIENT] 等待 3 秒后退出...\n");
    sleep(3);

    // 清理
    close(sockfd1);
    close(sockfd2);
    close(sockfd3);

    printf("[CLIENT] 测试完成。\n\n");
    return 0;
}

