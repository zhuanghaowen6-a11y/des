# ==========================
# Makefile for Router Simulation
# ==========================

# 编译器与选项
CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -lrt -lpthread -ljansson

# 可执行文件名
TARGETS = r1 r2

# 源文件
SRCS = r1.c r2.c

# 默认目标：编译所有
all: desd libdeshook.so r1 r2

# 测试目标：包含超时测试程序
test: all r1_timeout_test r2_timeout_test

# 分别编译
desd: desd.c common.c common.h
	$(CC) $(CFLAGS) desd.c common.c  -o desd $(LDFLAGS)

libdeshook.so: libdeshook.c common.c common.h
	$(CC) $(CFLAGS) -shared -fPIC libdeshook.c common.c  -o libdeshook.so $(LDFLAGS) -ldl

r1: r1.c common.h
	$(CC) $(CFLAGS) r1.c  -o r1

r2: r2.c common.h
	$(CC) $(CFLAGS) r2.c  -o r2

# 超时测试程序
r1_timeout_test: r1_timeout_test.c
	$(CC) $(CFLAGS) r1_timeout_test.c -o r1_timeout_test

r2_timeout_test: r2_timeout_test.c
	$(CC) $(CFLAGS) r2_timeout_test.c -o r2_timeout_test

# 清理
clean:
	rm -f desd libdeshook.so r1 r2 *.o /tmp/desd_control_socket /tmp/router_socket

# 伪目标（不生成文件）
.PHONY: all clean
