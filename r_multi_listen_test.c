// r_multi_listen_test.c - 测试多地址监听功能
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCKET1 "/tmp/router_socket1"
#define SOCKET2 "/tmp/router_socket2"
#define SOCKET3 "/tmp/router_socket3"

int main() {
    printf("[MULTI-LISTEN TEST] Testing multiple listen addresses...\n\n");
    
    // 清理旧的socket文件
    unlink(SOCKET1);
    unlink(SOCKET2);
    unlink(SOCKET3);
    
    // 创建并监听第一个socket
    printf("=== Creating and listening on socket 1 ===\n");
    int fd1 = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd1 < 0) {
        perror("socket1");
        return 1;
    }
    
    struct sockaddr_un addr1;
    memset(&addr1, 0, sizeof(addr1));
    addr1.sun_family = AF_UNIX;
    strncpy(addr1.sun_path, SOCKET1, sizeof(addr1.sun_path) - 1);
    
    if (bind(fd1, (struct sockaddr *)&addr1, sizeof(addr1)) < 0) {
        perror("bind1");
        return 1;
    }
    
    if (listen(fd1, 5) < 0) {
        perror("listen1");
        return 1;
    }
    printf("✓ Listening on %s (fd: %d)\n\n", SOCKET1, fd1);
    
    // 创建并监听第二个socket
    printf("=== Creating and listening on socket 2 ===\n");
    int fd2 = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd2 < 0) {
        perror("socket2");
        return 1;
    }
    
    struct sockaddr_un addr2;
    memset(&addr2, 0, sizeof(addr2));
    addr2.sun_family = AF_UNIX;
    strncpy(addr2.sun_path, SOCKET2, sizeof(addr2.sun_path) - 1);
    
    if (bind(fd2, (struct sockaddr *)&addr2, sizeof(addr2)) < 0) {
        perror("bind2");
        return 1;
    }
    
    if (listen(fd2, 5) < 0) {
        perror("listen2");
        return 1;
    }
    printf("✓ Listening on %s (fd: %d)\n\n", SOCKET2, fd2);
    
    // 创建并监听第三个socket
    printf("=== Creating and listening on socket 3 ===\n");
    int fd3 = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd3 < 0) {
        perror("socket3");
        return 1;
    }
    
    struct sockaddr_un addr3;
    memset(&addr3, 0, sizeof(addr3));
    addr3.sun_family = AF_UNIX;
    strncpy(addr3.sun_path, SOCKET3, sizeof(addr3.sun_path) - 1);
    
    if (bind(fd3, (struct sockaddr *)&addr3, sizeof(addr3)) < 0) {
        perror("bind3");
        return 1;
    }
    
    if (listen(fd3, 5) < 0) {
        perror("listen3");
        return 1;
    }
    printf("✓ Listening on %s (fd: %d)\n\n", SOCKET3, fd3);
    
    printf("=== Test: Duplicate listen (should be ignored) ===\n");
    if (listen(fd1, 5) < 0) {
        perror("listen1 again");
    } else {
        printf("✓ Duplicate listen handled\n\n");
    }
    
    printf("[MULTI-LISTEN TEST] Now listening on 3 addresses:\n");
    printf("  1. %s\n", SOCKET1);
    printf("  2. %s\n", SOCKET2);
    printf("  3. %s\n", SOCKET3);
    printf("\nWaiting for connections (press Ctrl+C to exit)...\n");
    
    // 简单的accept循环
    while (1) {
        accept(fd1, NULL, NULL);
        accept(fd2, NULL, NULL);
        accept(fd3, NULL, NULL);
    }
    //test
    close(fd1);
    close(fd2);
    close(fd3);
    return 0;
}

