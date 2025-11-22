// test_nonblocking_r1.c - 测试非阻塞socket行为（服务端）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>

#define SOCKET_PATH "\0/tmp/des_test_socket"

int main() {
    printf("\n=== R1: Non-Blocking Socket Test (Server) ===\n");
    fflush(stdout);

    // 1. 创建socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return 1;
    }
    printf("R1: socket() created, fd=%d\n", server_fd);
    fflush(stdout);

    // 2. 绑定和监听
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, SOCKET_PATH, strlen(SOCKET_PATH) + 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return 1;
    }
    printf("R1: bind() success\n");
    fflush(stdout);

    if (listen(server_fd, 1) < 0) {
        perror("listen failed");
        close(server_fd);
        return 1;
    }
    printf("R1: listen() success\n");
    fflush(stdout);

    // 3. Accept连接
    printf("R1: Calling accept()...\n");
    fflush(stdout);
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept failed");
        close(server_fd);
        return 1;
    }
    printf("R1: accept() success, client_fd=%d\n", client_fd);
    fflush(stdout);

    // 4. 设置为非阻塞模式
    int flags = fcntl(client_fd, F_GETFL, 0);
    printf("R1: fcntl(F_GETFL) returned flags=0x%x\n", flags);
    fflush(stdout);

    if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL failed");
        close(client_fd);
        close(server_fd);
        return 1;
    }
    printf("R1: fcntl(F_SETFL, O_NONBLOCK) success\n");
    fflush(stdout);

    // 5. 立即尝试read（应该返回EAGAIN，因为还没有数据）
    printf("\n--- Test 1: read() on non-blocking socket with NO data ---\n");
    fflush(stdout);
    char buf[256];
    ssize_t n = read(client_fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("✓ R1: read() correctly returned EAGAIN (no data available)\n");
        } else {
            printf("✗ R1: read() returned error: %s (expected EAGAIN)\n", strerror(errno));
        }
    } else {
        printf("✗ R1: read() returned %zd bytes (expected EAGAIN)\n", n);
    }
    fflush(stdout);

    // 6. 使用 poll() 等待数据（正确的非阻塞 I/O 模式）
    printf("\nR1: Using poll() to wait for data...\n");
    fflush(stdout);
    
    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    
    int ret = poll(&pfd, 1, 2000); // 2秒超时
    if (ret < 0) {
        perror("poll failed");
    } else if (ret == 0) {
        printf("✗ R1: poll() timeout (no data received)\n");
    } else {
        printf("✓ R1: poll() returned, fd is readable\n");
    }
    fflush(stdout);

    // 7. 再次尝试read（现在应该有数据）
    printf("\n--- Test 2: read() on non-blocking socket WITH data ---\n");
    fflush(stdout);
    n = read(client_fd, buf, sizeof(buf));
    if (n > 0) {
        buf[n] = '\0';
        printf("✓ R1: read() success, received %zd bytes: '%s'\n", n, buf);
    } else if (n == 0) {
        printf("✗ R1: read() returned 0 (connection closed)\n");
    } else {
        printf("✗ R1: read() failed: %s\n", strerror(errno));
    }
    fflush(stdout);

    // 8. 立即再次read（应该又返回EAGAIN）
    printf("\n--- Test 3: read() again (should be EAGAIN) ---\n");
    fflush(stdout);
    n = read(client_fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("✓ R1: read() correctly returned EAGAIN (no more data)\n");
        } else {
            printf("✗ R1: read() returned error: %s (expected EAGAIN)\n", strerror(errno));
        }
    } else {
        printf("✗ R1: read() returned %zd bytes (expected EAGAIN)\n", n);
    }
    fflush(stdout);

    // 9. 测试阻塞模式恢复
    printf("\n--- Test 4: Switch back to blocking mode ---\n");
    fflush(stdout);
    flags = fcntl(client_fd, F_GETFL, 0);
    if (fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL failed");
    } else {
        printf("✓ R1: Switched back to blocking mode\n");
    }
    fflush(stdout);

    // 10. 发送响应给客户端
    const char *response = "ACK from R1";
    if (write(client_fd, response, strlen(response)) < 0) {
        perror("write failed");
    } else {
        printf("R1: Sent response to client\n");
    }
    fflush(stdout);

    // 清理
    close(client_fd);
    close(server_fd);

    printf("\n=== R1: Non-Blocking Test Completed ===\n");
    fflush(stdout);
    return 0;
}
