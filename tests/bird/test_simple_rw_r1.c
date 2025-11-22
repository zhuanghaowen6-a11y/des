// test_simple_rw_r1.c - 简化的read/write测试（服务器端）
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

#define SERVER_PORT 8888
#define BUFFER_SIZE 1024

int main() {
    printf("=== R1: Simple read/write test (server) ===\n");
    
    // 1. 创建socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("R1: Created socket fd=%d\n", listen_fd);
    
    // 2. 绑定和监听
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    server_addr.sin_port = htons(SERVER_PORT);
    
    bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(listen_fd, 5);
    printf("R1: Listening on 127.0.0.1:8888...\n");
    
    // 3. Accept连接（阻塞模式）
    printf("R1: Waiting for connection (blocking)...\n");
    int conn_fd = accept(listen_fd, NULL, NULL);
    printf("R1: Accepted connection, conn_fd=%d\n", conn_fd);
    
    // 4. 测试fcntl
    printf("R1: Setting conn_fd to NON-BLOCKING...\n");
    int flags = fcntl(conn_fd, F_GETFL, 0);
    fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK);
    
    // 5. 立即read (应该返回EAGAIN)
    char buffer[BUFFER_SIZE];
    printf("R1: Trying immediate read() on NON-BLOCKING socket...\n");
    ssize_t n = read(conn_fd, buffer, sizeof(buffer));
    if (n < 0) {
        printf("R1: read() returned -1 (errno=%d) as expected\n", errno);
    }
    
    // 6. 改回阻塞模式并发送消息
    printf("R1: Setting back to BLOCKING mode...\n");
    fcntl(conn_fd, F_SETFL, flags);
    
    const char *msg = "Hello from R1!";
    printf("R1: write() sending: '%s'\n", msg);
    write(conn_fd, msg, strlen(msg));
    
    // 7. 阻塞read接收回复
    printf("R1: read() waiting for reply (blocking)...\n");
    n = read(conn_fd, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("R1: read() received: '%s'\n", buffer);
    }
    
    printf("R1: Test completed successfully!\n");
    close(conn_fd);
    close(listen_fd);
    return 0;
}
