# ==========================
# Makefile for DES Router Simulation
# Updated for new directory structure
# ==========================

# 编译器与选项
CC = gcc
CFLAGS = -Wall -g -Isrc
LDFLAGS = -lrt -lpthread -ljansson

# 目录
SRC_DIR = src
BUILD_DIR = build
TEST_DIR = tests

# 源文件路径
DESD_SRC = $(SRC_DIR)/desd.c
LIBDESHOOK_SRC = $(SRC_DIR)/libdeshook.c
COMMON_SRC = $(SRC_DIR)/common.c
COMMON_H = $(SRC_DIR)/common.h

# 输出路径
DESD = $(BUILD_DIR)/desd
LIBDESHOOK = $(BUILD_DIR)/libdeshook.so

# 基础测试程序
BASIC_R1 = $(BUILD_DIR)/r1
BASIC_R2 = $(BUILD_DIR)/r2

# BIRD测试程序
BIRD_TEST_R1 = $(BUILD_DIR)/test_rw_basic_r1
BIRD_TEST_R2 = $(BUILD_DIR)/test_rw_basic_r2

# ==========================
# 主要目标
# ==========================

# 默认：编译核心程序
all: $(DESD) $(LIBDESHOOK) $(BASIC_R1) $(BASIC_R2)

# 完整编译：包括所有测试程序
full: all bird-tests poll-tests tcp-tests timeout-tests multi-tests

# ==========================
# 核心程序
# ==========================

$(DESD): $(DESD_SRC) $(COMMON_SRC) $(COMMON_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DESD_SRC) $(COMMON_SRC) -o $(DESD) $(LDFLAGS)
	@echo "✓ Built desd"

