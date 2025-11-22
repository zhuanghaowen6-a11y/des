// test_minimal.c - 最小测试，只打印消息
#include <stdio.h>
#include <unistd.h>

int main() {
    printf("MAIN: Program started!\n");
    fflush(stdout);
    
    printf("MAIN: About to sleep 1 second...\n");
    fflush(stdout);
    
    sleep(1);
    
    printf("MAIN: Sleep completed!\n");
    fflush(stdout);
    
    printf("MAIN: Program exiting normally.\n");
    fflush(stdout);
    
    return 0;
}
