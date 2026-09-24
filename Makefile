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
NETBIRD_TAG := v0.79.0
NETBIRD_VERSION := 0.79.0
NETBIRD_SRC := $(CURDIR)/netbird

# Number of parallel jobs
JOBS ?= $(shell nproc)

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

.PHONY: all clean clone build install info help

all: info clone build install
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

install: build
	@echo "Binary ready in $(OUTPUT_DIR)/"
ifeq ($(UPX),1)
	@echo ""
	@echo "Compressing with upx..."
	@which upx >/dev/null 2>&1 && upx --best --lzma $(OUTPUT_DIR)/netbird || echo "  upx not found, skipping compression"
endif
	@echo ""
	@echo "Installed files:"
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
	@echo "  make install  - Build and compress with upx"
	@echo "  make clean    - Clean build artifacts and revert patches"
	@echo "  make help     - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  TRIM=0        - build upstream as-is (no IP-KVM patches)"
	@echo "  UPX=0         - skip upx compression"
	@echo "  GCFLAGS=...   - go compiler flags (default: all=-l)"
