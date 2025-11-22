// test_nonblock_simple_r1.c - 简单的非阻塞测试（不使用poll）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#define SOCKET_PATH "\0/tmp/des_test_socket"

int main() {
    printf("\n=== R1: Simple Non-Blocking Test ===\n");
    fflush(stdout);

    // 创建并监听
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, SOCKET_PATH, strlen(SOCKET_PATH) + 1);
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 1);
    printf("R1: Listening...\n");
    fflush(stdout);

    // Accept连接
    int client_fd = accept(server_fd, NULL, NULL);
    printf("R1: Connected, client_fd=%d\n", client_fd);
    fflush(stdout);

    // 设置非阻塞
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    printf("R1: Set to non-blocking mode\n");
    fflush(stdout);

    // Test 1: 立即read（无数据，应返回EAGAIN）
    printf("\n--- Test 1: read() with NO data ---\n");
    fflush(stdout);
    char buf[256];
    ssize_t n = read(client_fd, buf, sizeof(buf));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        printf("✓ Test 1 PASSED: EAGAIN when no data\n");
    } else {
        printf("✗ Test 1 FAILED: got %zd bytes or wrong errno\n", n);
    }
    fflush(stdout);

    // Test 2: 等待数据到达（使用简单的sleep）
    printf("\n--- Waiting for data (sleep 3s) ---\n");
    fflush(stdout);
    sleep(3);

    // Test 3: 现在read（应该有数据）
    printf("\n--- Test 2: read() WITH data ---\n");
    fflush(stdout);
    n = read(client_fd, buf, sizeof(buf));
    if (n > 0) {
        buf[n] = '\0';
        printf("✓ Test 2 PASSED: received %zd bytes: '%s'\n", n, buf);
    } else {
        printf("✗ Test 2 FAILED: got %zd bytes (errno=%d: %s)\n", n, errno, strerror(errno));
    }
    fflush(stdout);

    // Test 3: 再次read（应该又是EAGAIN）
    printf("\n--- Test 3: read() again (no more data) ---\n");
    fflush(stdout);
    n = read(client_fd, buf, sizeof(buf));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        printf("✓ Test 3 PASSED: EAGAIN after reading all data\n");
    } else {
        printf("✗ Test 3 FAILED: got %zd bytes or wrong errno\n", n);
    }
    fflush(stdout);

    // 发送响应
    write(client_fd, "ACK", 3);
    close(client_fd);
    close(server_fd);

    printf("\n=== R1: Test Completed ===\n");
    fflush(stdout);
    return 0;
}
