# 프록시 랩용 Makefile
#
# `handin` 규칙을 제외하면 이 파일은 자유롭게 수정해도 됩니다.
# 교수자나 조교는 여러분의 Makefile에서 `make`를 실행해 프록시를
# 소스에서 빌드합니다.

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
RUN_DIR = .run
TINY_PORT ?= 8000
PROXY_PORT ?= 8080
TINY_PID_FILE = $(RUN_DIR)/tiny.pid
PROXY_PID_FILE = $(RUN_DIR)/proxy.pid
TINY_LOG = $(RUN_DIR)/tiny.log
PROXY_LOG = $(RUN_DIR)/proxy.log

.PHONY: all clean clean-all handin help tiny-build build-all proxy \
	run-tiny run-proxy up down status logs grade free-port

all: proxy

$(BUILD_DIR):
	mkdir -p $@

$(RUN_DIR):
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

help:
	@printf '%s\n' \
		'사용 가능한 명령:' \
		'  make proxy                    - 프록시 실행 파일 빌드' \
		'  make tiny-build               - tiny와 CGI 빌드' \
		'  make build-all                - proxy + tiny 모두 빌드' \
		'  make run-proxy PROXY_PORT=8080 - 프록시를 포그라운드 실행' \
		'  make run-tiny TINY_PORT=8000   - tiny를 포그라운드 실행' \
		'  make up TINY_PORT=8000 PROXY_PORT=8080 - tiny와 proxy를 백그라운드 실행' \
		'  make down                     - up으로 띄운 tiny/proxy 종료' \
		'  make status                   - 현재 백그라운드 PID 확인' \
		'  make logs                     - tiny/proxy 로그 확인' \
		'  make grade                    - driver.sh 자동 채점 실행' \
		'  make free-port                - 사용 가능한 포트 하나 출력' \
		'  make clean-all                - proxy/tiny 빌드 결과와 실행 로그 정리'

tiny-build:
	$(MAKE) -C $(TINY_DIR)

build-all: proxy tiny-build

run-tiny: tiny-build
	cd $(TINY_DIR) && ./tiny $(TINY_PORT)

run-proxy: proxy
	./proxy $(PROXY_PORT)

up: build-all | $(RUN_DIR)
	@$(MAKE) --no-print-directory down
	@cd $(TINY_DIR) && nohup ./tiny $(TINY_PORT) > ../$(TINY_LOG) 2>&1 & echo $$! > ../$(TINY_PID_FILE)
	@nohup ./proxy $(PROXY_PORT) > $(PROXY_LOG) 2>&1 & echo $$! > $(PROXY_PID_FILE)
	@printf 'tiny  : http://127.0.0.1:%s (pid=%s)\n' "$(TINY_PORT)" "$$(cat $(TINY_PID_FILE))"
	@printf 'proxy : http://127.0.0.1:%s (pid=%s)\n' "$(PROXY_PORT)" "$$(cat $(PROXY_PID_FILE))"
	@printf '%s\n' '중지: make down' '로그: make logs'

down:
	@if [ -f "$(PROXY_PID_FILE)" ]; then \
		kill "$$(cat "$(PROXY_PID_FILE)")" 2>/dev/null || true; \
		rm -f "$(PROXY_PID_FILE)"; \
	fi
	@if [ -f "$(TINY_PID_FILE)" ]; then \
		kill "$$(cat "$(TINY_PID_FILE)")" 2>/dev/null || true; \
		rm -f "$(TINY_PID_FILE)"; \
	fi

status:
	@printf 'tiny  pid: %s\n' "$$(if [ -f "$(TINY_PID_FILE)" ]; then cat "$(TINY_PID_FILE)"; else echo 없음; fi)"
	@printf 'proxy pid: %s\n' "$$(if [ -f "$(PROXY_PID_FILE)" ]; then cat "$(PROXY_PID_FILE)"; else echo 없음; fi)"

logs:
	@printf '%s\n' '--- tiny.log ---'
	@if [ -f "$(TINY_LOG)" ]; then tail -n 40 "$(TINY_LOG)"; else echo '(tiny 로그 없음)'; fi
	@printf '%s\n' '--- proxy.log ---'
	@if [ -f "$(PROXY_LOG)" ]; then tail -n 40 "$(PROXY_LOG)"; else echo '(proxy 로그 없음)'; fi

grade: build-all
	./driver.sh

free-port:
	./free-port.sh

# 제출할 수 있는 tar 파일을 ../proxylab-handin.tar에 생성합니다.
# 이 규칙은 수정하지 마세요.
handin:
	(make clean; cd ..; tar cvf $(USER)-proxylab-handin.tar proxylab-handout --exclude tiny --exclude nop-server.py --exclude proxy --exclude driver.sh --exclude port-for-user.pl --exclude free-port.sh --exclude ".*")

clean:
	rm -rf *~ *.o proxy .build .run core *.tar *.zip *.gzip *.bzip *.gz

clean-all: clean
	$(MAKE) -C $(TINY_DIR) clean
