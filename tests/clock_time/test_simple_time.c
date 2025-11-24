// test_simple_time.c - 最简单的时间测试
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main() {
    printf("Starting simple time test...\n");
    fflush(stdout);
    
    printf("Calling clock_gettime...\n");
    fflush(stdout);
    
    struct timespec ts;
    int ret = clock_gettime(CLOCK_MONOTONIC, &ts);
    
    printf("clock_gettime returned: %d\n", ret);
    printf("Time: %ld.%09ld\n", ts.tv_sec, ts.tv_nsec);
    fflush(stdout);
    
    return 0;
}
