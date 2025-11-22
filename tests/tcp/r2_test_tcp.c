#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_PORT 5000
#define BUFFER_SIZE 1024

int main() {
    printf("[R2_TCP] Starting TCP server test...\n");
    
    // 1. 创建 TCP socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[R2_TCP ERROR] socket");
        exit(1);
    }
    printf("[R2_TCP] Created TCP socket (fd: %d)\n", listen_fd);
    
    // 设置地址重用
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("[R2_TCP WARNING] setsockopt");
    }
    
    // 2. Bind 到 127.0.0.1:5000
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[R2_TCP ERROR] bind");
        close(listen_fd);
        exit(1);
    }
    printf("[R2_TCP] Bound to 127.0.0.1:%d\n", SERVER_PORT);
    
    // 3. Listen
    if (listen(listen_fd, 5) < 0) {
        perror("[R2_TCP ERROR] listen");
        close(listen_fd);
        exit(1);
    }
    printf("[R2_TCP] Listening on 127.0.0.1:%d (DESD controlled)...\n", SERVER_PORT);
    
    // 4. Accept 连接（阻塞在 desd）
    printf("[R2_TCP] Waiting for client connection...\n");
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int conn_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len);
    if (conn_fd < 0) {
        perror("[R2_TCP ERROR] accept");
        close(listen_fd);
        exit(1);
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("[R2_TCP] Accepted connection from %s:%d (fd: %d)\n", 
           client_ip, ntohs(client_addr.sin_port), conn_fd);
    
    // 5. 接收数据
    printf("[R2_TCP] Waiting to receive data...\n");
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received = recv(conn_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received < 0) {
        perror("[R2_TCP ERROR] recv");
        close(conn_fd);
        close(listen_fd);
        exit(1);
    }
    buffer[bytes_received] = '\0';
    printf("[R2_TCP] Received %zd bytes: \"%s\"\n", bytes_received, buffer);
    
    // 6. 发送响应
    const char *response = "Hello from R2 (TCP Server)!";
    ssize_t bytes_sent = send(conn_fd, response, strlen(response), 0);
    if (bytes_sent < 0) {
        perror("[R2_TCP ERROR] send");
        close(conn_fd);
        close(listen_fd);
        exit(1);
    }
    printf("[R2_TCP] Sent response: \"%s\" (%zd bytes)\n", response, bytes_sent);
    
    // 7. 再接收一次数据
    printf("[R2_TCP] Waiting to receive second message...\n");
    bytes_received = recv(conn_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received < 0) {
        perror("[R2_TCP ERROR] recv");
    } else if (bytes_received == 0) {
        printf("[R2_TCP] Connection closed by client.\n");
    } else {
        buffer[bytes_received] = '\0';
        printf("[R2_TCP] Received second message: \"%s\" (%zd bytes)\n", buffer, bytes_received);
        
        // 发送最终响应
        const char *final_response = "Goodbye from R2!";
        bytes_sent = send(conn_fd, final_response, strlen(final_response), 0);
        if (bytes_sent < 0) {
            perror("[R2_TCP ERROR] send");
        } else {
            printf("[R2_TCP] Sent final response: \"%s\" (%zd bytes)\n", final_response, bytes_sent);
        }
    }
    
    // 8. 清理
    printf("[R2_TCP] Closing connections...\n");
    close(conn_fd);
    close(listen_fd);
    printf("[R2_TCP] TCP server test completed successfully!\n");
    
    return 0;
}

