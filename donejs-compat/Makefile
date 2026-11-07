CC ?= gcc
AR ?= ar
CFLAGS ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -O2
HOST_CFLAGS ?= $(CFLAGS) -Iinclude -Isrc
TARGET_CFLAGS ?= -ffreestanding -fno-builtin -nostdlib -mno-red-zone -mcmodel=large -DDJS_FREESTANDING -Iinclude -Isrc

SRCS = src/djs_core.c \
       src/djs_lexer.c \
       src/djs_parser.c \
       src/djs_literals.c \
       src/djs_operators.c \
       src/djs_expr.c \
       src/djs_object.c \
       src/djs_builtins.c \
       src/djs_interp.c \
       src/djs_modules.c \
       src/djs_runtime.c

ASRCS = src/djs_jmp.S

HOST_OBJS = $(SRCS:.c=.host.o) $(ASRCS:.S=.host.o)
TARGET_OBJS = $(SRCS:.c=.o) $(ASRCS:.S=.o)

.PHONY: all test clean lib

all: test

lib: libdonejs.a

libdonejs.a: $(TARGET_OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(TARGET_CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(TARGET_CFLAGS) -c $< -o $@

%.host.o: %.c
	$(CC) $(HOST_CFLAGS) -c $< -o $@

%.host.o: %.S
	$(CC) $(HOST_CFLAGS) -c $< -o $@

selftest: $(HOST_OBJS) tests/selftest.c
	$(CC) $(HOST_CFLAGS) $^ -o $@ -lm

test: selftest
	./selftest

clean:
	rm -f src/*.o src/*.host.o selftest libdonejs.a
