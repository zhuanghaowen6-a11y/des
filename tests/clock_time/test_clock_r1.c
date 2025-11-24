// test_clock_r1.c - 测试clock_gettime虚拟时间功能
// 这个程序会多次查询时间，并触发一些事件让虚拟时间推进

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
        printf("[R1 %s] Virtual Time: %ld.%09ld (%.6f seconds)\n", 
               label, ts.tv_sec, ts.tv_nsec, 
               ts.tv_sec + ts.tv_nsec / 1000000000.0);
    } else {
        perror("clock_gettime failed");
    }
    fflush(stdout);
}

int main() {
    printf("=== R1: Clock Virtual Time Test ===\n");
    fflush(stdout);
    
    // 测试1：启动后立即查询时间（应该是0或接近0）
    printf("\n[Test 1] Initial time after startup:\n");
    print_time("INIT");
    
    // 测试2：创建socket（触发事件）
    printf("\n[Test 2] After creating socket:\n");
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("[R1] Created listen socket fd=%d\n", listen_fd);
    print_time("SOCKET");
    
    // 测试3：bind和listen（触发listen事件）
    printf("\n[Test 3] After bind and listen:\n");
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        return 1;
    }
    printf("[R1] Listening on port %d\n", PORT);
    print_time("LISTEN");
    
    // 测试4：sleep一段时间（让虚拟时间推进）
    printf("\n[Test 4] After sleep(2):\n");
    printf("[R1] Sleeping for 2 seconds...\n");
    sleep(2);
    print_time("SLEEP");
    
    // 测试5：连续多次查询时间（验证一致性）
    printf("\n[Test 5] Multiple consecutive time queries:\n");
    for (int i = 0; i < 5; i++) {
        char label[32];
        snprintf(label, sizeof(label), "QUERY-%d", i);
        print_time(label);
    }
    
    // 测试6：accept等待连接（会阻塞，让虚拟时间推进）
    printf("\n[Test 6] Waiting for connection (will timeout after 5s):\n");
    printf("[R1] Calling accept()...\n");
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_fd > 0) {
        printf("[R1] Accepted connection from client, fd=%d\n", client_fd);
        print_time("ACCEPT");
        
        // 测试7：接收数据后查询时间
        printf("\n[Test 7] After receiving data:\n");
        char buffer[256];
        ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            buffer[n] = '\0';
            printf("[R1] Received: '%s'\n", buffer);
            print_time("RECV");
            
            // 发送回复
            const char* reply = "Hello from R1";
            send(client_fd, reply, strlen(reply), 0);
            print_time("SEND");
        }
        
        close(client_fd);
    } else {
        printf("[R1] Accept failed or timed out\n");
        print_time("ACCEPT_FAIL");
    }
    
    // 测试8：最后查询时间
    printf("\n[Test 8] Final time before exit:\n");
    print_time("FINAL");
    
    close(listen_fd);
    
    printf("\n=== R1: Test Completed ===\n");
    fflush(stdout);
    return 0;
}
