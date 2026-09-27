CC = gcc
CFLAGS = -Wall -Wextra -g -Imcp_lib
LDLIBS = -ljson-c -lcurl -lpthread

MCP_CORE_DIR = mcp_lib
MCP_CORE_LIB = $(MCP_CORE_DIR)/libmcp_core.a

# Unity test framework
UNITY_DIR = vendor/unity
UNITY_SRC = $(UNITY_DIR)/src/unity.c
UNITY_INC = -I$(UNITY_DIR)/src

# Main server
SRCS = mcp_json_rpc.c \
       mcp_utils.c \
       mcp_tools_file.c \
       mcp_tools_dir.c \
       mcp_tools_misc.c \
       mcp_tools_cmd.c \
       mcp_server.c \
       mcp_main.c

OBJS = $(SRCS:.c=.o)
TARGET = mcpsv_file

# Test configuration - libraries needed for tests (object files to link)
TEST_LIB_OBJS = mcp_utils.o mcp_json_rpc.o mcp_tools_file.o mcp_tools_dir.o \
                mcp_tools_misc.o mcp_tools_cmd.o mcp_server.o

# Test source files
TEST_DIR = tests
TEST_SRCS = $(wildcard $(TEST_DIR)/test_*.c)
TEST_OBJS = $(TEST_SRCS:.c=.o)
TEST_TARGET = run_tests

all: $(TARGET)

$(TARGET): $(OBJS) mcp-core
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(MCP_CORE_LIB) $(LDLIBS)

mcp-core:
	$(MAKE) -C $(MCP_CORE_DIR)

%.o: %.c mcp_common.h
	$(CC) $(CFLAGS) -c $< -o $@

# Test targets
test: $(TEST_TARGET)
	./$(TEST_TARGET)

test-verbose: $(TEST_TARGET)
	./$(TEST_TARGET) -v

$(TEST_TARGET): $(UNITY_SRC) $(TEST_OBJS) $(TEST_LIB_OBJS) mcp-core
	$(CC) $(CFLAGS) $(UNITY_INC) -o $@ $(UNITY_SRC) $(TEST_OBJS) $(TEST_LIB_OBJS) $(MCP_CORE_LIB) $(LDLIBS)

$(TEST_DIR)/%.o: $(TEST_DIR)/%.c mcp_common.h
$(CC) $(CFLAGS) $(UNITY_INC) -I./ -c $< -o $@

# Generate test runner automatically (optional, using Ruby script from Unity)
# ruby $(UNITY_DIR)/auto/generate_test_runner.rb src/mcp_utils.h tests/test_mcp_utils.c tests/test_mcp_utilsRunner.c

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f $(TEST_OBJS) $(TEST_TARGET)
	rm -f *.gcno *.gcda *.gcov coverage.info

cov-clean: clean
	rm -rf coverage_html

.PHONY: all mcp-core test test-verbose clean cov-clean
