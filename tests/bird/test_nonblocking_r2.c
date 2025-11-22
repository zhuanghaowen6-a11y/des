// test_nonblocking_r2.c - 测试非阻塞socket行为（客户端）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#define SOCKET_PATH "\0/tmp/des_test_socket"

int main() {
    printf("\n=== R2: Non-Blocking Socket Test (Client) ===\n");
    fflush(stdout);

    // 稍微延迟，确保服务端已准备好
    sleep(1);

    // 1. 创建socket
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket failed");
        return 1;
    }
    printf("R2: socket() created, fd=%d\n", sock_fd);
    fflush(stdout);

    // 2. 连接到服务端
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, SOCKET_PATH, strlen(SOCKET_PATH) + 1);

    printf("R2: Calling connect()...\n");
    fflush(stdout);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect failed");
        close(sock_fd);
        return 1;
    }
    printf("R2: connect() success\n");
    fflush(stdout);

    // 3. 短暂延迟后发送数据（让服务端先非阻塞read得到EAGAIN）
    printf("R2: Short delay before sending data...\n");
    fflush(stdout);
    sleep(1);

    // 4. 发送数据
    const char *message = "Hello from R2";
    printf("R2: Sending message: '%s'\n", message);
    fflush(stdout);
    if (write(sock_fd, message, strlen(message)) < 0) {
        perror("write failed");
        close(sock_fd);
        return 1;
    }
    printf("R2: write() success\n");
    fflush(stdout);

    // 5. 等待服务端处理
    sleep(1);

    // 6. 接收服务端的响应
    printf("R2: Reading response from server...\n");
    fflush(stdout);
    char buf[256];
    ssize_t n = read(sock_fd, buf, sizeof(buf));
    if (n > 0) {
        buf[n] = '\0';
        printf("R2: Received %zd bytes: '%s'\n", n, buf);
    } else if (n == 0) {
        printf("R2: Connection closed by server\n");
    } else {
        perror("R2: read failed");
    }
    fflush(stdout);

    // 清理
    close(sock_fd);

    printf("\n=== R2: Non-Blocking Test Completed ===\n");
    fflush(stdout);
    return 0;
}
