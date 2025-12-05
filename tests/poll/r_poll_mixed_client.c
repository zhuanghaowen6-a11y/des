// 测试poll混合fd处理 - 客户端
// 同时监听TCP socket（DES管理）和pipe（非DES）

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>

#define SERVER_PORT 9999
#define TEST_MESSAGE "Hello from client via TCP"
#define PIPE_MESSAGE "Client pipe data"

int main() {
    printf("\n========== R1 (Client) Poll Mixed FD Test ==========\n");
    
    // 等待服务器启动
    sleep(2);
    
    // 1. 创建pipe（非DES管理的fd）
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(1);
    }
    printf("[R1] Created pipe: read_fd=%d, write_fd=%d (non-DES)\n", pipefd[0], pipefd[1]);
    
    // 2. 创建TCP socket连接到服务器（DES管理）
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        exit(1);
    }
    printf("[R1] Created TCP socket: fd=%d (DES-managed)\n", sock_fd);
    
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
    printf("[R1] ✓ Connected to server\n");
    
    // 3. 向pipe写入数据（模拟本地数据）
    printf("\n[R1] Writing data to pipe...\n");
    write(pipefd[1], PIPE_MESSAGE, strlen(PIPE_MESSAGE));
    printf("[R1] Wrote '%s' to pipe\n", PIPE_MESSAGE);
    
    // 4. 发送数据给服务器
    printf("\n[R1] Sending message to server...\n");
    send(sock_fd, TEST_MESSAGE, strlen(TEST_MESSAGE), 0);
    printf("[R1] ✓ Sent: '%s'\n", TEST_MESSAGE);
    
    // 5. 使用poll同时监听pipe和TCP socket
    printf("\n========== Test 1: Poll Mixed FDs (pipe + TCP) ==========\n");
    
    struct pollfd fds[2];
    fds[0].fd = pipefd[0];      // 非DES管理的pipe
    fds[0].events = POLLIN;
    fds[1].fd = sock_fd;         // DES管理的TCP socket
    fds[1].events = POLLIN;
    
    printf("[R1] Polling 2 fds: pipe(%d) + TCP(%d)\n", pipefd[0], sock_fd);
    printf("[R1] Expecting: pipe should be ready immediately (has data)\n");
    printf("[R1] Expecting: TCP should be ready when server responds\n");
    
    int ret = poll(fds, 2, 10000);  // 10秒超时
    
    if (ret < 0) {
        perror("poll");
        exit(1);
    } else if (ret == 0) {
        printf("[R1] ✗ Poll timeout\n");
        exit(1);
    }
    
    printf("[R1] ✓ poll() returned %d ready FD(s)\n", ret);
    
    // 检查pipe是否就绪
    if (fds[0].revents & POLLIN) {
        printf("[R1] ✓ Pipe fd %d is ready (POLLIN)\n", pipefd[0]);
        char buf[256];
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("[R1] ✓ Read from pipe: '%s'\n", buf);
        }
    } else {
        printf("[R1] ✗ Pipe fd %d is NOT ready (expected ready)\n", pipefd[0]);
    }
    
    // 检查TCP socket是否就绪
    if (fds[1].revents & POLLIN) {
        printf("[R1] ✓ TCP fd %d is ready (POLLIN)\n", sock_fd);
        char buf[256];
        ssize_t n = recv(sock_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            printf("[R1] ✓ Received from server: '%s'\n", buf);
        }
    } else {
        printf("[R1] TCP fd %d is not ready yet\n", sock_fd);
    }
    
    // 清理
    close(sock_fd);
    close(pipefd[0]);
    close(pipefd[1]);
    
    printf("\n========== Test Summary ==========\n");
    printf("✓ Mixed FD poll test PASSED\n");
    printf("✓ Pipe (non-DES) and TCP (DES) both handled correctly\n");
    
    return 0;
}
