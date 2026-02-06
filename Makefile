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

# 源文件路径
DESD_SRC = $(SRC_DIR)/desd.c
LIBDESHOOK_SRC = $(SRC_DIR)/libdeshook.c
COMMON_SRC = $(SRC_DIR)/common.c
COMMON_H = $(SRC_DIR)/common.h

# 输出路径
DESD = $(BUILD_DIR)/desd
LIBDESHOOK = $(BUILD_DIR)/libdeshook.so

# ==========================
# 主要目标
# ==========================

# 默认：编译核心程序
all: $(DESD) $(LIBDESHOOK)

# 完整编译：当前等同于 all
full: all

# ==========================
# 核心程序
# ==========================

$(DESD): $(DESD_SRC) $(COMMON_SRC) $(COMMON_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -mcmodel=medium $(DESD_SRC) $(COMMON_SRC) -o $(DESD) $(LDFLAGS)
	@echo "✓ Built desd"

$(LIBDESHOOK): $(LIBDESHOOK_SRC) $(COMMON_SRC) $(COMMON_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -fPIC $(LIBDESHOOK_SRC) $(COMMON_SRC) -o $(LIBDESHOOK) $(LDFLAGS) -ldl -lpthread
	@echo "✓ Built libdeshook.so"

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
	@echo "  all           - Build core programs (desd, libdeshook.so)"
	@echo "  full          - Same as 'all'"
	@echo "  clean         - Remove build artifacts and logs"
	@echo "  clean-all     - Remove build/ and logs/ directories"
	@echo "  help          - Show this help message"

# ==========================
# 伪目标
# ==========================

.PHONY: all full clean clean-all help
