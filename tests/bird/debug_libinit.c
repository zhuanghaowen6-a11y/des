// debug_libinit.c - 调试lib_init是否被调用多次
#include <stdio.h>
#include <unistd.h>

int main() {
    printf("=== MAIN START ===\n");
    fflush(stdout);
    
    printf("main: pid=%d\n", getpid());
    fflush(stdout);
    
    printf("main: Calling sleep(1)...\n");
    fflush(stdout);
    
    sleep(1);
    
    printf("main: Sleep done\n");
    fflush(stdout);
    
    printf("=== MAIN END ===\n");
    fflush(stdout);
    
    return 0;
}
