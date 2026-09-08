CC = gcc
CFLAGS = -g -Wall
LDFLAGS = -lpthread -lm
OS = $(shell uname -s)
ARCH = $(shell uname -m)
PLATFORM = $(OS)-$(ARCH)
BUILD_DIR = .build/$(PLATFORM)
PROXY_BIN = $(BUILD_DIR)/proxy.bin
CSAPP_OBJ = $(BUILD_DIR)/csapp.o
PROXY_OBJ = $(BUILD_DIR)/proxy.o
TINY_DIR = tiny

.PHONY: all tiny-build build-all proxy

all: proxy

$(BUILD_DIR):
	mkdir -p $@

$(CSAPP_OBJ): csapp.c csapp.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c csapp.c -o $@

$(PROXY_OBJ): proxy.c csapp.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c proxy.c -o $@

$(PROXY_BIN): $(PROXY_OBJ) $(CSAPP_OBJ)
	$(CC) $(CFLAGS) $(PROXY_OBJ) $(CSAPP_OBJ) -o $@ $(LDFLAGS)

proxy: $(PROXY_BIN)
	printf '%s\n' \
		'#!/bin/sh' \
		'set -eu' \
		'DIR=$$(CDPATH= cd -- "$$(dirname "$$0")" && pwd)' \
		'BIN="$$DIR/$(PROXY_BIN)"' \
		'make -s -C "$$DIR" "$(PROXY_BIN)" >/dev/null' \
		'exec "$$BIN" "$$@"' > $@
	chmod +x $@

tiny-build:
	$(MAKE) -C $(TINY_DIR)

build-all: proxy tiny-build

.PHONY: clean
clean:
	rm -f proxy $(PROXY_BIN) $(CSAPP_OBJ) $(PROXY_OBJ)
