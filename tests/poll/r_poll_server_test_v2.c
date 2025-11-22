/*
 * poll() 精确 FD 匹配测试 - 服务端 v2
 * 
 * 简化版本：只监听一个地址，接受 3 个连接
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#define SOCKET_PATH "/tmp/poll_test_socket"

int main() {
    printf("\n========================================\n");
    printf("poll() 精确 FD 匹配测试 - 服务端 v2\n");
    printf("========================================\n\n");

    // 清理旧的 socket 文件
    unlink(SOCKET_PATH);

    // 创建监听 socket
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[SERVER] socket creation failed");
        return 1;
    }

    // 绑定并监听
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[SERVER] bind");
        return 1;
    }
    
    if (listen(listen_fd, 5) < 0) {
        perror("[SERVER] listen");
        return 1;
    }
    printf("[SERVER] Listening on %s (fd=%d)\n", SOCKET_PATH, listen_fd);

    printf("\n[SERVER] Waiting for 3 client connections...\n");

    // 接受 3 个连接
    int conn_fd1 = accept(listen_fd, NULL, NULL);
    if (conn_fd1 < 0) {
        perror("[SERVER] accept 1");
        return 1;
    }
    printf("[SERVER] ✓ Accepted connection 1 (fd=%d)\n", conn_fd1);

    int conn_fd2 = accept(listen_fd, NULL, NULL);
    if (conn_fd2 < 0) {
        perror("[SERVER] accept 2");
        return 1;
    }
    printf("[SERVER] ✓ Accepted connection 2 (fd=%d)\n", conn_fd2);

    int conn_fd3 = accept(listen_fd, NULL, NULL);
    if (conn_fd3 < 0) {
        perror("[SERVER] accept 3");
        return 1;
    }
    printf("[SERVER] ✓ Accepted connection 3 (fd=%d)\n", conn_fd3);

    printf("\n========================================\n");
    printf("测试开始：使用 poll() 监听 3 个 FD\n");
    printf("========================================\n\n");

    // 准备 poll 结构
    struct pollfd fds[3];
    fds[0].fd = conn_fd1;
    fds[0].events = POLLIN;
    fds[1].fd = conn_fd2;
    fds[1].events = POLLIN;
    fds[2].fd = conn_fd3;
    fds[2].events = POLLIN;

    printf("[SERVER] 配置 poll():\n");
    printf("  fds[0].fd = %d (连接 1)\n", conn_fd1);
    printf("  fds[1].fd = %d (连接 2)\n", conn_fd2);
    printf("  fds[2].fd = %d (连接 3)\n", conn_fd3);
    printf("\n[SERVER] 等待 5 秒...\n");
    printf("[SERVER] 提示：客户端应该只向连接 1 和 3 发送数据，不向连接 2 发送\n\n");

    sleep(2); // 给客户端时间发送数据

    // 调用 poll() - 5 秒超时
    int ret = poll(fds, 3, 5000);

    printf("\n========================================\n");
    printf("poll() 返回结果\n");
    printf("========================================\n\n");

    if (ret < 0) {
        perror("[SERVER] poll failed");
        return 1;
    } else if (ret == 0) {
        printf("[SERVER] ⚠ poll() 超时，没有任何 FD 就绪\n");
        printf("[SERVER] ✗ 测试失败：应该有数据到达\n");
        return 1;
    } else {
        printf("[SERVER] poll() 返回: %d 个 FD 就绪\n\n", ret);

        // 检查每个 FD 的状态
        int ready_count = 0;
        printf("[SERVER] 检查各个 FD 的 revents:\n");
        
        printf("  fds[0] (fd=%d): revents=0x%x ", conn_fd1, fds[0].revents);
        if (fds[0].revents & POLLIN) {
            printf("✓ POLLIN (可读)\n");
            ready_count++;
        } else {
            printf("✗ 无事件\n");
        }

        printf("  fds[1] (fd=%d): revents=0x%x ", conn_fd2, fds[1].revents);
        if (fds[1].revents & POLLIN) {
            printf("✗ POLLIN (不应该有数据！)\n");
            ready_count++;
        } else {
            printf("✓ 无事件 (正确)\n");
        }

        printf("  fds[2] (fd=%d): revents=0x%x ", conn_fd3, fds[2].revents);
        if (fds[2].revents & POLLIN) {
            printf("✓ POLLIN (可读)\n");
            ready_count++;
        } else {
            printf("✗ 无事件\n");
        }

        printf("\n[SERVER] 实际就绪的 FD 数量: %d\n", ready_count);

        // 验证结果
        printf("\n========================================\n");
        printf("测试结果验证\n");
        printf("========================================\n\n");

        int test_passed = 1;

        // 期望：FD1 有数据，FD2 无数据，FD3 有数据
        if (!(fds[0].revents & POLLIN)) {
            printf("[SERVER] ✗ 错误：FD1 应该有数据但未被标记\n");
            test_passed = 0;
        } else {
            printf("[SERVER] ✓ 正确：FD1 被标记为可读\n");
        }

        if (fds[1].revents & POLLIN) {
            printf("[SERVER] ✗ 错误：FD2 不应该有数据但被标记为可读\n");
            printf("[SERVER]   这说明 poll() 无法精确区分 FD（方案A行为）\n");
            test_passed = 0;
        } else {
            printf("[SERVER] ✓ 正确：FD2 未被标记（精确匹配生效）\n");
        }

        if (!(fds[2].revents & POLLIN)) {
            printf("[SERVER] ✗ 错误：FD3 应该有数据但未被标记\n");
            test_passed = 0;
        } else {
            printf("[SERVER] ✓ 正确：FD3 被标记为可读\n");
        }

        if (ret != 2) {
            printf("[SERVER] ⚠ 警告：poll() 返回 %d，期望是 2\n", ret);
        }

        // 读取数据验证
        printf("\n[SERVER] 尝试从各个 FD 读取数据:\n");
        char buf[1024];
        ssize_t n;

        if (fds[0].revents & POLLIN) {
            n = recv(conn_fd1, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                printf("  FD1: 收到 %zd 字节: \"%s\"\n", n, buf);
            }
        }

        if (fds[1].revents & POLLIN) {
            n = recv(conn_fd2, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                printf("  FD2: 收到 %zd 字节: \"%s\" (不应该收到！)\n", n, buf);
            }
        }

        if (fds[2].revents & POLLIN) {
            n = recv(conn_fd3, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                printf("  FD3: 收到 %zd 字节: \"%s\"\n", n, buf);
            }
        }

        printf("\n========================================\n");
        if (test_passed) {
            printf("✓✓✓ 测试通过！poll() 精确 FD 匹配工作正常！\n");
        } else {
            printf("✗✗✗ 测试失败！poll() 无法精确区分 FD。\n");
        }
        printf("========================================\n\n");

        // 清理
        close(conn_fd1);
        close(conn_fd2);
        close(conn_fd3);
        close(listen_fd);
        unlink(SOCKET_PATH);

        return test_passed ? 0 : 1;
    }

    return 0;
}

