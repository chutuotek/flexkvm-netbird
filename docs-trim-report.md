# NetBird 客户端 IP-KVM 裁剪

目标：把 NetBird Go agent（上游 main `6e17f50`）裁到"IP-KVM 够用"的最小面，
并量出每一步的实际收益。基线对照是 tailscale（`flexkvm-tailscale`）。

## 结论摘要

| 问题 | 答案 |
|---|---|
| 能砍吗 | 能。三轮砍完：**40,239,230 → 29,819,006 B（−25.9%）** |
| 再压缩 | **upx 后 6,304,796 B（6.01 MiB）**，是 tailscaled（3.83 MiB）的 1.6 倍 |
| 内存 | 空闲 RSS **46,264 → 33,748 kB（−27%）** |
| 还能继续砍吗 | 能，但只剩 ~8%：剩下的都是**核心互联**，动一刀要改引擎逻辑 |
| 真正的瓶颈 | 不是 flash，是 **RAM**：33.7 MB 仍高于 tailscaled 的 29.8 MB |

## 构建配方

```bash
cd 3rd/netbird
git apply reports/netbird/0001-ipkvm-trim-commands.patch
git apply reports/netbird/0002-unify-gopacket-fork.patch
git apply reports/netbird/0003-ipkvm-deep-cuts.patch
```

```bash
export GOTOOLCHAIN=local GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0

go build -trimpath -gcflags='all=-l' -ldflags='-s -w -buildid=' -o out/netbird ./client
upx --best --lzma out/netbird
```

运行时收内存（实测 46.3 MB → 33.7 MB）：

```bash
export GOMEMLIMIT=32MiB GOGC=30
# 起守护进程（--disable-dns / --disable-firewall 是 up 的 flag，不在这里）
netbird service run --config /userdata/netbird/default.json \
  --daemon-addr unix:///userdata/netbird/nb.sock \
  --enable-json-socket --json-socket unix:///userdata/netbird/nb-http.sock \
  --log-file console --log-level info

# 接入网络（这一步才带那两个 flag）
netbird up --disable-dns --disable-firewall -m https://your-mgmt.example.com -k <SETUP_KEY>
```

`--enable-json-socket` 是给 flexkvm-app 用的：netbird 本地控制面默认是 gRPC，
C 侧没法直接讲；这个 grpc-gateway 网关提供 HTTP/JSON over unix socket，
等价于 tailscaled 的 unix socket / zerotier 的 localhost HTTP。

## 测量明细

全部为 `GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0` 实测。

| 形态 | 字节 | 相对上游 |
|---|---|---|
| 上游原样 `-s -w -trimpath` | 40,239,230 | — |
| + `-gcflags='all=-l'` | 35,324,030 | −12.2% |
| + 合并重复 gopacket | 39,125,118 | −2.8% |
| + IP-KVM 命令裁剪（0001） | 38,994,046 | −3.1% |
| **0001 + 0002 + noinline** | **33,423,486** | **−16.9%** |
| **+ 深度裁剪（0003）** | **29,819,006** | **−25.9%** |
| 上游 + upx | 9,019,796 | −77.6% |
| 0001+0002 + upx | 7,080,112 | −82.4% |
| **全叠加 + upx** | **6,304,796** | **−84.3%** |
| 对照 tailscaled（upx） | 3,828,608 | — |

内存（设备实测，`GOMEMLIMIT=32MiB GOGC=30`）：

| 形态 | 空闲 RSS |
|---|---|
| 上游原样，默认 GOGC | 46,264 kB |
| 0001+0002 | 35,828 kB |
| 全叠加 | **33,748 kB** |
| 对照 tailscaled | 29,788 kB |

ELF 段（`readelf -lW` 实测）：

| 形态 | 文件 | FileSiz 合计 | BSS |
|---|---|---|---|
| 上游 | 40,239,230 | 38.33 MiB | 39.92 MiB |
| 0001+0002 | 33,423,486 | 31.74 MiB | 35.33 MiB |
| 全叠加 | 29,819,006 | 28.35 MiB | 35.33 MiB |

> BSS 不影响 flash（RW 段 FileSiz 只有 ~0.55 MiB），其中 32 MiB 是 Go 1.24+ FIPS 的
> `crypto/internal/fips140/drbg.memory`。**符号体积榜 ≠ flash 收益榜**，这一点用
> `readelf` 的 FileSiz/MemSiz 对不上就会判断错方向。

## 三个补丁

### 0001 命令裁剪（33 个文件）

