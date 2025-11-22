// test_nonblock_simple_r2.c - 简单的非阻塞测试客户端
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCKET_PATH "\0/tmp/des_test_socket"

int main() {
    printf("\n=== R2: Simple Non-Blocking Test Client ===\n");
    fflush(stdout);

    // 延迟确保服务端先启动
    sleep(1);

    // 连接到服务端
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, SOCKET_PATH, strlen(SOCKET_PATH) + 1);
    connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr));
    printf("R2: Connected\n");
    fflush(stdout);

    // 延迟后发送数据（让R1先尝试read得到EAGAIN）
    printf("R2: Waiting 2 seconds before sending...\n");
    fflush(stdout);
    sleep(2);

    // 发送数据
    const char *msg = "Hello from R2";
    write(sock_fd, msg, strlen(msg));
    printf("R2: Sent message\n");
    fflush(stdout);

    // 接收响应
    sleep(1);
    char buf[64];
    ssize_t n = read(sock_fd, buf, sizeof(buf));
    if (n > 0) {
        buf[n] = '\0';
        printf("R2: Received '%s'\n", buf);
    }
    fflush(stdout);

    close(sock_fd);
    printf("\n=== R2: Test Completed ===\n");
    fflush(stdout);
    return 0;
}
