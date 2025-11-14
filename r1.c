// r1.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "common.h" // 引入 common.h 以使用 ROUTER_SOCKET_PATH

// #define SOCK_PATH "/tmp/router_socket" // 不再需要，使用 common.h 中的定义
#define BUF_SIZE 1024

int main() {
    int sockfd;
    struct sockaddr_un addr;
    char buffer[BUF_SIZE];

    // 1. 创建 socket
    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(1);
    }
    printf("[R1] Created socket %d\n", sockfd);
    // 2. 设置目标地址
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ROUTER_SOCKET_PATH, sizeof(addr.sun_path) - 1); // 使用 ROUTER_SOCKET_PATH

    // 3. 连接服务端
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        exit(1);
    }

    printf("[R1] Connected to R2!\n");

    // 4. 循环发送消息
    while (1) {
        printf("[R1] Enter message: ");
        fgets(buffer, BUF_SIZE, stdin);
        buffer[strcspn(buffer, "\n")] = '\0'; // 去掉换行符

        if (strcmp(buffer, "exit") == 0)
            break;

        send(sockfd, buffer, strlen(buffer), 0);

        int n = recv(sockfd, buffer, BUF_SIZE - 1, 0);
        if (n <= 0)
            break;
        buffer[n] = '\0';
        printf("[R1] Received reply: %s\n", buffer);
    }

    close(sockfd);
    return 0;
}
