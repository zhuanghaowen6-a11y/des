// test_simple_rw_r2.c - 简化的read/write测试（客户端）
// 使用阻塞socket简化测试

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8888
#define BUFFER_SIZE 1024

int main() {
    printf("=== R2: Simple read/write test (client) ===\n");
    
    // 等待服务器启动
    sleep(2);
    
    // 1. 创建socket并连接
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("R2: Created socket fd=%d\n", sock_fd);
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    
    printf("R2: Connecting to %s:%d...\n", SERVER_IP, SERVER_PORT);
    connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    printf("R2: Connected!\n");
    
    // 2. 测试fcntl
    printf("R2: Setting socket to NON-BLOCKING...\n");
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
    
    // 3. 立即read (应该返回EAGAIN，因为对端还没发送)
    char buffer[BUFFER_SIZE];
    printf("R2: Trying immediate read() on NON-BLOCKING socket...\n");
    ssize_t n = read(sock_fd, buffer, sizeof(buffer));
    if (n < 0) {
        printf("R2: read() returned -1 (errno=%d) as expected\n", errno);
    }
    
    // 4. 改回阻塞模式
    printf("R2: Setting back to BLOCKING mode...\n");
    fcntl(sock_fd, F_SETFL, flags);
    
    // 5. 阻塞read接收消息
    printf("R2: read() waiting for message (blocking)...\n");
    n = read(sock_fd, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("R2: read() received: '%s'\n", buffer);
    }
    
    // 6. write发送回复
    const char *msg = "Hello from R2!";
    printf("R2: write() sending: '%s'\n", msg);
    write(sock_fd, msg, strlen(msg));
    
    printf("R2: Test completed successfully!\n");
    close(sock_fd);
    return 0;
}
