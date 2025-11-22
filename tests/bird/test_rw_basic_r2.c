// test_rw_basic_r2.c - 最基础的read/write测试（客户端）
// 只测试阻塞模式，避免复杂逻辑

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define PORT 8888

int main() {
    printf("R2: Starting basic read/write test (client)\n");
    fflush(stdout);
    
    // 等待服务器启动
    sleep(2);
    
    // 创建socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("R2: socket() returned fd=%d\n", sock_fd);
    fflush(stdout);
    
    // 连接
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);
    
    printf("R2: Calling connect() to %s:%d...\n", SERVER_IP, PORT);
    fflush(stdout);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("R2: connect() success\n");
    fflush(stdout);
    
    // 使用read接收数据（阻塞模式）
    char buffer[256];
    printf("R2: Calling read() to receive data (blocking)...\n");
    fflush(stdout);
    ssize_t received = read(sock_fd, buffer, sizeof(buffer) - 1);
    if (received > 0) {
        buffer[received] = '\0';
        printf("R2: read() returned %zd bytes: '%s'\n", received, buffer);
        fflush(stdout);
    } else {
        printf("R2: read() returned %zd\n", received);
        fflush(stdout);
    }
    
    // 使用write发送回复
    const char *msg = "Hello from R2";
    printf("R2: Calling write() to send: '%s'\n", msg);
    fflush(stdout);
    ssize_t sent = write(sock_fd, msg, strlen(msg));
    printf("R2: write() returned %zd bytes\n", sent);
    fflush(stdout);
    
    printf("R2: Test completed!\n");
    fflush(stdout);
    close(sock_fd);
    return 0;
}
