// r1_tcp.c - TCP版本的客户端
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "common.h"

#define BUF_SIZE 1024
#define SERVER_PORT 5000
#define SERVER_IP "127.0.0.1"

int main() {
    int sockfd;
    struct sockaddr_in addr;
    char buffer[BUF_SIZE];

    // 1. 创建 TCP socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(1);
    }
    printf("[R1-TCP] Created socket %d\n", sockfd);

    // 2. 设置目标地址
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        exit(1);
    }

    // 3. 连接服务端
    printf("[R1-TCP] Connecting to %s:%d...\n", SERVER_IP, SERVER_PORT);
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        exit(1);
    }

    printf("[R1-TCP] Connected to R2!\n");

    // 4. 循环发送消息
    while (1) {
        printf("[R1-TCP] Enter message: ");
        fflush(stdout);
        
        if (fgets(buffer, BUF_SIZE, stdin) == NULL) {
            break;
        }
        
        buffer[strcspn(buffer, "\n")] = '\0'; // 去掉换行符

        if (strcmp(buffer, "exit") == 0) {
            printf("[R1-TCP] Exiting...\n");
            break;
        }

        // 发送消息
        ssize_t sent = send(sockfd, buffer, strlen(buffer), 0);
        if (sent < 0) {
            perror("send");
            break;
        }
        printf("[R1-TCP] Sent %ld bytes\n", sent);

        // 接收回复
        int n = recv(sockfd, buffer, BUF_SIZE - 1, 0);
        if (n <= 0) {
            if (n < 0) {
                perror("recv");
            }
            printf("[R1-TCP] Connection closed\n");
            break;
        }
        buffer[n] = '\0';
        printf("[R1-TCP] Received reply: %s\n", buffer);
    }

    close(sockfd);
    printf("[R1-TCP] Socket closed\n");
    return 0;
}
