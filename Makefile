#
# NetBird (client) ARMv7 Cross-Compilation Makefile
# Target Platform: RV1106 (arm-rockchip830-linux-uclibcgnueabihf)
# Tag: v0.79.0
#
# netbird 客户端是纯 Go：CGO_ENABLED=0 编出来是完全静态的，不依赖目标机 libc
# 版本，所以在 uClibc 根文件系统上直接能跑（client/ 里唯一的 import "C" 在
# client/ui 桌面包，不进这个二进制）。
#
# patch/ 下三个补丁是 IP-KVM 裁剪，用 TRIM=0 可以关掉编上游原样：
#   0001 命令面裁剪     删掉 ssh/kubernetes/expose/capture/debug/... 等 19 个
#                       client/cmd 文件，只留 up/down/status/login/logout/
#                       service/version
#   0002 合并 gopacket  上游自己用废弃的 google/gopacket，而 rosenpass 依赖
#                       用 gopacket/gopacket，两个 fork 同时进二进制；统一到
#                       新 fork 省 ~1 MB
#   0003 深度裁剪       防火墙换 no-op、relay 去 QUIC、去 eBPF WG 代理、
#                       Rosenpass 换 no-op
#

SHELL:=/bin/bash
export LC_ALL=C

# Build output configuration
PKG_NAME := netbird
PKG_BIN ?= out
OUTPUT_DIR := $(PKG_BIN)
CURRENT_DIR := $(shell pwd)

# netbird version（注入到二进制里，见 version 包）
NETBIRD_TAG := v0.81.3
NETBIRD_VERSION := 0.81.3
NETBIRD_SRC := $(CURDIR)/netbird

# Number of parallel jobs
JOBS ?= $(shell nproc)

# C 控制台用的交叉编译器（netbird 本体是纯 Go，只有 CLI 需要 C 工具链）
CROSS_COMPILE ?= arm-rockchip830-linux-uclibcgnueabihf-
CC_CLI := $(CROSS_COMPILE)gcc

# 禁用内联以缩小体积（约 -12%）。
# 必须带 all= 前缀，否则只作用于 ./client 本身、不含依赖包，等于无效
GCFLAGS ?= all=-l

# IP-KVM 裁剪开关：TRIM=1 应用 patch/ 下全部补丁，TRIM=0 编上游原样
TRIM ?= 1

# 用 upx 压缩产物（与 tailscale/zerotier/frp 一致）；UPX=0 可关闭
UPX ?= 1

