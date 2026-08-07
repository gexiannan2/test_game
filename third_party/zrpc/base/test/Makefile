# zrpc 日志单元测试 Makefile
# 在 WSL/Linux 下于本目录执行: make && ./zrpc_log_test

CXX       ?= g++
ROOT      := ../..
ZRPC_BASE := ..
BUILD_DIR := build

CPPFLAGS  := -I$(ROOT) -std=c++17
CXXFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS   := -pthread

LOG_SRCS := \
	$(ZRPC_BASE)/log.cc \
	$(ZRPC_BASE)/logger.cc \
	$(ZRPC_BASE)/async_log.cc \
	$(ZRPC_BASE)/time_stamp.cc

TEST_SRC := log_test.cc

.PHONY: all clean test

all: $(BUILD_DIR)/zrpc_log_test

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/zrpc_log_test: $(TEST_SRC) $(LOG_SRCS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_SRC) $(LOG_SRCS) -o $@ $(LDFLAGS)

test: $(BUILD_DIR)/zrpc_log_test
	./$(BUILD_DIR)/zrpc_log_test

clean:
	rm -rf $(BUILD_DIR)
