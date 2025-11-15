// r1_timeout_test.c - 用于测试超时机制的R1程序
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define ROUTER_SOCKET_PATH "/tmp/router_socket"

int main() {
    printf("[R1 TEST] Starting timeout test...\n");
    
    // 创建socket
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[R1 TEST] socket");
        return 1;
    }
    printf("[R1 TEST] Created socket %d\n", sockfd);
    
    // 连接到R2
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ROUTER_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[R1 TEST] connect");
        close(sockfd);
        return 1;
    }
    printf("[R1 TEST] Connected to R2!\n");
    
    // 测试场景1: 立即发送数据（R2应该在超时前收到）
    printf("\n=== Test Case 1: Send immediately (should receive before timeout) ===\n");
    sleep(1);
    const char *msg1 = "Message before timeout";
    if (send(sockfd, msg1, strlen(msg1), 0) < 0) {
        perror("[R1 TEST] send msg1");
    } else {
        printf("[R1 TEST] Sent: %s\n", msg1);
    }
    
    // 等待R2处理
    sleep(3);
    
    // 测试场景2: 延迟发送数据（R2可能会超时）
    printf("\n=== Test Case 2: Delayed send (R2 might timeout) ===\n");
    printf("[R1 TEST] Waiting 7 seconds before sending (R2 timeout is 5 seconds)...\n");
    sleep(7);
    const char *msg2 = "Message after timeout";
    if (send(sockfd, msg2, strlen(msg2), 0) < 0) {
        perror("[R1 TEST] send msg2");
    } else {
        printf("[R1 TEST] Sent: %s\n", msg2);
    }
    
    // 等待R2处理
    sleep(2);
    
    // 测试场景3: 正常通信
    printf("\n=== Test Case 3: Normal communication ===\n");
    const char *msg3 = "Normal message";
    if (send(sockfd, msg3, strlen(msg3), 0) < 0) {
        perror("[R1 TEST] send msg3");
    } else {
        printf("[R1 TEST] Sent: %s\n", msg3);
    }
    
    sleep(2);
    
    close(sockfd);
    printf("[R1 TEST] Test completed.\n");
    return 0;
}