删除 `client/cmd/` 下 19 个文件（ssh、kubernetes、expose、forwarding_rules、
networks、profile、capture、trace、debug、state、service_installer、qr、jobs），
改 `root.go`/`service.go` 的注册，`up.go`/`service_controller.go` 去掉 SIGUSR1
调试包钩子，新增 `ssh_flags.go` 保留 SSH 服务端开关（`up`/`login` 仍要读）。

裁剪后命令面：`up down status login logout deregister version service completion help`。

### 0002 合并重复的 gopacket fork（12 个文件）

netbird 自己的代码用弃坑的 `github.com/google/gopacket v1.1.19`，而
`cunicu.li/go-rosenpass`（被 `client/internal/rosenpass` 引用）用
`github.com/gopacket/gopacket v1.4.0`，**两个 fork 同时进二进制**。
统一到新 fork：flash −1.09 MiB、BSS −4.60 MiB。

> 注意：只 import `.../gopacket/layers` 的文件（如 `uspfilter/conntrack/icmp.go`）
> 不会被 `"github.com/google/gopacket"` 这个精确匹配抓到，必须用前缀匹配。
> 第一版补丁就漏了它，导致 uspfilter 编译不过（`ICMPv4TypeCode` 类型不匹配）。

### 0003 深度裁剪（9 个文件）

| 砍掉 | 做法 | 代价 |
|---|---|---|
| 防火墙 | `create_linux.go` 返回 no-op Manager，新增 `nop.go` | 本地 ACL/NAT 不再安装 |
| iptables/nftables 后端 | 关掉 factory，`state_linux.go` 去掉对应 ShutdownState | 无 |
| relay QUIC 传输 | `dialers_generic.go` 只保留 WebSocket | relay 走 TCP/WS |
| eBPF WG 代理 | `factory_kernel.go` 恒用用户态 UDP 代理 | 等于 `NB_DISABLE_EBPF_WG_PROXY=true` |
| Rosenpass 抗量子 PSK | `rosenpass` 换成 no-op Manager，删 handler | Rosenpass 永远关 |

## 没砍的，以及为什么

深度裁剪后 `go list -deps` 仍在链接里的重包，逐个查过引用链：

| 包 | 引用者 | 为什么难砍 |
|---|---|---|
| `miekg/dns` | `shared/management/networkmap/privatezones.go`、`client/internal/dns`、`certmagic` | **管理协议的网络映射在用**，砍了会坏私网域名匹配 |
| `gopacket` | `sharedsock`、`wgproxy/udp`（raw socket）、`client/internal/dns`、`client/server/trace.go` | 要四个子系统同时消失，外加 `server/trace.go` 的 gRPC 方法要补桩 |
| `gvisor` netstack | `connect.go`、`engine.go`、`routemanager`、`dns/server.go` 等 9 处 | 是一个受支持的运行模式，砍它要改核心路径 |
| `prometheus` | `client/internal/metrics` ← `client/internal/peer`（`metrics_saver.go`） | 织进 peer 连接里了 |
| `client/ssh` | `client/server` 网关 + `profilemanager` + `ssh/config` 状态 | 中等手术量，只值 0.13 MiB |
| `uspfilter` | `client/server/trace.go`（包追踪 RPC） | 同上，且单独砍它不会带走 gopacket |

另外：

- **没有 feature build tag 可用**：整个仓库只有 `pprof` 一个功能标签（其余全是平台标签）。
  想再砍只能改源码，也就是维护一个 fork——不像 zerotier 有
  `-Os --gc-sections` + 宏开关的现成路子。
- **再往下就是核心**：gvisor/pion/wireguard-go/grpc/protobuf 全是 netbird 之所以是
  netbird 的东西，合计约 4 MiB flash。

**结论：29.8 MB 已经很接近"还能正常工作的 netbird"的下限，再砍只剩约 8%，代价是动
引擎逻辑。而 flash 早就不是约束了（upx 后 6 MB），真正的门槛是 33.7 MB 的常驻内存。**

## 验证记录（192.168.103.103，RV1106）

- 补丁链 0001→0002→0003 串行 `git apply --check` 全部通过，体积可复现
  （33,423,486 / 29,819,006 两次复现一致）
- `netbird version` → `development-6e17f50040dc-dirty`，upx 版可直接执行
- `--help` 命令面与预期一致
- `service run` 起守护进程成功，gRPC + JSON 两个 socket 都建出来，SIGTERM 干净退出，
  日志 0 条 ERROR/panic
- 峰值 RSS **33,748 kB**（`GOMEMLIMIT=32MiB GOGC=30`），设备未 OOM
- 设备侧测试产物已全部清理，源码树复原为 0 项改动
