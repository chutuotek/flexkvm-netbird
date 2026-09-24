# flexkvm-netbird

NetBird 客户端 ARMv7 交叉编译（RV1106 / uClibc 根文件系统）。

## 版本

- NetBird: **v0.79.0**（上游 tag，纯 Go）
- 目标: `GOOS=linux GOARCH=arm GOARM=7`，**CGO_ENABLED=0 → 完全静态**，
  不依赖目标机 libc 版本，uClibc 直接能跑
- Go: 需要 `go >= 1.26`（go.mod 要求 1.26.0；用 `GOTOOLCHAIN=local` 禁止自动下载工具链）

## 编译

```bash
make              # 默认：应用 IP-KVM 裁剪补丁并做 upx 压缩
make TRIM=0       # 编上游原样（不做裁剪，用于对照/调试）
make UPX=0        # 不压缩
```

## 产物

```
out/
└── netbird    # 客户端（守护进程 + CLI 同一个二进制）
```

`file` 结果：`ELF 32-bit LSB executable, ARM, EABI5, statically linked, stripped`，
**0 个动态依赖**。

## 裁剪说明

`patch/` 下三个补丁构成 IP-KVM 裁剪，全部靠 patch 实现，**不改上游的分支/提交**：

| 补丁 | 内容 |
|---|---|
| `0001-ipkvm-trim-commands` | 命令面裁剪：删 19 个 `client/cmd` 文件（ssh / kubernetes / expose / capture / trace / debug / state / profile / networks / forwarding / qr / service_installer 等），只留 `up down status login logout deregister version service` |
| `0002-unify-gopacket-fork` | 上游自己用已废弃的 `google/gopacket`，而 `cunicu.li/go-rosenpass` 用 `gopacket/gopacket`，**两个 fork 同时进二进制**；统一到新 fork |
| `0003-ipkvm-deep-cuts` | 防火墙换 no-op（省掉 uspfilter + iptables + nftables 三套）、relay 只留 WebSocket（去掉 quic-go）、去掉 eBPF WG 代理（cilium/ebpf）、Rosenpass 换 no-op |

**功能代价（必须在产品层面确认）**：

- 本地**不再安装 ACL/NAT 规则**（防火墙是 no-op）—— 管理端的访问策略不会在本机强制
- relay 只走 **TCP/WebSocket**，没有 QUIC
- 没有 eBPF WG 代理（退化为用户态 UDP 代理）、没有 Rosenpass 抗量子 PSK
- 没有 netbird 自己的 DNS（`--accept-dns` 也不用），解析走系统 `/etc/resolv.conf`

详细测量与验证过程见 [docs-trim-report.md](docs-trim-report.md)。

## 体积与内存

| 形态 | 体积 | 空闲 RSS |
|---|---|---|
| 上游原样 | 40,239,230 B | 46,264 kB（默认 GOGC） |
| + 补丁（裁剪后） | **29,819,006 B** | **33,748 kB**（`GOMEMLIMIT=32MiB GOGC=30`） |
| 裁剪 + upx | **6,304,796 B** | — |
| 对照 tailscaled | 3,828,608 B（upx） | 29,788 kB |
| 对照 ZeroTier | 304,132 B（upx） | 7,316 kB |

> **netbird 是三家里最重的**：体积是 tailscaled 的 1.6 倍，内存比 ZeroTier 高 4 倍多。
> 在这块 212 MB 的板子上同时开 tailscale + zerotier + netbird 会被 OOM killer 打死
> （实测过，见 docs-trim-report.md）。

## 运行时

```bash
./netbird version
# 1) 起守护进程（注意 --disable-dns / --disable-firewall 不属于这个命令）
./netbird service run \
    --config /userdata/netbird/default.json \
    --daemon-addr unix:///userdata/netbird/nb.sock \
    --enable-json-socket --json-socket unix:///userdata/netbird/nb-http.sock \
    --log-file console --log-level info

# 2) 接入网络：--disable-dns / --disable-firewall 是 up 的 flag（会写进 SetConfig）
./netbird up --disable-dns --disable-firewall \
    --management-url https://your-mgmt.example.com \
    --setup-key <SETUP_KEY>
```

- **必须能连到 management + signal 服务器**：可用 NetBird Cloud，或按上游
  `infrastructure_files/docker-compose.yml.tmpl` 自建（国内自建要备案）
- 状态默认在 `/var/lib/netbird/`，用 `NB_STATE_DIR` 指到 `/userdata`
- 不需要 systemd：直接 `service run` 即可
- 本地控制面：默认 gRPC unix socket；**给 C 侧集成要用
  `--enable-json-socket`**（grpc-gateway 提供 HTTP/JSON over unix socket）
- 需要 `/dev/net/tun` + root

## 安全提醒

上游 2026-07-30 发布过
[GHSA-qcpp-8vwj-hhwr](https://netbird.io/knowledge-hub/security-advisory-daemon-ipc-local-privilege-escalation)：
客户端守护进程 IPC **接受任意本地进程的无认证控制命令**，本地非特权用户可读配置、
改安全设置、断开 VPN、把节点挪到别的账号并借此提权。
影响 v0.5.0（2022-03）至 v0.75.1，**v0.76.0 修复**（本仓库的 v0.79.0 已包含修复）。

但要注意：**本地 socket 默认是 `0666`**。集成时如果把它放在多用户可写的目录里，
等于把控制权交给任何能跑进程的东西——建议收紧到 `0600` 并放在 `0700` 目录下。

## 工具链

- 目标平台: RV1106 (arm-rockchip830-linux-uclibcgnueabihf)
- Go: `CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 GOTOOLCHAIN=local`
- 链接参数: `-ldflags="-s -w -buildid= -X github.com/netbirdio/netbird/version.version=0.79.0"
  -trimpath -buildvcs=false -gcflags='all=-l'`
  > `-gcflags` 必须带 `all=` 前缀，否则只作用于 `./client` 本身、不含依赖包，等于无效。
  > `-buildvcs=false` 让二进制版本固定输出 `0.79.0`（否则会带上 `-dirty` 后缀）。
