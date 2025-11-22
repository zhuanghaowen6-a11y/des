#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 5000
#define BUFFER_SIZE 1024

int main() {
    printf("[R1_TCP] Starting TCP client test...\n");
    
    // 1. 创建 TCP socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[R1_TCP ERROR] socket");
        exit(1);
    }
    printf("[R1_TCP] Created TCP socket (fd: %d)\n", sock_fd);
    
    // 2. Connect 到服务器 127.0.0.1:5000（阻塞在 desd）
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    server_addr.sin_port = htons(SERVER_PORT);
    
    printf("[R1_TCP] Connecting to %s:%d (DESD controlled)...\n", SERVER_IP, SERVER_PORT);
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[R1_TCP ERROR] connect");
        close(sock_fd);
        exit(1);
    }
    printf("[R1_TCP] Connected to %s:%d successfully!\n", SERVER_IP, SERVER_PORT);
    
    // 3. 发送第一条消息
    const char *message1 = "Hello from R1 (TCP Client)!";
    ssize_t bytes_sent = send(sock_fd, message1, strlen(message1), 0);
    if (bytes_sent < 0) {
        perror("[R1_TCP ERROR] send");
        close(sock_fd);
        exit(1);
    }
    printf("[R1_TCP] Sent message: \"%s\" (%zd bytes)\n", message1, bytes_sent);
    
    // 4. 接收响应
    printf("[R1_TCP] Waiting for response...\n");
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received = recv(sock_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received < 0) {
        perror("[R1_TCP ERROR] recv");
        close(sock_fd);
        exit(1);
    }
    buffer[bytes_received] = '\0';
    printf("[R1_TCP] Received response: \"%s\" (%zd bytes)\n", buffer, bytes_received);
    
    // 5. 发送第二条消息
    const char *message2 = "Thank you, R2! Goodbye!";
    bytes_sent = send(sock_fd, message2, strlen(message2), 0);
    if (bytes_sent < 0) {
        perror("[R1_TCP ERROR] send");
        close(sock_fd);
        exit(1);
    }
    printf("[R1_TCP] Sent second message: \"%s\" (%zd bytes)\n", message2, bytes_sent);
    
    // 6. 接收最终响应
    printf("[R1_TCP] Waiting for final response...\n");
    bytes_received = recv(sock_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received < 0) {
        perror("[R1_TCP ERROR] recv");
    } else if (bytes_received == 0) {
        printf("[R1_TCP] Connection closed by server.\n");
    } else {
        buffer[bytes_received] = '\0';
        printf("[R1_TCP] Received final response: \"%s\" (%zd bytes)\n", buffer, bytes_received);
    }
    
    // 7. 清理
    printf("[R1_TCP] Closing connection...\n");
    close(sock_fd);
    printf("[R1_TCP] TCP client test completed successfully!\n");
    
    return 0;
}

