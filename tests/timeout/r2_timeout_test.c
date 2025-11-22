// r2_timeout_test.c - 用于测试超时机制的R2程序
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <errno.h>

#define ROUTER_SOCKET_PATH "/tmp/router_socket"

// 带超时的recv函数
ssize_t recv_with_timeout(int sockfd, void *buf, size_t len, int timeout_seconds) {
    fd_set read_fds;
    struct timeval timeout;
    
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);
    
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;
    
    printf("[R2 TEST] Waiting for data with %d seconds timeout...\n", timeout_seconds);
    int ret = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);
    
    if (ret < 0) {
        perror("[R2 TEST] select error");
        return -1;
    } else if (ret == 0) {
        printf("[R2 TEST] ⏰ TIMEOUT! No data received within %d seconds.\n", timeout_seconds);
        errno = EAGAIN;
        return -1;
    } else {
        // 数据就绪，调用recv
        printf("[R2 TEST] ✓ Data available before timeout, receiving...\n");
        return recv(sockfd, buf, len, 0);
    }
}

int main() {
    printf("[R2 TEST] Starting timeout test server...\n");
    
    // 删除旧的socket文件
    unlink(ROUTER_SOCKET_PATH);
    
    // 创建socket
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[R2 TEST] socket");
        return 1;
    }
    
    // 绑定
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ROUTER_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[R2 TEST] bind");
        close(listen_fd);
        return 1;
    }
    
    // 监听
    if (listen(listen_fd, 5) < 0) {
        perror("[R2 TEST] listen");
        close(listen_fd);
        return 1;
    }
    printf("[R2 TEST] Listening on %s...\n", ROUTER_SOCKET_PATH);
    
    // 接受连接
    int conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        perror("[R2 TEST] accept");
        close(listen_fd);
        return 1;
    }
    printf("[R2 TEST] Connection established with R1!\n");
    
    char buffer[1024];
    
    // 测试场景1: 等待数据（5秒超时，R1会在1秒后发送）
    printf("\n=== Test Case 1: recv with 5s timeout (R1 sends after 1s) ===\n");
    memset(buffer, 0, sizeof(buffer));
    ssize_t n = recv_with_timeout(conn_fd, buffer, sizeof(buffer) - 1, 5);
    if (n > 0) {
        buffer[n] = '\0';
        printf("[R2 TEST] ✓ Received before timeout: %s\n", buffer);
    } else {
        printf("[R2 TEST] ✗ Recv failed or timed out\n");
    }
    
    // 测试场景2: 等待数据（5秒超时，R1会在7秒后发送，应该超时）
    printf("\n=== Test Case 2: recv with 5s timeout (R1 sends after 7s) ===\n");
    sleep(2);  // 确保和R1同步
    memset(buffer, 0, sizeof(buffer));
    n = recv_with_timeout(conn_fd, buffer, sizeof(buffer) - 1, 5);
    if (n > 0) {
        buffer[n] = '\0';
        printf("[R2 TEST] ✓ Received before timeout: %s\n", buffer);
    } else {
        printf("[R2 TEST] ✗ Recv timed out as expected!\n");
    }
    
    // 超时后，尝试立即再次接收（应该能收到延迟的消息）
    printf("[R2 TEST] Trying to receive the delayed message...\n");
    sleep(3);  // 等待R1的延迟消息
    memset(buffer, 0, sizeof(buffer));
    n = recv_with_timeout(conn_fd, buffer, sizeof(buffer) - 1, 5);
    if (n > 0) {
        buffer[n] = '\0';
        printf("[R2 TEST] ✓ Received delayed message: %s\n", buffer);
    }
    
    // 测试场景3: 正常接收
    printf("\n=== Test Case 3: Normal recv (no timeout test) ===\n");
    memset(buffer, 0, sizeof(buffer));
    n = recv_with_timeout(conn_fd, buffer, sizeof(buffer) - 1, 5);
    if (n > 0) {
        buffer[n] = '\0';
        printf("[R2 TEST] ✓ Received: %s\n", buffer);
    }
    
    close(conn_fd);
    close(listen_fd);
    printf("[R2 TEST] Test completed.\n");
    return 0;
}

