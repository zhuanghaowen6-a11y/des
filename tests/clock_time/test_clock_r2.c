// test_clock_r2.c - 测试clock_gettime虚拟时间功能（客户端）
// 连接到R1并触发通信事件

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define PORT 8888

void print_time(const char* label) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        printf("[R2 %s] Virtual Time: %ld.%09ld (%.6f seconds)\n", 
               label, ts.tv_sec, ts.tv_nsec, 
               ts.tv_sec + ts.tv_nsec / 1000000000.0);
    } else {
        perror("clock_gettime failed");
    }
    fflush(stdout);
}

int main() {
    printf("=== R2: Clock Virtual Time Test (Client) ===\n");
    fflush(stdout);
    
    // 测试1：启动后立即查询时间
    printf("\n[Test 1] Initial time after startup:\n");
    print_time("INIT");
    
    // 等待一下，让R1先启动
    printf("\n[R2] Waiting 1 second for server to start...\n");
    sleep(1);
    print_time("AFTER_WAIT");
    
    // 测试2：创建socket
    printf("\n[Test 2] Creating socket:\n");
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("[R2] Created socket fd=%d\n", sock_fd);
    print_time("SOCKET");
    
    // 测试3：连接到服务器
    printf("\n[Test 3] Connecting to server:\n");
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);
    
    printf("[R2] Calling connect()...\n");
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("[R2] Connected!\n");
    print_time("CONNECT");
    
    // 测试4：发送数据
    printf("\n[Test 4] Sending data:\n");
    const char* msg = "Hello from R2";
    printf("[R2] Sending: '%s'\n", msg);
    ssize_t sent = send(sock_fd, msg, strlen(msg), 0);
    printf("[R2] Sent %zd bytes\n", sent);
    print_time("SEND");
    
    // 测试5：接收回复
    printf("\n[Test 5] Receiving reply:\n");
    char buffer[256];
    ssize_t received = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);
    if (received > 0) {
        buffer[received] = '\0';
        printf("[R2] Received: '%s'\n", buffer);
        print_time("RECV");
    }
    
    // 测试6：连续查询时间
    printf("\n[Test 6] Multiple consecutive time queries:\n");
    for (int i = 0; i < 5; i++) {
        char label[32];
        snprintf(label, sizeof(label), "QUERY-%d", i);
        print_time(label);
    }
    
    // 测试7：关闭连接前查询时间
    printf("\n[Test 7] Before closing:\n");
    print_time("BEFORE_CLOSE");
    
    close(sock_fd);
    
    // 测试8：最终查询
    printf("\n[Test 8] Final time:\n");
    print_time("FINAL");
    
    printf("\n=== R2: Test Completed ===\n");
    fflush(stdout);
    return 0;
}
