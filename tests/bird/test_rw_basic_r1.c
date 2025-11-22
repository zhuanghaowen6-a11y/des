// test_rw_basic_r1.c - 最基础的read/write测试（服务器）
// 只测试阻塞模式，避免复杂逻辑

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define PORT 8888

int main() {
    printf("R1: Starting basic read/write test (server)\n");
    fflush(stdout);
    
    // 创建socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("R1: socket() returned fd=%d\n", listen_fd);
    fflush(stdout);
    
    // 绑定
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(PORT);
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    printf("R1: bind() success\n");
    fflush(stdout);
    
    // 监听
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        return 1;
    }
    printf("R1: listen() success, waiting for connection...\n");
    fflush(stdout);
    
    // Accept（阻塞模式）
    int conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        perror("accept");
        return 1;
    }
    printf("R1: accept() returned fd=%d\n", conn_fd);
    fflush(stdout);
    
    // 测试fcntl设置非阻塞
    printf("R1: Testing fcntl() to set NON-BLOCKING...\n");
    fflush(stdout);
    int flags = fcntl(conn_fd, F_GETFL, 0);
    printf("R1: fcntl(F_GETFL) returned flags=0x%x\n", flags);
    fflush(stdout);
    
    fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK);
    printf("R1: fcntl(F_SETFL, O_NONBLOCK) called\n");
    fflush(stdout);
    
    // 改回阻塞模式
    fcntl(conn_fd, F_SETFL, flags);
    printf("R1: fcntl() reset to BLOCKING mode\n");
    fflush(stdout);
    
    // 使用write发送数据
    const char *msg = "Hello from R1";
    printf("R1: Calling write() to send: '%s'\n", msg);
    fflush(stdout);
    ssize_t sent = write(conn_fd, msg, strlen(msg));
    printf("R1: write() returned %zd bytes\n", sent);
    fflush(stdout);
    
    // 使用read接收数据（阻塞模式）
    char buffer[256];
    printf("R1: Calling read() to receive data (blocking)...\n");
    fflush(stdout);
    ssize_t received = read(conn_fd, buffer, sizeof(buffer) - 1);
    if (received > 0) {
        buffer[received] = '\0';
        printf("R1: read() returned %zd bytes: '%s'\n", received, buffer);
        fflush(stdout);
    } else {
        printf("R1: read() returned %zd\n", received);
        fflush(stdout);
    }
    
    printf("R1: Test completed!\n");
    fflush(stdout);
    close(conn_fd);
    close(listen_fd);
    return 0;
}
