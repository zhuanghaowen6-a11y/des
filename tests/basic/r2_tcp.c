// r2_tcp.c - TCP版本的服务端
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

int main() {
    int listen_fd, conn_fd;
    struct sockaddr_in addr, client_addr;
    socklen_t client_len;
    char buffer[BUF_SIZE];
    int reuse = 1;

    // 1. 创建TCP socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }
    printf("[R2-TCP] Created socket %d\n", listen_fd);

    // 设置SO_REUSEADDR，避免"Address already in use"错误
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        exit(1);
    }

    // 2. 绑定端口
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有接口
    addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        exit(1);
    }
    printf("[R2-TCP] Bound to port %d\n", SERVER_PORT);

    // 3. 监听
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        exit(1);
    }

    printf("[R2-TCP] Listening on 0.0.0.0:%d...\n", SERVER_PORT);

    // 4. 等待连接
    client_len = sizeof(client_addr);
    conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (conn_fd < 0) {
        perror("accept");
        close(listen_fd);
        exit(1);
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    printf("[R2-TCP] Connection established with R1 from %s:%d (fd: %d)\n", 
           client_ip, ntohs(client_addr.sin_port), conn_fd);

    // 5. 接收并回复消息
    while (1) {
        int n = recv(conn_fd, buffer, BUF_SIZE - 1, 0);
        if (n <= 0) {
            if (n < 0) {
                perror("recv");
            }
            printf("[R2-TCP] Connection closed.\n");
            break;
        }
        buffer[n] = '\0';
        printf("[R2-TCP] Received %d bytes: %s\n", n, buffer);

        // 回复
        char reply[BUF_SIZE];
        size_t header_len = strlen("R2-TCP ACK: ");
        size_t max_buffer_len = sizeof(reply) - 1 - header_len;
        snprintf(reply, sizeof(reply), "R2-TCP ACK: %.*s", (int)max_buffer_len, buffer);
        
        ssize_t sent = send(conn_fd, reply, strlen(reply), 0);
        if (sent < 0) {
            perror("send");
            break;
        }
        printf("[R2-TCP] Sent reply (%ld bytes)\n", sent);
    }

    close(conn_fd);
    close(listen_fd);
    printf("[R2-TCP] Sockets closed\n");
    return 0;
}
