// 测试poll性能 - 服务器端
// 验证非DES fd就绪时立即返回（不等待DES）

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

#define SERVER_PORT 9997

// 获取当前时间（毫秒）
long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main() {
    printf("\n========== R2 Poll Timing Test (Server) ==========\n");
    
    // 1. 创建pipe（非DES，写入数据使其立即可读）
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(1);
    }
    write(pipefd[1], "test_data", 9);  // 写入数据
    printf("[R2] Pipe created with data ready: read_fd=%d\n", pipefd[0]);
    
    // 2. 创建TCP listening socket（DES管理）
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(1);
    }
    
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        exit(1);
    }
    printf("[R2] TCP listening on 127.0.0.1:%d (DES-managed)\n", SERVER_PORT);
    
    // 3. Accept连接（但客户端不会立即连接）
    printf("\n[R2] Waiting for client connection...\n");
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept");
        exit(1);
    }
    printf("[R2] Client connected: fd=%d\n", client_fd);
    
    // 4. 关键测试：poll混合fd，timeout=30000ms（30秒）
    printf("\n========== 关键测试：Mixed Poll with Large Timeout ==========\n");
    printf("[R2] Pipe is ready (has data) - 非DES\n");
    printf("[R2] TCP client_fd not ready yet - DES管理\n");
    printf("[R2] timeout=30000ms (30 seconds)\n");
    printf("[R2] Expected: return IMMEDIATELY (< 100ms) due to pipe ready\n");
    printf("[R2] Old behavior: would wait for TCP or timeout\n");
    
    struct pollfd fds[2];
    fds[0].fd = pipefd[0];
    fds[0].events = POLLIN;
    fds[1].fd = client_fd;
    fds[1].events = POLLIN;
    
    long long start_time = get_time_ms();
    printf("\n[R2] Calling poll() at %lld ms...\n", start_time);
    
    int ret = poll(fds, 2, 30000);  // 30秒超时
    
    long long end_time = get_time_ms();
    long long elapsed = end_time - start_time;
    
    printf("[R2] poll() returned at %lld ms\n", end_time);
    printf("[R2] Elapsed time: %lld ms\n", elapsed);
    
    // 验证结果
    printf("\n========== 验证结果 ==========\n");
    
    if (ret < 0) {
        printf("✗ poll() failed: %s\n", strerror(errno));
        close(client_fd);
        close(listen_fd);
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }
    
    printf("[R2] poll() returned: %d ready FD(s)\n", ret);
    
    // 检查时间
    int timing_ok = 0;
    if (elapsed < 100) {
        printf("✓ Timing CORRECT: %lld ms (< 100ms)\n", elapsed);
        printf("✓ 立即返回，没有等待DES socket\n");
        timing_ok = 1;
    } else if (elapsed > 1000) {
        printf("✗ Timing WRONG: %lld ms (> 1 second)\n", elapsed);
        printf("✗ Bug: 等待了DES socket，性能优化失败！\n");
    } else {
        printf("⚠ Timing borderline: %lld ms\n", elapsed);
        timing_ok = 1;  // 接受
    }
    
    // 检查fd状态
    int fd_status_ok = 0;
    if (fds[0].revents & POLLIN) {
        printf("✓ Pipe fd %d is ready (POLLIN)\n", pipefd[0]);
        char buf[10];
        read(pipefd[0], buf, 9);
        fd_status_ok = 1;
    } else {
        printf("✗ Pipe fd %d NOT ready - WRONG!\n", pipefd[0]);
    }
    
    if (fds[1].revents == 0) {
        printf("✓ TCP fd %d NOT ready (revents=0) - 符合预期\n", client_fd);
    } else {
        printf("⚠ TCP fd %d revents=%d\n", client_fd, fds[1].revents);
    }
    
    // 发送确认给客户端，让它也能完成测试
    const char *msg = "OK";
    send(client_fd, msg, strlen(msg), 0);
    
    sleep(1);  // 等待客户端接收
    
    // 清理
    close(client_fd);
    close(listen_fd);
    close(pipefd[0]);
    close(pipefd[1]);
    
    printf("\n========== R2 测试总结 ==========\n");
    if (timing_ok && fd_status_ok) {
        printf("✓✓✓ R2 TIMING TEST PASSED ✓✓✓\n");
        printf("✓ 性能优化成功\n");
        return 0;
    } else {
        printf("✗✗✗ R2 TIMING TEST FAILED ✗✗✗\n");
        return 1;
    }
}
