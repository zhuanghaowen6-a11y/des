// r2.c
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
    int listen_fd, conn_fd;
    struct sockaddr_un addr;
    char buffer[BUF_SIZE];

    // 如果旧的socket文件存在，删除它
    unlink(ROUTER_SOCKET_PATH); // 使用 ROUTER_SOCKET_PATH

    // 1. 创建socket
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    // 2. 绑定socket文件路径
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ROUTER_SOCKET_PATH, sizeof(addr.sun_path) - 1); // 使用 ROUTER_SOCKET_PATH

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        exit(1);
    }

    // 3. 监听
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        exit(1);
    }

    printf("[R2] Listening on %s...\n", ROUTER_SOCKET_PATH); // 使用 ROUTER_SOCKET_PATH

    // 4. 等待连接
    conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        perror("accept");
        close(listen_fd);
        exit(1);
    }
    printf("[R2] Connection established with R1!\n");

    // 5. 接收并回复消息
    while (1) {
        int n = recv(conn_fd, buffer, BUF_SIZE - 1, 0);
        if (n <= 0) {
            printf("[R2] Connection closed.\n");
            break;
        }
        buffer[n] = '\0';
        printf("[R2] Received: %s\n", buffer);

        // 回复
        char reply[BUF_SIZE];
        size_t header_len = strlen("R2 ACK: ");
        size_t max_buffer_len = sizeof(reply) - 1 - header_len; // -1 for null terminator
        snprintf(reply, sizeof(reply), "R2 ACK: %.*s", (int)max_buffer_len, buffer);
        printf("R2 received: %s\n", buffer);
        send(conn_fd, reply, strlen(reply) + 1, 0); // +1 to send null terminator
    }

    close(conn_fd);
    close(listen_fd);
    unlink(ROUTER_SOCKET_PATH); // 使用 ROUTER_SOCKET_PATH
    return 0;
}
