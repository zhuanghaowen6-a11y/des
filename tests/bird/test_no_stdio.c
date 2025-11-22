// test_no_stdio.c - 完全不使用stdio，使用系统调用write
#include <unistd.h>
#include <string.h>

#define WRITE_MSG(msg) write(2, msg, strlen(msg))

int main() {
    WRITE_MSG(">>> MAIN STARTED (no stdio) <<<\n");
    
    sleep(1);
    
    WRITE_MSG(">>> MAIN ENDING <<<\n");
    
    return 0;
}
