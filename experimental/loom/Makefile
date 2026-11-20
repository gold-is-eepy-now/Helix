# LOOM host build. It is separate from the Helix kernel build and never touches it.
# Uses HOST_CC (like the Helix Makefile) so a cross compiler in CC does not reach this build.

HOST_CC ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Werror -Wshadow -g -O1
CPPFLAGS = -Iinclude
BUILD    = build

LIB_SRC  = src/field.c src/contract.c src/morph.c src/continuity.c src/weaver.c

.PHONY: all test demo clean

all: $(BUILD)/loom-demo $(BUILD)/loom-test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/loom-demo: $(LIB_SRC) src/sim_video.c include/loom.h | $(BUILD)
	$(HOST_CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(LIB_SRC) src/sim_video.c

$(BUILD)/loom-test: $(LIB_SRC) tests/test_loom.c include/loom.h | $(BUILD)
	$(HOST_CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(LIB_SRC) tests/test_loom.c

test: $(BUILD)/loom-test
	./$(BUILD)/loom-test

demo: $(BUILD)/loom-demo
	./$(BUILD)/loom-demo

clean:
	rm -rf $(BUILD)
