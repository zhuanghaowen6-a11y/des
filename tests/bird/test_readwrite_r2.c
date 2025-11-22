// test_readwrite_r2.c - 测试read/write和非阻塞socket行为
// 模拟BIRD的BGP客户端行为

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8888
#define BUFFER_SIZE 1024

int main() {
    printf("=== R2: BIRD-like Client (using read/write + non-blocking) ===\n");
    
    // 等待服务器启动
    sleep(1);
    
    // 1. 创建socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }
    printf("R2: Created socket fd=%d\n", sock_fd);
    
    // 2. 连接服务器
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    
    printf("R2: Connecting to %s:%d...\n", SERVER_IP, SERVER_PORT);
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }
    printf("R2: Connected successfully\n");
    
    // 3. 设置socket为非阻塞（模拟BIRD的fcntl）
    int flags = fcntl(sock_fd, F_GETFL, 0);
    printf("R2: Current flags: 0x%x\n", flags);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
    printf("R2: Set socket to NON-BLOCKING\n");
    
    // 4. 使用poll等待服务器的消息
    printf("R2: Using poll() to wait for server message...\n");
    struct pollfd poll_fds[1];
    poll_fds[0].fd = sock_fd;
    poll_fds[0].events = POLLIN;
    
    int poll_ret = poll(poll_fds, 1, 5000);
    if (poll_ret <= 0) {
        printf("R2: poll() returned %d (timeout or error)\n", poll_ret);
        close(sock_fd);
        return 1;
    }
    
    // 5. 使用read接收数据
    char buffer[BUFFER_SIZE];
    printf("R2: poll() returned, reading data...\n");
    ssize_t n = read(sock_fd, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("R2: read() received %zd bytes: '%s'\n", n, buffer);
    } else if (n < 0) {
        perror("R2: read() error");
    }
    
    // 6. 立即再次read（应该返回EAGAIN）
    printf("R2: Trying immediate read() again (should get EAGAIN)...\n");
    n = read(sock_fd, buffer, sizeof(buffer));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("R2: read() returned EAGAIN as expected\n");
        } else {
            perror("R2: read() error");
        }
    } else {
        printf("R2: Unexpected: read() returned %zd bytes\n", n);
    }
    
    // 7. 使用write发送响应
    const char *msg1 = "HELLO FROM R2 (BGP OPEN REPLY)";
    ssize_t sent = write(sock_fd, msg1, strlen(msg1));
    printf("R2: write() sent %zd bytes: '%s'\n", sent, msg1);
    
    // 8. 等待服务器的ACK
    printf("R2: Using poll() to wait for ACK...\n");
    poll_ret = poll(poll_fds, 1, 5000);
    if (poll_ret > 0) {
        n = read(sock_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            printf("R2: read() received ACK %zd bytes: '%s'\n", n, buffer);
        }
    }
    
    // 9. 发送最终消息
    const char *msg2 = "FINAL FROM R2 (BGP UPDATE)";
    sent = write(sock_fd, msg2, strlen(msg2));
    printf("R2: write() sent final message %zd bytes: '%s'\n", sent, msg2);
    
    sleep(1);
    printf("R2: Test completed successfully!\n");
    close(sock_fd);
    return 0;
}