$(LIBDESHOOK): $(LIBDESHOOK_SRC) $(COMMON_SRC) $(COMMON_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -fPIC $(LIBDESHOOK_SRC) $(COMMON_SRC) -o $(LIBDESHOOK) $(LDFLAGS) -ldl
	@echo "✓ Built libdeshook.so"

# ==========================
# 基础测试程序
# ==========================

$(BASIC_R1): $(TEST_DIR)/basic/r1.c $(COMMON_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/basic/r1.c -o $(BASIC_R1)
	@echo "✓ Built r1"

$(BASIC_R2): $(TEST_DIR)/basic/r2.c $(COMMON_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/basic/r2.c -o $(BASIC_R2)
	@echo "✓ Built r2"

# ==========================
# BIRD测试程序
# ==========================

bird-tests: $(BIRD_TEST_R1) $(BIRD_TEST_R2)

$(BIRD_TEST_R1): $(TEST_DIR)/bird/test_rw_basic_r1.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/bird/test_rw_basic_r1.c -o $(BIRD_TEST_R1)
	@echo "✓ Built test_rw_basic_r1"

$(BIRD_TEST_R2): $(TEST_DIR)/bird/test_rw_basic_r2.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/bird/test_rw_basic_r2.c -o $(BIRD_TEST_R2)
	@echo "✓ Built test_rw_basic_r2"

# ==========================
# Poll测试程序
# ==========================

poll-tests: $(BUILD_DIR)/r_poll_server_enhanced $(BUILD_DIR)/r_poll_client_enhanced \
            $(BUILD_DIR)/r_poll_server_test_v2 $(BUILD_DIR)/r_poll_client_test_v2

$(BUILD_DIR)/r_poll_server_enhanced: $(TEST_DIR)/poll/r_poll_server_enhanced.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/poll/r_poll_server_enhanced.c -o $@
	@echo "✓ Built r_poll_server_enhanced"

$(BUILD_DIR)/r_poll_client_enhanced: $(TEST_DIR)/poll/r_poll_client_enhanced.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/poll/r_poll_client_enhanced.c -o $@
	@echo "✓ Built r_poll_client_enhanced"

$(BUILD_DIR)/r_poll_server_test_v2: $(TEST_DIR)/poll/r_poll_server_test_v2.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/poll/r_poll_server_test_v2.c -o $@
	@echo "✓ Built r_poll_server_test_v2"

$(BUILD_DIR)/r_poll_client_test_v2: $(TEST_DIR)/poll/r_poll_client_test_v2.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/poll/r_poll_client_test_v2.c -o $@
	@echo "✓ Built r_poll_client_test_v2"

# ==========================
# TCP测试程序
# ==========================

tcp-tests: $(BUILD_DIR)/r1_test_tcp $(BUILD_DIR)/r2_test_tcp

$(BUILD_DIR)/r1_test_tcp: $(TEST_DIR)/tcp/r1_test_tcp.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/tcp/r1_test_tcp.c -o $@
	@echo "✓ Built r1_test_tcp"

$(BUILD_DIR)/r2_test_tcp: $(TEST_DIR)/tcp/r2_test_tcp.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/tcp/r2_test_tcp.c -o $@
	@echo "✓ Built r2_test_tcp"

# ==========================
# 超时测试程序
# ==========================

timeout-tests: $(BUILD_DIR)/r1_timeout_test $(BUILD_DIR)/r2_timeout_test

$(BUILD_DIR)/r1_timeout_test: $(TEST_DIR)/timeout/r1_timeout_test.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/timeout/r1_timeout_test.c -o $@
	@echo "✓ Built r1_timeout_test"

$(BUILD_DIR)/r2_timeout_test: $(TEST_DIR)/timeout/r2_timeout_test.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/timeout/r2_timeout_test.c -o $@
	@echo "✓ Built r2_timeout_test"

# ==========================
# 多连接测试程序
# ==========================

multi-tests: $(BUILD_DIR)/r_multi_connect_test $(BUILD_DIR)/r_multi_listen_test

$(BUILD_DIR)/r_multi_connect_test: $(TEST_DIR)/multi/r_multi_connect_test.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/multi/r_multi_connect_test.c -o $@
	@echo "✓ Built r_multi_connect_test"

$(BUILD_DIR)/r_multi_listen_test: $(TEST_DIR)/multi/r_multi_listen_test.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_DIR)/multi/r_multi_listen_test.c -o $@
	@echo "✓ Built r_multi_listen_test"

# ==========================
# 清理
# ==========================

clean:
	rm -rf $(BUILD_DIR)/*
	rm -rf logs/*
	-rm -f /tmp/desd_control_socket /tmp/router_socket 2>/dev/null || true
	@echo "✓ Cleaned build and logs"

clean-all: clean
	rm -rf $(BUILD_DIR) logs
	@echo "✓ Cleaned all generated files"

# ==========================
# 帮助
# ==========================

help:
	@echo "DES Project Makefile"
	@echo "===================="
	@echo ""
	@echo "Targets:"
	@echo "  all           - Build core programs (desd, libdeshook.so, r1, r2)"
	@echo "  full          - Build all programs including tests"
	@echo "  bird-tests    - Build BIRD support tests"
	@echo "  poll-tests    - Build poll enhancement tests"
	@echo "  tcp-tests     - Build TCP tests"
	@echo "  timeout-tests - Build timeout tests"
	@echo "  multi-tests   - Build multi-connection tests"
	@echo "  clean         - Remove build artifacts and logs"
	@echo "  clean-all     - Remove build/ and logs/ directories"
	@echo "  help          - Show this help message"
	@echo ""
	@echo "Directory Structure:"
	@echo "  src/     - Source code"
	@echo "  tests/   - Test programs"
	@echo "  build/   - Compiled executables"
	@echo "  scripts/ - Shell scripts"
	@echo "  docs/    - Documentation"
	@echo "  logs/    - Log files"

# ==========================
# 伪目标
# ==========================

.PHONY: all full clean clean-all help \
        bird-tests poll-tests tcp-tests timeout-tests multi-tests
