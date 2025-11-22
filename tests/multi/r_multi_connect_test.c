// r_multi_connect_test.c - 测试连接到多个监听地址
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int test_connect(const char *path) {
    printf("Attempting to connect to %s...\n", path);
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    
    printf("✓ Successfully connected to %s (fd: %d)\n\n", path, fd);
    return fd;
}

int main() {
    printf("[MULTI-CONNECT TEST] Testing connections to multiple addresses...\n\n");
    
    sleep(1);  // 等待服务器准备好
    
    // 测试连接到第一个地址
    printf("=== Test 1: Connect to /tmp/router_socket1 ===\n");
    int fd1 = test_connect("/tmp/router_socket1");
    if (fd1 < 0) {
        printf("✗ Failed to connect to socket1\n\n");
    }
    
    sleep(1);
    
    // 测试连接到第二个地址
    printf("=== Test 2: Connect to /tmp/router_socket2 ===\n");
    int fd2 = test_connect("/tmp/router_socket2");
    if (fd2 < 0) {
        printf("✗ Failed to connect to socket2\n\n");
    }
    
    sleep(1);
    
    // 测试连接到第三个地址
    printf("=== Test 3: Connect to /tmp/router_socket3 ===\n");
    int fd3 = test_connect("/tmp/router_socket3");
    if (fd3 < 0) {
        printf("✗ Failed to connect to socket3\n\n");
    }
    
    printf("[MULTI-CONNECT TEST] All connection tests completed!\n");
    printf("Summary:\n");
    printf("  Socket1: %s\n", fd1 >= 0 ? "✓ Connected" : "✗ Failed");
    printf("  Socket2: %s\n", fd2 >= 0 ? "✓ Connected" : "✗ Failed");
    printf("  Socket3: %s\n", fd3 >= 0 ? "✓ Connected" : "✗ Failed");
    
    // 清理
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    if (fd3 >= 0) close(fd3);
    
    return 0;
}

