// 测试poll混合fd的性能 - 验证非DES就绪时立即返回
// 即使timeout很大，只要非DES fd就绪，也应该立即返回

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

#define SERVER_PORT 9998

// 获取当前时间（毫秒）
long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main() {
    printf("\n========== Poll Timing Test (验证立即返回) ==========\n");
    
    // 1. 创建pipe（非DES，已有数据）
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(1);
    }
    write(pipefd[1], "test", 4);  // 写入数据，使pipe立即可读
    printf("[TEST] Pipe created with data ready: read_fd=%d\n", pipefd[0]);
    
    // 2. 创建TCP socket（DES，连接到不存在的服务器，永远不会就绪）
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        exit(1);
    }
    printf("[TEST] TCP socket created: fd=%d (DES-managed)\n", sock_fd);
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SERVER_PORT);
    
    // 尝试连接（会失败或阻塞，但我们不等待）
    printf("[TEST] Attempting connect (will not complete)...\n");
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
    connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    // connect会失败或返回EINPROGRESS，这没关系
    
    // 3. 测试：poll混合fd，timeout=30000ms（30秒）
    printf("\n========== 关键测试：timeout=30000ms ==========\n");
    printf("[TEST] Pipe is ready (has data)\n");
    printf("[TEST] TCP is NOT ready (no server listening)\n");
    printf("[TEST] Expected: return immediately (< 100ms)\n");
    printf("[TEST] If bug exists: would wait 30 seconds\n");
    
    struct pollfd fds[2];
    fds[0].fd = pipefd[0];
    fds[0].events = POLLIN;
    fds[1].fd = sock_fd;
    fds[1].events = POLLIN;
    
    long long start_time = get_time_ms();
    printf("\n[TEST] Calling poll() at time=%lld ms...\n", start_time);
    
    int ret = poll(fds, 2, 30000);  // 30秒超时
    
    long long end_time = get_time_ms();
    long long elapsed = end_time - start_time;
    
    printf("[TEST] poll() returned at time=%lld ms\n", end_time);
    printf("[TEST] Elapsed time: %lld ms\n", elapsed);
    
    // 验证结果
    printf("\n========== 结果验证 ==========\n");
    
    if (ret < 0) {
        printf("✗ poll() failed: %s\n", strerror(errno));
        return 1;
    }
    
    printf("[RESULT] poll() returned: %d ready FD(s)\n", ret);
    
    // 检查时间
    if (elapsed < 100) {
        printf("✓ Timing CORRECT: returned in %lld ms (< 100ms)\n", elapsed);
        printf("✓ Did NOT wait for DES socket\n");
    } else if (elapsed > 1000) {
        printf("✗ Timing WRONG: took %lld ms (> 1 second)\n", elapsed);
        printf("✗ Bug: waited for DES socket despite non-DES ready!\n");
        return 1;
    } else {
        printf("⚠ Timing borderline: %lld ms (100-1000ms)\n", elapsed);
    }
    
    // 检查fd状态
    if (fds[0].revents & POLLIN) {
        printf("✓ Pipe fd %d is ready (POLLIN) - CORRECT\n", pipefd[0]);
        char buf[10];
        read(pipefd[0], buf, 4);
    } else {
        printf("✗ Pipe fd %d is NOT ready - WRONG!\n", pipefd[0]);
        return 1;
    }
    
    if (fds[1].revents == 0) {
        printf("✓ TCP fd %d is NOT ready (revents=0) - CORRECT\n", sock_fd);
    } else {
        printf("⚠ TCP fd %d has revents=%d\n", sock_fd, fds[1].revents);
    }
    
    // 清理
    close(sock_fd);
    close(pipefd[0]);
    close(pipefd[1]);
    
    printf("\n========== 测试总结 ==========\n");
    if (elapsed < 100) {
        printf("✓✓✓ TIMING TEST PASSED ✓✓✓\n");
        printf("✓ Non-DES fd就绪时立即返回（不等待DES）\n");
        printf("✓ 性能优化成功\n");
        return 0;
    } else {
        printf("✗✗✗ TIMING TEST FAILED ✗✗✗\n");
        return 1;
    }
}
