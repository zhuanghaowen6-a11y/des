// test_readwrite_r1.c - 测试read/write和非阻塞socket行为
// 模拟BIRD的BGP服务器端行为

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

#define SERVER_PORT 8888
#define BUFFER_SIZE 1024

int main() {
    printf("=== R1: BIRD-like Server (using read/write + non-blocking) ===\n");
    
    // 1. 创建socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    printf("R1: Created socket fd=%d\n", listen_fd);
    
    // 2. 设置地址重用（模拟BIRD的setsockopt）
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 3. 绑定和监听
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr); // 使用127.0.0.1代替INADDR_ANY
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    printf("R1: Bound to port %d\n", SERVER_PORT);
    
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    printf("R1: Listening...\n");
    
    // 4. 设置监听socket为非阻塞（模拟BIRD的fcntl）
    int flags = fcntl(listen_fd, F_GETFL, 0);
    printf("R1: Current flags for listen_fd: 0x%x\n", flags);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    printf("R1: Set listen_fd to NON-BLOCKING\n");
    
    // 5. 使用poll等待连接
    printf("R1: Waiting for connection using poll()...\n");
    struct pollfd poll_fds[1];
    poll_fds[0].fd = listen_fd;
    poll_fds[0].events = POLLIN;
    
    int poll_ret = poll(poll_fds, 1, 10000); // 10秒超时
    if (poll_ret <= 0) {
        printf("R1: poll() returned %d (timeout or error)\n", poll_ret);
        close(listen_fd);
        return 1;
    }
    printf("R1: poll() returned, connection available\n");
    
    // 6. accept连接
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int conn_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (conn_fd < 0) {
        perror("accept");
        close(listen_fd);
        return 1;
    }
    printf("R1: Accepted connection from %s:%d, conn_fd=%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), conn_fd);
    
    // 7. 设置连接socket为非阻塞
    flags = fcntl(conn_fd, F_GETFL, 0);
    fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK);
    printf("R1: Set conn_fd to NON-BLOCKING\n");
    
    // 8. 使用write发送数据（模拟BGP OPEN消息）
    const char *msg1 = "HELLO FROM R1 (BGP OPEN)";
    ssize_t sent = write(conn_fd, msg1, strlen(msg1));
    printf("R1: write() sent %zd bytes: '%s'\n", sent, msg1);
    
    // 9. 立即尝试read（应该返回EAGAIN，因为对端还没发送）
    char buffer[BUFFER_SIZE];
    printf("R1: Trying immediate read() on non-blocking socket...\n");
    ssize_t n = read(conn_fd, buffer, sizeof(buffer));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("R1: read() returned EAGAIN as expected (no data yet)\n");
        } else {
            perror("R1: read() error");
        }
    } else {
        printf("R1: Unexpected: read() returned %zd bytes\n", n);
    }
    
    // 10. 使用poll等待数据
    printf("R1: Using poll() to wait for data...\n");
    poll_fds[0].fd = conn_fd;
    poll_fds[0].events = POLLIN;
    poll_ret = poll(poll_fds, 1, 5000);
    
    if (poll_ret > 0) {
        printf("R1: poll() returned, data available\n");
        n = read(conn_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            printf("R1: read() received %zd bytes: '%s'\n", n, buffer);
        }
    }
    
    // 11. 发送响应
    const char *msg2 = "ACK FROM R1 (BGP KEEPALIVE)";
    sent = write(conn_fd, msg2, strlen(msg2));
    printf("R1: write() sent %zd bytes: '%s'\n", sent, msg2);
    
    // 12. 再次等待数据
    sleep(1);
    poll_ret = poll(poll_fds, 1, 3000);
    if (poll_ret > 0) {
        n = read(conn_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            printf("R1: read() final message %zd bytes: '%s'\n", n, buffer);
        }
    }
    
    printf("R1: Test completed successfully!\n");
    close(conn_fd);
    close(listen_fd);
    return 0;
}
