# Makefile for UD -- optimized, size-stripped build.
# Usage:
#   make            build ./ud   (ud.exe on Windows)
#   make examples   build, then run every example
#   make clean      remove build artifacts

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -ffunction-sections -fdata-sections \
           -fno-asynchronous-unwind-tables -fno-unwind-tables
LDFLAGS ?= -s -Wl,--gc-sections

SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
BIN := ud

ifeq ($(OS),Windows_NT)
	BIN := ud.exe
endif

# input.ud waits on stdin, so leave it out of the batch run
RUNNABLE := $(filter-out examples/input.ud,$(wildcard examples/*.ud))

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC) $(LDFLAGS)
	@echo "Built $@"

.PHONY: examples clean
examples: $(BIN)
	@for f in $(RUNNABLE); do echo "===== $$f ====="; ./$(BIN) $$f; done

clean:
	rm -f $(BIN) *.o *.ldx
