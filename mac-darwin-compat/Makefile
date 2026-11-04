# Makefile — Mac/Darwin compatibility subsystem for Helix OS.

CC ?= gcc
CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Iinclude -I../userspace -I../kernel/src
LDFLAGS := 

SRCS := src/mach_o.c \
        src/darwin_syscall.c \
        src/mach_port.c \
        src/mac_fs.c \
        src/plist.c \
        src/bundle.c \
        src/darwin_env.c \
        src/mac_appkit_x11.c \
        src/mac_subsystem.c \
        src/cli/sw_vers.c \
        src/cli/defaults.c \
        src/cli/plutil.c \
        src/cli/uname_mac.c \
        src/cli/otool_mac.c \
        src/cli/open_mac.c \
        apps/calculator.c \
        apps/textedit.c

OBJS := $(SRCS:.c=.o)

MAIN_SRC := src/mac_subsystem_main.c
MAIN_OBJ := src/mac_subsystem_main.o

TEST_SRC := tests/mac_test.c
TEST_OBJ := tests/mac_test.o

TARGET := darwin-exec
TEST_BIN := mac_test

.PHONY: all clean test

all: $(TARGET) $(TEST_BIN)

$(TARGET): $(MAIN_OBJ) $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

$(TEST_BIN): $(TEST_OBJ) $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)
	@echo "[mac-darwin-compat] PASS: all checks"

clean:
	rm -f $(OBJS) $(MAIN_OBJ) $(TEST_OBJ) $(TARGET) $(TEST_BIN)
