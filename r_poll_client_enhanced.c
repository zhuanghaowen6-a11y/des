#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <errno.h>

#define SOCKET_PATH "/tmp/router_socket"

int main() {
    setbuf(stdout, NULL);  // 禁用输出缓冲
    setbuf(stderr, NULL);
    
    printf("[R1 POLL ENHANCED TEST] Starting enhanced poll test client...\n");
    
    sleep(1);  // 等待服务器启动
    
    // 1. 创建socket
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }
    printf("[R1] Created socket (fd=%d)\n", sockfd);
    
    // 2. 连接到服务器
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("[R1] Connected to server\n");
    
    sleep(1);  // 让服务器处理测试1
    
    printf("\n========== CLIENT TEST CASES ==========\n\n");
    
    // ===== 测试1: POLLIN - 等待服务器发送的数据 =====
    printf("--- Test 1: POLLIN (waiting for server message) ---\n");
    
    struct pollfd fds_in[1];
    fds_in[0].fd = sockfd;
    fds_in[0].events = POLLIN;
    fds_in[0].revents = 0;
    
    int ret = poll(fds_in, 1, 5000);  // 5秒超时
    if (ret > 0) {
        printf("✓ poll() returned %d ready FD(s)\n", ret);
        if (fds_in[0].revents & POLLIN) {
            printf("✓ POLLIN detected: data available\n");
            
            char buffer[256];
            ssize_t n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                printf("✓ Received %zd bytes: \"%s\"\n", n, buffer);
            }
        }
    } else if (ret == 0) {
        printf("✗ poll() timed out\n");
    } else {
        perror("poll");
    }
    
    sleep(1);
    
    // ===== 测试2: POLLOUT - 检查socket可写并发送数据 =====
    printf("\n--- Test 2: POLLOUT (socket writable) ---\n");
    
    struct pollfd fds_out[1];
    fds_out[0].fd = sockfd;
    fds_out[0].events = POLLOUT;
    fds_out[0].revents = 0;
    
    ret = poll(fds_out, 1, 2000);
    if (ret > 0) {
        printf("✓ poll() returned %d ready FD(s)\n", ret);
        if (fds_out[0].revents & POLLOUT) {
            printf("✓ POLLOUT detected: socket is writable\n");
            
            // 发送数据给服务器
            const char *msg = "Hello from client!";
            ssize_t n = send(sockfd, msg, strlen(msg), 0);
            if (n > 0) {
                printf("✓ Sent %zd bytes: \"%s\"\n", n, msg);
            }
        }
        if (fds_out[0].revents & POLLERR) {
            printf("✗ POLLERR detected\n");
        }
        if (fds_out[0].revents & POLLHUP) {
            printf("✗ POLLHUP detected\n");
        }
    } else if (ret == 0) {
        printf("✗ poll() timed out\n");
    } else {
        perror("poll");
    }
    
    sleep(1);
    
    // ===== 测试3: POLLIN | POLLOUT - 同时监听可读和可写 =====
    printf("\n--- Test 3: POLLIN | POLLOUT (both events) ---\n");
    
    // 先发送一条消息给服务器
    const char *msg2 = "Test message for combined poll";
    send(sockfd, msg2, strlen(msg2), 0);
    printf("[R1] Sent message to server\n");
    
    sleep(1);  // 让消息到达
    
    struct pollfd fds_both[1];
    fds_both[0].fd = sockfd;
    fds_both[0].events = POLLIN | POLLOUT;
    fds_both[0].revents = 0;
    
    ret = poll(fds_both, 1, 5000);
    if (ret > 0) {
        printf("✓ poll() returned %d ready FD(s)\n", ret);
        printf("  revents = 0x%04x (", fds_both[0].revents);
        if (fds_both[0].revents & POLLIN) printf("POLLIN ");
        if (fds_both[0].revents & POLLOUT) printf("POLLOUT ");
        if (fds_both[0].revents & POLLERR) printf("POLLERR ");
        if (fds_both[0].revents & POLLHUP) printf("POLLHUP ");
        printf(")\n");
        
        if (fds_both[0].revents & POLLOUT) {
            printf("✓ Socket is writable\n");
        }
        if (fds_both[0].revents & POLLIN) {
            printf("✓ Data is available\n");
            char buffer[256];
            ssize_t n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                printf("  Received: \"%s\"\n", buffer);
            }
        }
    } else if (ret == 0) {
        printf("✗ poll() timed out\n");
    } else {
        perror("poll");
    }
    
    printf("\n========== TEST COMPLETED ==========\n");
    
    close(sockfd);
    
    return 0;
}
