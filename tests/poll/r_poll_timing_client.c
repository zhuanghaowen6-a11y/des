// 测试poll性能 - 客户端
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
    printf("\n========== R1 Poll Timing Test (Client) ==========\n");
    
    // 等待服务器启动
    sleep(2);
    
    // 1. 创建pipe（非DES，写入数据使其立即可读）
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(1);
    }
    write(pipefd[1], "client_data", 11);  // 写入数据
    printf("[R1] Pipe created with data ready: read_fd=%d\n", pipefd[0]);
    
    // 2. 连接到服务器（DES管理的TCP socket）
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        exit(1);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SERVER_PORT);
    
    printf("[R1] Connecting to server...\n");
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        exit(1);
    }
    printf("[R1] Connected to server: fd=%d (DES-managed)\n", sock_fd);
    
    // 3. 关键测试：poll混合fd，timeout=30000ms（30秒）
    // 注意：我们不发送数据，所以TCP socket不会有数据可读
    printf("\n========== 关键测试：Mixed Poll with Large Timeout ==========\n");
    printf("[R1] Pipe is ready (has data) - 非DES\n");
    printf("[R1] TCP sock_fd not ready (no data from server yet) - DES管理\n");
    printf("[R1] timeout=30000ms (30 seconds)\n");
    printf("[R1] Expected: return IMMEDIATELY (< 100ms) due to pipe ready\n");
    printf("[R1] Old behavior: would wait for TCP or timeout\n");
    
    struct pollfd fds[2];
    fds[0].fd = pipefd[0];
    fds[0].events = POLLIN;
    fds[1].fd = sock_fd;
    fds[1].events = POLLIN;
    
    long long start_time = get_time_ms();
    printf("\n[R1] Calling poll() at %lld ms...\n", start_time);
    
    int ret = poll(fds, 2, 30000);  // 30秒超时
    
    long long end_time = get_time_ms();
    long long elapsed = end_time - start_time;
    
    printf("[R1] poll() returned at %lld ms\n", end_time);
    printf("[R1] Elapsed time: %lld ms\n", elapsed);
    
    // 验证结果
    printf("\n========== 验证结果 ==========\n");
    
    if (ret < 0) {
        printf("✗ poll() failed: %s\n", strerror(errno));
        close(sock_fd);
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }
    
    printf("[R1] poll() returned: %d ready FD(s)\n", ret);
    
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
        char buf[20];
        read(pipefd[0], buf, 11);
        fd_status_ok = 1;
    } else {
        printf("✗ Pipe fd %d NOT ready - WRONG!\n", pipefd[0]);
    }
    
    if (fds[1].revents == 0) {
        printf("✓ TCP fd %d NOT ready (revents=0) - 符合预期\n", sock_fd);
    } else {
        printf("⚠ TCP fd %d revents=%d\n", sock_fd, fds[1].revents);
    }
    
    // 接收服务器的确认消息
    char recv_buf[10];
    recv(sock_fd, recv_buf, sizeof(recv_buf), 0);
    
    // 清理
    close(sock_fd);
    close(pipefd[0]);
    close(pipefd[1]);
    
    printf("\n========== R1 测试总结 ==========\n");
    if (timing_ok && fd_status_ok) {
        printf("✓✓✓ R1 TIMING TEST PASSED ✓✓✓\n");
        printf("✓ 性能优化成功\n");
        return 0;
    } else {
        printf("✗✗✗ R1 TIMING TEST FAILED ✗✗✗\n");
        return 1;
    }
}