ifeq ($(TRIM),1)
  PATCHES := $(sort $(wildcard $(CURDIR)/patch/*.patch))
else
  PATCHES :=
endif
PATCH_COUNT := $(words $(PATCHES))

# ---- C 控制台（替代官方 netbird CLI）----
#
# netbird 的 CLI 与 daemon 是**同一个二进制里的子命令**（client/main.go 只有
# 15 行，直接 cmd.Execute()），所以删不掉：只要 netbird 在，CLI 就在。
#
# 这里额外提供一个功能等价的 C 版控制台，体积 33 KB，走 netbird 自己的
# grpc-gateway JSON socket（--enable-json-socket），不 popen 任何东西。
# 附带好处：官方 CLI 一次调用要做进程启动 + gRPC，C 版是纯 HTTP 往返。
#
# 注意：**必须**让 daemon 带 --enable-json-socket 启动，否则只有 gRPC
# socket，C 版连不上（见 flexkvm-netbird/README.md 的运行时一节）。
CLI_ENABLE ?= 1
LIB_DIR    := $(CURDIR)/../lib
LIB_A      := $(LIB_DIR)/out/libvpnctl.a
CLI_BIN    := $(OUTPUT_DIR)/netbird-cli
CLI_SRC    := $(CURDIR)/cli/netbird-cli.c
CLI_CFLAGS := -std=c17 -O2 -Wall -Wextra
CLI_LDLIBS := -ljansson
NB_SYSROOT ?= $(shell ls -d /root/rv1106/flexkvm_sdk/sysdrv/source/buildroot/buildroot-*/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot 2>/dev/null | head -1)

.PHONY: all clean clone build cli lib install info help

all: info clone build cli install
	@echo "Build $(PKG_NAME) done"

info:
	@echo "=========================================="
	@echo "NetBird (client) ARMv7 Cross-Compilation"
	@echo "=========================================="
	@echo "Version: $(NETBIRD_TAG) (pkg $(NETBIRD_VERSION))"
	@echo "Toolchain: go (CGO_ENABLED=0, GOTOOLCHAIN=local)"
	@echo "Source: $(NETBIRD_SRC)"
	@echo "Output Dir: $(OUTPUT_DIR)"
	@echo "Trim: $(TRIM) (patches: $(PATCH_COUNT))"
	@echo "UPX: $(UPX)"
	@echo "=========================================="

clone:
	@echo "Initializing/Updating netbird submodule $(NETBIRD_TAG)..."
	@git submodule update --init --force netbird
	@cd $(NETBIRD_SRC) && git checkout -q $(NETBIRD_TAG) 2>/dev/null || \
		{ echo "错误：无法切到 $(NETBIRD_TAG)，请先 git submodule update --init --force netbird"; exit 1; }

build: clone
	@echo "Building netbird..."
	@mkdir -p $(PKG_BIN)

	@# 每次构建都把 submodule 还原到干净状态再打补丁。
	@# 注意不能只用 `git checkout -- .`：它只还原**已跟踪**文件，而补丁新增的
	@# 未跟踪文件（client/cmd/ssh_flags.go 等）会留下来，和还原出来的原文件
	@# 撞成 "serverSSHAllowedFlag redeclared"。必须配 git clean -fd。
	@echo "Resetting source tree..."
	@git -C $(NETBIRD_SRC) checkout -q -- . 2>/dev/null || true
	@git -C $(NETBIRD_SRC) clean -fdq

	@for p in $(PATCHES); do \
		echo "Applying: $$(basename $$p)"; \
		git -C $(NETBIRD_SRC) apply --ignore-whitespace "$$p" || exit 1; \
	done
	@if [ -z "$(PATCHES)" ]; then echo "(TRIM=0：不打补丁，编上游原样)"; fi

	@( \
		export CGO_ENABLED=0 && \
		export GOOS=linux && \
		export GOARCH=arm && \
		export GOARM=7 && \
		export GOTOOLCHAIN=local && \
		cd $(NETBIRD_SRC) && \
		LDFLAGS="-s -w -buildid= -X github.com/netbirdio/netbird/version.version=$(NETBIRD_VERSION)" && \
		go build -ldflags="$$LDFLAGS" -trimpath -buildvcs=false \
			-gcflags="$(GCFLAGS)" -p $(JOBS) \
			-o $(CURDIR)/$(PKG_BIN)/netbird ./client \
	)

lib:
	@$(MAKE) -C $(LIB_DIR)

cli: lib
	@echo "Building C console (netbird-cli)..."
	@mkdir -p $(OUTPUT_DIR)
	@$(CC_CLI) $(CLI_CFLAGS) \
		$(if $(NB_SYSROOT),--sysroot=$(NB_SYSROOT) -I$(NB_SYSROOT)/usr/include) \
		-I$(LIB_DIR)/include \
		$(CLI_SRC) $(LIB_A) \
		$(if $(NB_SYSROOT),-L$(NB_SYSROOT)/usr/lib) \
		$(CLI_LDLIBS) -o $(CLI_BIN)
	@echo "  -> $(CLI_BIN)"

install: build cli
	@echo "Binary ready in $(OUTPUT_DIR)/"
ifeq ($(UPX),1)
	@echo ""
	@echo "Compressing with upx..."
	@which upx >/dev/null 2>&1 && upx --best --lzma $(OUTPUT_DIR)/netbird || echo "  upx not found, skipping compression"
endif
	@echo ""
	@echo "Installed files:"
	@test -f $(CLI_BIN) && { which $(CROSS_COMPILE)strip >/dev/null 2>&1 && $(CROSS_COMPILE)strip $(CLI_BIN) || true; } || true
	@ls -lh $(OUTPUT_DIR)/

clean:
	@if [ -d "$(NETBIRD_SRC)" ]; then \
		cd $(NETBIRD_SRC) && git checkout -q . 2>/dev/null && git clean -fdq; \
	fi
	rm -rf $(PKG_BIN)
	@echo "Clean done"

help:
	@echo "NetBird Build Targets:"
	@echo ""
	@echo "  make          - Build netbird client (trimmed) and compress with upx"
	@echo "  make clone    - Checkout pinned netbird submodule tag"
	@echo "  make build    - Build only"
	@echo "  make cli      - Build the C console (netbird-cli)"
	@echo "  make install  - Build (+ C console) and compress with upx"
	@echo "  make clean    - Clean build artifacts and revert patches"
	@echo "  make help     - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  TRIM=0        - build upstream as-is (no IP-KVM patches)"
	@echo "  UPX=0         - skip upx compression"
	@echo "  GCFLAGS=...   - go compiler flags (default: all=-l)"
