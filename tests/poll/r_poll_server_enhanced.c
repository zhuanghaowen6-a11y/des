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
    
    printf("[R2 POLL ENHANCED TEST] Starting enhanced poll test server...\n");
    
    // 1. 创建监听socket
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    printf("[R2] Created listen socket (fd=%d)\n", listen_fd);
    
    // 2. 绑定地址
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    unlink(SOCKET_PATH);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    printf("[R2] Bound to %s\n", SOCKET_PATH);
    
    // 3. 开始监听
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        return 1;
    }
    printf("[R2] Listening...\n");
    
    // 4. 接受连接
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        return 1;
    }
    printf("[R2] Accepted connection (fd=%d)\n", client_fd);
    
    printf("\n========== POLL TEST CASES ==========\n\n");
    
    // ===== 测试1: POLLOUT - 检查socket可写 =====
    printf("--- Test 1: POLLOUT (socket writable) ---\n");
    struct pollfd fds_out[1];
    fds_out[0].fd = client_fd;
    fds_out[0].events = POLLOUT;
    fds_out[0].revents = 0;
    
    int ret = poll(fds_out, 1, 2000);  // 2秒超时
    if (ret > 0) {
        printf("✓ poll() returned %d ready FD(s)\n", ret);
        if (fds_out[0].revents & POLLOUT) {
            printf("✓ POLLOUT detected: socket is writable\n");
            
            // 尝试写入数据
            const char *msg = "Hello from server!";
            ssize_t n = send(client_fd, msg, strlen(msg), 0);
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
    
    sleep(1);  // 推进虚拟时间
    
    // ===== 测试2: POLLIN - 等待数据到达 =====
    printf("\n--- Test 2: POLLIN (data available) ---\n");
    printf("[R2] Waiting for data from client...\n");
    
    struct pollfd fds_in[1];
    fds_in[0].fd = client_fd;
    fds_in[0].events = POLLIN;
    fds_in[0].revents = 0;
    
    ret = poll(fds_in, 1, 5000);  // 5秒超时
    if (ret > 0) {
        printf("✓ poll() returned %d ready FD(s)\n", ret);
        if (fds_in[0].revents & POLLIN) {
            printf("✓ POLLIN detected: data available\n");
            
            char buffer[256];
            ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
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
    
    // ===== 测试3: POLLIN | POLLOUT - 同时监听可读和可写 =====
    printf("\n--- Test 3: POLLIN | POLLOUT (both events) ---\n");
    
    struct pollfd fds_both[1];
    fds_both[0].fd = client_fd;
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
            ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
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
    
    close(client_fd);
    close(listen_fd);
    unlink(SOCKET_PATH);
    
    return 0;
}
