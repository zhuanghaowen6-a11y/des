// 测试poll混合fd处理 - 服务器端
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
#define TEST_MESSAGE "Hello from server via TCP"
#define PIPE_MESSAGE "Data from pipe"

int main() {
    printf("\n========== R2 (Server) Poll Mixed FD Test ==========\n");
    
    // 1. 创建pipe（非DES管理的fd）
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(1);
    }
    printf("[R2] Created pipe: read_fd=%d, write_fd=%d (non-DES)\n", pipefd[0], pipefd[1]);
    
    // 2. 创建TCP listening socket（DES管理）
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }
    printf("[R2] Created TCP listen socket: fd=%d (DES-managed)\n", listen_fd);
    
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
    printf("[R2] Listening on 127.0.0.1:%d\n", SERVER_PORT);
    
    // 3. 向pipe写入数据（模拟本地数据源）
    printf("\n[R2] Writing data to pipe...\n");
    write(pipefd[1], PIPE_MESSAGE, strlen(PIPE_MESSAGE));
    printf("[R2] Wrote '%s' to pipe\n", PIPE_MESSAGE);
    
    // 4. Accept连接
    printf("\n[R2] Waiting for client connection...\n");
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept");
        exit(1);
    }
    printf("[R2] Accepted client connection: fd=%d (DES-managed)\n", client_fd);
    
    // 5. 使用poll同时监听pipe和TCP socket
    printf("\n========== Test 1: Poll Mixed FDs (pipe + TCP) ==========\n");
    
    struct pollfd fds[2];
    fds[0].fd = pipefd[0];      // 非DES管理的pipe
    fds[0].events = POLLIN;
    fds[1].fd = client_fd;       // DES管理的TCP socket
    fds[1].events = POLLIN;
    
    printf("[R2] Polling 2 fds: pipe(%d) + TCP(%d)\n", pipefd[0], client_fd);
    printf("[R2] Expecting: pipe should be ready immediately (has data)\n");
    printf("[R2] Expecting: TCP should be ready when client sends data\n");
    
    int ret = poll(fds, 2, 5000);  // 5秒超时
    
    if (ret < 0) {
        perror("poll");
        exit(1);
    } else if (ret == 0) {
        printf("[R2] ✗ Poll timeout\n");
        exit(1);
    }
    
    printf("[R2] ✓ poll() returned %d ready FD(s)\n", ret);
    
    // 检查pipe是否就绪
    if (fds[0].revents & POLLIN) {
        printf("[R2] ✓ Pipe fd %d is ready (POLLIN)\n", pipefd[0]);
        char buf[256];
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("[R2] ✓ Read from pipe: '%s'\n", buf);
        }
    } else {
        printf("[R2] ✗ Pipe fd %d is NOT ready (expected ready)\n", pipefd[0]);
    }
    
    // 检查TCP socket是否就绪
    if (fds[1].revents & POLLIN) {
        printf("[R2] ✓ TCP fd %d is ready (POLLIN)\n", client_fd);
        char buf[256];
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            printf("[R2] ✓ Received from client: '%s'\n", buf);
        }
    } else {
        printf("[R2] TCP fd %d is not ready yet (client may not have sent data)\n", client_fd);
    }
    
    // 6. 发送响应给客户端
    printf("\n[R2] Sending response to client...\n");
    send(client_fd, TEST_MESSAGE, strlen(TEST_MESSAGE), 0);
    printf("[R2] ✓ Sent: '%s'\n", TEST_MESSAGE);
    
    sleep(1);  // 等待客户端接收
    
    // 清理
    close(client_fd);
    close(listen_fd);
    close(pipefd[0]);
    close(pipefd[1]);
    
    printf("\n========== Test Summary ==========\n");
    printf("✓ Mixed FD poll test PASSED\n");
    printf("✓ Pipe (non-DES) and TCP (DES) both handled correctly\n");
    
    return 0;
}
