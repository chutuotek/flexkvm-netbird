/*
 * netbird-cli.c - netbird daemon 的 C 控制台（替代官方 `netbird` 二进制）。
 *
 * 为什么走 JSON socket 而不是调官方 CLI：
 *   - 官方 CLI 是同一个 6MB 的 Go 二进制，每敲一次命令要付 ~30MB RSS 的启动成本，
 *     这块板子只有 207MB，netbird 自己常驻已占 33MB；
 *   - 官方 CLI 的 status 在没装 jq 时会降级成丑格式，而根文件系统里没有 jq。
 *
 * 协议要点（源码 client/cmd/service_json_gateway.go + 真机实测）：
 *   - daemon 加 --enable-json-socket 后，grpc-gateway 在 unix socket 上提供
 *     HTTP/JSON 转发。路径由 grpc-gateway 默认推导：
 *         POST /daemon.DaemonService/<MethodName>
 *     包名 daemon、服务名 DaemonService、方法名**大小写敏感**。
 *     实测 /status、/api/status、/daemon.DaemonService/status 全部 404。
 *   - 请求必须带 Content-Type: application/json（vpnctl_http_request 默认就是）。
 *   - 业务失败**不体现在 HTTP 状态上**：大多数错误是 HTTP 4xx/5xx + 正文
 *     {"code":N,"message":"..."}，但 500 里的 code 也可能是 2（Unknown），
 *     而且 200 也不能直接当成功——所以统一按正文字段判断，不用状态码做判据。
 *   - code 是 gRPC status code（google.rpc.Code），按 7/5/3 映射到 errno 便于
 *     上层和其它 app 控制台保持一致的返回语义。
 *
 * 安全：daemon 建 socket 时 chmod 0666（client/cmd/service_socket.go），
 * 任何本地进程都能无认证控制 VPN。本控制台在 up/login 成功后把它收到 0600，
 * 失败只告警不中断——收紧权限是加固，不该让一次成功连接因此回滚。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <jansson.h>

#include "vpnctl.h"

#define PROG "netbird-cli"

/* JSON socket 路径：编译期可覆盖（设备上是 /userdata/nb/nb-http.sock） */
#ifndef NB_HTTP_SOCK
#define NB_HTTP_SOCK "/userdata/nb/nb-http.sock"
#endif

/* gRPC 控制 socket：权限同样是 0666，加固时一起收 */
#ifndef NB_DAEMON_SOCK
#define NB_DAEMON_SOCK "/userdata/nb/nb.sock"
#endif

/*
 * grpc-gateway 生成的路由前缀。**不能**改成 / 或 /api：
 * 实测 /status → 404，/daemon.DaemonService/Status → 200。
 */
#define NB_RPC_PREFIX "/daemon.DaemonService"

/* 回环 unix socket 的正常往返是个位数毫秒，超时只用于兜住 daemon 假死 */
#define NB_CONNECT_TIMEOUT_MS 3000
#define NB_TIMEOUT_MS 15000
/* up/login 要等 management/signal 握手，官方 CLI 也是阻塞等，给足预算 */
#define NB_CONNECT_TIMEOUT_LONG_MS 120000
/* SSO 要等用户去浏览器点授权，官方 CLI 无限等；这里给 5 分钟，超时可重试 */
#define NB_SSO_TIMEOUT_MS 300000

/* gRPC code → 负 errno。只映射控制台会据此分支的三个码，其余归 -EIO */
static int nb_code_errno(long long code) {
    switch (code) {
    case 3:
        return -EINVAL; /* InvalidArgument */
    case 5:
        return -ENOENT; /* NotFound */
    case 7:
        return -EACCES; /* PermissionDenied */
    default:
        return -EIO;
    }
}

/*
 * 一次 RPC。成功（且正文里没有 code 字段）返回 0 并把解析后的 JSON 给 *out；
 * 失败返回负 errno 并已打印原因。*out 恒被初始化。
 *
 * body 必须是合法 JSON 对象字面量：grpc-gateway 对空正文会按 400 处理。
 */
static int nb_api(const char *rpc, const char *body, int timeout_ms,
                  json_t **out) {
    char path[128];
    vpnctl_http_opts_t opts;
    vpnctl_http_resp_t resp;
    json_t *root = NULL;
    long http_status;
    int ret;

    if (out) {
        *out = NULL;
    }

    if (snprintf(path, sizeof(path), "%s/%s", NB_RPC_PREFIX, rpc) >=
        (int)sizeof(path)) {
        return vpnctl_error(PROG, -ENAMETOOLONG, "rpc name too long: %s", rpc);
    }

    memset(&opts, 0, sizeof(opts));
    opts.unix_path = NB_HTTP_SOCK;
    opts.host = "localhost";
    opts.connect_timeout_ms = NB_CONNECT_TIMEOUT_MS;
    opts.io_timeout_ms = timeout_ms;
    opts.total_timeout_ms = timeout_ms;

    ret = vpnctl_http_request(&opts, "POST", path, NULL, body, &resp);
    if (ret != 0) {
        if (ret == -EHOSTDOWN) {
            return vpnctl_error(
                PROG, ret,
                "netbird daemon is not running (json socket %s)", NB_HTTP_SOCK);
        }
        if (ret == -ETIMEDOUT) {
            return vpnctl_error(PROG, ret, "%s timed out after %d ms", rpc,
                                timeout_ms);
        }
        return vpnctl_error(PROG, ret, "%s: transport error: %s", rpc,
                            strerror(-ret));
    }
    http_status = resp.status;

    /* 空正文只在 204 之类场景合法；这里所有 RPC 都回 JSON，空即畸形 */
    if (!resp.body || resp.body[0] == '\0') {
        vpnctl_http_resp_free(&resp);
        return vpnctl_error(PROG, -EBADMSG, "%s: empty response (HTTP %ld)",
                            rpc, http_status);
    }

    {
        json_error_t jerr;

        root = json_loads(resp.body, 0, &jerr);
        if (!root) {
            vpnctl_http_resp_free(&resp);
            return vpnctl_error(PROG, -EBADMSG,
                                "%s: bad JSON (HTTP %ld): %s", rpc, http_status,
                                jerr.text);
        }
    }
    vpnctl_http_resp_free(&resp);

    /*
     * 先看正文里的 code，再看 HTTP 状态：grpc-gateway 的 500 里 code 可能是
     * 2（Unknown，如 "client gave up to connect"），把它当成"服务端 500"会丢掉
     * 真正的原因；反过来，200 也可能是错误体，不能只信状态码。
     */
    {
        long long code = 0;

        if (vpnctl_json_int(root, "code", &code) == 0) {
            char msg[512];

            vpnctl_json_str(root, "message", msg, sizeof(msg));
            ret = nb_code_errno(code);
            vpnctl_error(PROG, ret, "%s failed (grpc code %lld, HTTP %ld): %s",
                         rpc, code, http_status,
                         msg[0] ? msg : "(daemon sent no message)");
            json_decref(root);
            return ret;
        }
    }

    if (http_status != 200) {
        /* 有正文但不是 grpc-gateway 的错误体：多半是 proxy/网关层回的 HTML */
        json_decref(root);
        return vpnctl_error(PROG, -EIO, "%s: HTTP %ld from %s", rpc, http_status,
                            NB_RPC_PREFIX);
    }

    if (out) {
        *out = root;
    } else {
        json_decref(root);
    }
    return 0;
}

/* proto 的 int64 在 JSON 里是字符串（"51820"），取不到就返回空串 */
static void nb_json_int_str(json_t *obj, const char *key, char *dst,
                            size_t dst_len) {
    if (vpnctl_json_str(obj, key, dst, dst_len) != 0) {
        long long v = 0;

        dst[0] = '\0';
        if (vpnctl_json_int(obj, key, &v) == 0) {
            snprintf(dst, dst_len, "%lld", v);
        }
    }
}

/* ---------------- kv 表渲染辅助 ---------------- */

/*
 * 库的列宽由首行数据定死且之后只增不减（见 vpnctl_output.c 的注释：不攒全部
 * 行是为了省这块 212MB 板子的内存）。kv 表的键名长短不齐，先打印的行就会
 * 短一截、后面整体右移。把键名右补空格到最宽的那个即可，值列随之对齐。
 */
static void nb_kv_key(char *buf, size_t cap, const char *key, size_t width) {
    snprintf(buf, cap, "%-*s", (int)width, key);
}

/*
 * 固定宽度取值的格子：库只在首行定列宽、之后只增不减，所以每行自己先把值
 * 截断/补齐到同一个宽度，表格左右边框就不会随行长抖动（peer 的 fqdn 尤其
 * 明显：首行 peer-a、后行 peer-b 更长时，前几行会短一截）。
 */
static void nb_fixed_cell(char *dst, size_t cap, const char *src, size_t width) {
    snprintf(dst, cap, "%-*.*s", (int)width, (int)width,
             (src && src[0]) ? src : "-");
}

static size_t nb_kv_width(const char *const *keys, size_t n) {
    size_t i, w = 0;

    for (i = 0; i < n; i++) {
        size_t l = strlen(keys[i]);

        if (l > w) {
            w = l;
        }
    }
    return w;
}

/* ---------------- status ---------------- */

/* 取 peers 里 connStatus=="Connected" 的条数，同时给出总数 */
static void nb_peer_counts(json_t *peers, int *connected, int *total) {
    size_t i, n = json_is_array(peers) ? json_array_size(peers) : 0;

    *connected = 0;
    *total = (int)n;
    for (i = 0; i < n; i++) {
        char st[64];

        vpnctl_json_str(json_array_get(peers, i), "connStatus", st,
                        sizeof(st));
        if (strcmp(st, "Connected") == 0) {
            (*connected)++;
        }
    }
}

static int cmd_status(void) {
    json_t *root = NULL;
    json_t *full;
    char daemon_status[64];
    /* 记住用户的 --no-trunc：下面为了不截 URL 会临时打开全局开关 */
    bool user_no_trunc = vpnctl_no_trunc();
    int ret;

    ret = nb_api("Status", "{\"getFullPeerStatus\":true}", NB_TIMEOUT_MS, &root);
    if (ret != 0) {
        return ret;
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(root);
        json_decref(root);
        return 0;
    }

    vpnctl_json_str(root, "status", daemon_status, sizeof(daemon_status));
    full = json_object_get(root, "fullStatus");

    /*
     * 顶层用 key/value 表，和 easytier 的 info 一致；management/signal 的
     * connected 与 error 合并进同一格，避免为两个布尔再开两列。
     */
    {
        static const char *const titles[] = {"key", "value"};
        char ver[64], mgmt[1024], sig[1024], fqdn[256], ip[128], ipv6[128];
        char wgport[32], iface[32], peers[48];
        char mgmt_url[512], mgmt_err[256], sig_url[512], sig_err[256];
        bool mgmt_up = false, sig_up = false, kernel_if = false;
        long long wg = 0;
        int conn_peers = 0, total_peers = 0;
        vpnctl_table_t t;

        vpnctl_json_str(root, "daemonVersion", ver, sizeof(ver));
        vpnctl_json_nested_str(full, "managementState", "URL", mgmt_url,
                               sizeof(mgmt_url));
        vpnctl_json_nested_str(full, "managementState", "error", mgmt_err,
                               sizeof(mgmt_err));
        vpnctl_json_nested_bool(full, "managementState", "connected", &mgmt_up);
        vpnctl_json_nested_str(full, "signalState", "URL", sig_url,
                               sizeof(sig_url));
        vpnctl_json_nested_str(full, "signalState", "error", sig_err,
                               sizeof(sig_err));
        vpnctl_json_nested_bool(full, "signalState", "connected", &sig_up);
        vpnctl_json_nested_str(full, "localPeerState", "fqdn", fqdn,
                               sizeof(fqdn));
        vpnctl_json_nested_str(full, "localPeerState", "IP", ip, sizeof(ip));
        vpnctl_json_nested_str(full, "localPeerState", "ipv6", ipv6,
                               sizeof(ipv6));
        vpnctl_json_nested_bool(full, "localPeerState", "kernelInterface",
                                &kernel_if);
        vpnctl_json_nested_int(full, "localPeerState", "wgPort", &wg);
        nb_peer_counts(json_object_get(full, "peers"), &conn_peers,
                       &total_peers);

        if (mgmt_up) {
            snprintf(mgmt, sizeof(mgmt), "%s (connected)",
                     mgmt_url[0] ? mgmt_url : "-");
        } else {
            snprintf(mgmt, sizeof(mgmt), "%s (disconnected%s%s)",
                     mgmt_url[0] ? mgmt_url : "-",
                     mgmt_err[0] ? ": " : "", mgmt_err);
        }
        if (sig_up) {
            snprintf(sig, sizeof(sig), "%s (connected)",
                     sig_url[0] ? sig_url : "-");
        } else {
            snprintf(sig, sizeof(sig), "%s (disconnected%s%s)",
                     sig_url[0] ? sig_url : "-", sig_err[0] ? ": " : "",
                     sig_err);
        }
        snprintf(wgport, sizeof(wgport), "%lld", wg);
        snprintf(iface, sizeof(iface), "%s",
                 kernel_if ? "kernel" : "userspace");
        snprintf(peers, sizeof(peers), "%d/%d connected", conn_peers,
                 total_peers);

        static const char *const keys[] = {
            "status",         "daemon_version", "management", "signal",
            "fqdn",           "ipv4",           "ipv6",       "interface",
            "wireguard_port", "peers",
        };
        const char *vals[sizeof(keys) / sizeof(keys[0])];
        char keybuf[sizeof(keys) / sizeof(keys[0])][32];
        size_t k, kw = nb_kv_width(keys, sizeof(keys) / sizeof(keys[0]));

        vals[0] = daemon_status;
        vals[1] = ver[0] ? ver : "-";
        vals[2] = mgmt;
        vals[3] = sig;
        vals[4] = fqdn[0] ? fqdn : "-";
        vals[5] = ip[0] ? ip : "-";
        vals[6] = ipv6[0] ? ipv6 : "-";
        vals[7] = iface;
        vals[8] = wg > 0 ? wgport : "-";
        vals[9] = peers;

        vpnctl_set_no_trunc(true); /* URL 很长，截断就失去诊断价值 */
        vpnctl_table_init(&t, titles, 2);
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            const char *row[2];

            nb_kv_key(keybuf[k], sizeof(keybuf[k]), keys[k], kw);
            row[0] = keybuf[k];
            row[1] = vals[k];
            vpnctl_table_row(&t, row);
        }
        vpnctl_table_finish(&t);
        vpnctl_set_no_trunc(user_no_trunc); /* 还原，别影响下面 peer 表的判断 */
    }

    /* peers 单独一张表；没有 peer 时不打空表（未登录时 total 恒为 0） */
    {
        json_t *peers = json_object_get(full, "peers");
        size_t i, n = json_is_array(peers) ? json_array_size(peers) : 0;

        if (n > 0) {
            static const char *const titles[] = {"ip", "fqdn", "status",
                                                 "relayed", "latency"};
            vpnctl_table_t t;

            /*
             * 列宽自己定：库默认按终端 100 列均分（每列 11），fqdn 会被切得
             * 没法看。16+34+11+7+9 加分隔符约 95 列，正好一屏。
             */
            enum { PEER_IP_W = 16, PEER_FQDN_W = 34, PEER_ST_W = 11,
                   PEER_REL_W = 7, PEER_LAT_W = 9 };

            /* --no-trunc 时列宽交给库自己算，别用固定宽度覆盖用户的意图 */
            bool fixed = !user_no_trunc;

            vpnctl_table_init(&t, titles, 5);
            if (fixed) {
                t.max_width[0] = PEER_IP_W;
                t.max_width[1] = PEER_FQDN_W;
                t.max_width[2] = PEER_ST_W;
                t.max_width[3] = PEER_REL_W;
                t.max_width[4] = PEER_LAT_W;
            }

            for (i = 0; i < n; i++) {
                json_t *p = json_array_get(peers, i);
                char ip[64], fqdn[256], st[64], lat[32];
                char cell[5][64];
                const char *row[5];
                const char *relayed =
                    json_is_true(json_object_get(p, "relayed")) ? "yes" : "no";

                vpnctl_json_str(p, "IP", ip, sizeof(ip));
                vpnctl_json_str(p, "fqdn", fqdn, sizeof(fqdn));
                vpnctl_json_str(p, "connStatus", st, sizeof(st));
                vpnctl_json_str(p, "latency", lat, sizeof(lat));
                /* 缺失字段统一显示 "-"，两条渲染路径都不用再判空 */
                if (ip[0] == '\0') {
                    snprintf(ip, sizeof(ip), "-");
                }
                if (fqdn[0] == '\0') {
                    snprintf(fqdn, sizeof(fqdn), "-");
                }
                if (st[0] == '\0') {
                    snprintf(st, sizeof(st), "-");
                }
                if (lat[0] == '\0') {
                    snprintf(lat, sizeof(lat), "-");
                }

                if (fixed) {
                    nb_fixed_cell(cell[0], sizeof(cell[0]), ip, PEER_IP_W);
                    nb_fixed_cell(cell[1], sizeof(cell[1]), fqdn, PEER_FQDN_W);
                    nb_fixed_cell(cell[2], sizeof(cell[2]), st, PEER_ST_W);
                    nb_fixed_cell(cell[3], sizeof(cell[3]), relayed, PEER_REL_W);
                    nb_fixed_cell(cell[4], sizeof(cell[4]), lat, PEER_LAT_W);
                    row[0] = cell[0];
                    row[1] = cell[1];
                    row[2] = cell[2];
                    row[3] = cell[3];
                    row[4] = cell[4];
                } else {
                    row[0] = ip;
                    row[1] = fqdn;
                    row[2] = st;
                    row[3] = relayed;
                    row[4] = lat;
                }
                vpnctl_table_row(&t, row);
            }
            vpnctl_table_finish(&t);
        }
    }

    /* 未登录时官方 CLI 会给出下一步提示，这里保留同样的引导但只有一句 */
    if (strcmp(daemon_status, "NeedsLogin") == 0 ||
        strcmp(daemon_status, "LoginFailed") == 0 ||
        strcmp(daemon_status, "SessionExpired") == 0) {
        printf("\nnot logged in; run `" PROG
               " up` (or `" PROG " login -k <setup-key> -m <management-url>`)\n");
    }

    json_decref(root);
    return 0;
}

/* ---------------- version ---------------- */

/*
 * netbird 没有 Version RPC（实测 /daemon.DaemonService/Version → 404），
 * daemonVersion 只能从 Status 拿。代价是 daemon 没起时拿不到版本——
 * 官方 `netbird version` 是编译期常量，不需要 daemon。
 */
static int cmd_version(void) {
    json_t *root = NULL;
    char ver[64];
    int ret;

    ret = nb_api("Status", "{}", NB_TIMEOUT_MS, &root);
    if (ret != 0) {
        return ret;
    }

    vpnctl_json_str(root, "daemonVersion", ver, sizeof(ver));
    if (ver[0] == '\0') {
        json_decref(root);
        return vpnctl_error(PROG, -ENOENT, "daemon reported no version");
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(json_string(ver));
    } else {
        printf("%s\n", ver);
    }
    json_decref(root);
    return 0;
}

/* ---------------- getconfig ---------------- */

typedef struct {
    const char *key;
    char type; /* 's' 字符串 / 'b' 布尔 / 'i' 整数 / 'p' 敏感值 */
} nb_cfg_field_t;

/*
 * 字段顺序照 daemon.proto 的 GetConfigResponse 排，方便和服务端定义对照。
 * preSharedKey 单独按 'p' 处理：它是密钥，控制和日志里只显示有没有设置。
 */
static const nb_cfg_field_t nb_cfg_fields[] = {
    {"managementUrl", 's'},
    {"adminURL", 's'},
    {"interfaceName", 's'},
    {"wireguardPort", 's'},
    {"mtu", 's'},
    {"configFile", 's'},
    {"logFile", 's'},
    {"preSharedKey", 'p'},
    {"disableAutoConnect", 'b'},
    {"disableNotifications", 'b'},
    {"networkMonitor", 'b'},
    {"lazyConnectionEnabled", 'b'},
    {"disableDns", 'b'},
    {"disableClientRoutes", 'b'},
    {"disableServerRoutes", 'b'},
    {"blockInbound", 'b'},
    {"blockLanAccess", 'b'},
    {"serverSSHAllowed", 'b'},
    {"rosenpassEnabled", 'b'},
    {"rosenpassPermissive", 'b'},
    {"enableSSHRoot", 'b'},
    {"enableSSHSFTP", 'b'},
    {"enableSSHLocalPortForwarding", 'b'},
    {"enableSSHRemotePortForwarding", 'b'},
    {"disableSSHAuth", 'b'},
    {"sshJWTCacheTTL", 'i'},
    {"disableIpv6", 'b'},
    {"remoteJobsAllowed", 'b'},
};

static int cmd_getconfig(void) {
    json_t *root = NULL;
    int ret;

    /* profileName 必给：空串时 daemon 回 "profile handle is empty" */
    ret = nb_api("GetConfig", "{\"profileName\":\"default\"}", NB_TIMEOUT_MS,
                 &root);
    if (ret != 0) {
        return ret;
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(root);
        json_decref(root);
        return 0;
    }

    {
        static const char *const titles[] = {"key", "value"};
        vpnctl_table_t t;
        size_t i;

        size_t nfields = sizeof(nb_cfg_fields) / sizeof(nb_cfg_fields[0]);
        size_t kw = 0;

        for (i = 0; i < nfields; i++) {
            size_t l = strlen(nb_cfg_fields[i].key);

            if (l > kw) {
                kw = l;
            }
        }

        vpnctl_set_no_trunc(true); /* 路径/URL 不能截 */
        vpnctl_table_init(&t, titles, 2);
        for (i = 0; i < nfields; i++) {
            const nb_cfg_field_t *f = &nb_cfg_fields[i];
            char val[512];
            char key[64];
            const char *row[2];

            switch (f->type) {
            case 'b': {
                bool b = false;

                if (vpnctl_json_bool(root, f->key, &b) == 0) {
                    snprintf(val, sizeof(val), "%s", b ? "true" : "false");
                } else {
                    snprintf(val, sizeof(val), "-");
                }
                break;
            }
            case 'i': {
                long long v = 0;

                if (vpnctl_json_int(root, f->key, &v) == 0) {
                    snprintf(val, sizeof(val), "%lld", v);
                } else {
                    snprintf(val, sizeof(val), "-");
                }
                break;
            }
            case 'p':
                vpnctl_json_str(root, f->key, val, sizeof(val));
                snprintf(val, sizeof(val), "%s",
                         val[0] ? "(set)" : "(empty)");
                break;
            default: /* 's'：int64 字段在 JSON 里是字符串，取不到再试数字 */
                nb_json_int_str(root, f->key, val, sizeof(val));
                if (val[0] == '\0') {
                    snprintf(val, sizeof(val), "-");
                }
                break;
            }

            nb_kv_key(key, sizeof(key), f->key, kw);
            row[0] = key;
            row[1] = val;
            vpnctl_table_row(&t, row);
        }
        vpnctl_table_finish(&t);
    }

    json_decref(root);
    return 0;
}

/* ---------------- features ---------------- */

static int cmd_features(void) {
    json_t *root = NULL;
    int ret;

    ret = nb_api("GetFeatures", "{}", NB_TIMEOUT_MS, &root);
    if (ret != 0) {
        return ret;
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(root);
        json_decref(root);
        return 0;
    }

    {
        static const char *const titles[] = {"key", "value"};
        /* disableAdvancedView 是 optional，老 daemon 不回该字段 */
        static const char *const keys[] = {"disableProfiles",
                                           "disableUpdateSettings",
                                           "disableNetworks",
                                           "disableAdvancedView"};
        vpnctl_table_t t;
        size_t i;

        size_t nkeys = sizeof(keys) / sizeof(keys[0]);
        size_t kw = nb_kv_width(keys, nkeys);

        vpnctl_table_init(&t, titles, 2);
        for (i = 0; i < nkeys; i++) {
            char val[16];
            char key[32];
            bool b = false;
            const char *row[2];

            if (vpnctl_json_bool(root, keys[i], &b) == 0) {
                snprintf(val, sizeof(val), "%s", b ? "true" : "false");
            } else {
                snprintf(val, sizeof(val), "-");
            }
            nb_kv_key(key, sizeof(key), keys[i], kw);
            row[0] = key;
            row[1] = val;
            vpnctl_table_row(&t, row);
        }
        vpnctl_table_finish(&t);
    }

    json_decref(root);
    return 0;
}

/* ---------------- 权限加固 ---------------- */

/*
 * 把 daemon 建的 socket 从 0666 收到 0600。
 *
 * 只在 up/login 成功后调用：daemon 重启会重新建 socket 并 chmod 0666，
 * 所以每次连接成功后补一刀，覆盖"重启后第一次 up"这个窗口。
 * 不存在（daemon 换过路径）或已是 0600 时静默跳过，失败只告警——
 * 权限收紧是附加收益，不能因为它失败就让已经建立的连接报错。
 */
static void nb_harden_socket(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
        return;
    }
    if ((st.st_mode & 0077) == 0) {
        return;
    }
    if (chmod(path, 0600) != 0) {
        fprintf(stderr, "%s: warning: cannot chmod %s to 0600: %s\n", PROG,
                path, strerror(errno));
        return;
    }
    /* 只在真的改了权限时提示一次，之后同一次 daemon 生命周期内会静默 */
    fprintf(stderr, "%s: note: tightened %s permissions to 0600\n", PROG, path);
}

static void nb_harden_sockets(void) {
    nb_harden_socket(NB_HTTP_SOCK);
    nb_harden_socket(NB_DAEMON_SOCK);
}

/* ---------------- up / down / logout ---------------- */

static int cmd_up(void) {
    json_t *root = NULL;
    int ret;

    /* Up 默认阻塞到引擎就绪（async=false），所以用长超时 */
    ret = nb_api("Up", "{}", NB_CONNECT_TIMEOUT_LONG_MS, &root);
    if (ret != 0) {
        return ret;
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(root);
    } else {
        printf("Connected\n");
    }
    json_decref(root);

    nb_harden_sockets();
    return 0;
}

static int cmd_down(void) {
    json_t *root = NULL;
    int ret;

    ret = nb_api("Down", "{}", NB_TIMEOUT_MS, &root);
    if (ret != 0) {
        return ret;
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(root);
    } else {
        printf("Disconnected\n");
    }
    json_decref(root);
    return 0;
}

static int cmd_logout(void) {
    json_t *root = NULL;
    int ret;

    ret = nb_api("Logout", "{}", NB_TIMEOUT_MS, &root);
    if (ret != 0) {
        return ret;
    }

    if (vpnctl_json_mode()) {
        vpnctl_print_json(root);
    } else {
        printf("Deregistered successfully\n");
    }
    json_decref(root);
    return 0;
}

/* ---------------- login ---------------- */

/* WaitSSOLogin：daemon 会阻塞到用户在浏览器完成授权 */
static int nb_wait_sso(const char *user_code) {
    json_t *req = json_object();
    json_t *resp = NULL;
    char *body;
    char email[256];
    int ret;

    if (!req) {
        return -ENOMEM;
    }
    if (json_object_set_new(req, "userCode", json_string(user_code)) != 0) {
        json_decref(req);
        return -ENOMEM;
    }
    body = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (!body) {
        return -ENOMEM;
    }

    ret = nb_api("WaitSSOLogin", body, NB_SSO_TIMEOUT_MS, &resp);
    free(body);
    if (ret != 0) {
        return ret;
    }

    /* 只有交互模式会走到这里（--json 在 Login 阶段就返回了），不用再分模式 */
    vpnctl_json_str(resp, "email", email, sizeof(email));
    if (email[0]) {
        printf("Login successful (%s)\n", email);
    } else {
        printf("Login successful\n");
    }
    json_decref(resp);
    return 0;
}

static int cmd_login(int argc, char **argv) {
    const char *setup_key = NULL;
    const char *mgmt_url = NULL;
    bool no_wait = false;
    char user_code[64] = "";
    char uri[512] = "";
    json_t *req = NULL;
    json_t *resp = NULL;
    char *body = NULL;
    int i;
    int ret;

    for (i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "-k") == 0 ||
             strcmp(argv[i], "--setup-key") == 0) &&
            i + 1 < argc) {
            setup_key = argv[++i];
        } else if ((strcmp(argv[i], "-m") == 0 ||
                    strcmp(argv[i], "--management-url") == 0) &&
                   i + 1 < argc) {
            mgmt_url = argv[++i];
        } else if (strcmp(argv[i], "--no-wait") == 0) {
            no_wait = true;
        } else {
            return vpnctl_error(PROG, -EINVAL, "login: unknown argument: %s",
                                argv[i]);
        }
    }

    /*
     * 用 jansson 拼请求体而不是 snprintf：setup key / URL 里可能有需要转义的
     * 字符，手拼容易做出非法 JSON。缺省字段不出现，daemon 按"未提供"处理。
     */
    req = json_object();
    if (!req) {
        return -ENOMEM;
    }
    if (setup_key && setup_key[0]) {
        json_object_set_new(req, "setupKey", json_string(setup_key));
    }
    if (mgmt_url && mgmt_url[0]) {
        json_object_set_new(req, "managementUrl", json_string(mgmt_url));
    }
    body = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (!body) {
        return -ENOMEM;
    }

    ret = nb_api("Login", body, NB_CONNECT_TIMEOUT_LONG_MS, &resp);
    free(body);
    if (ret != 0) {
        return ret;
    }

    {
        bool needs_sso = false;

        vpnctl_json_bool(resp, "needsSSOLogin", &needs_sso);
        if (!needs_sso) {
            /* 用 setup key 直接注册成功；JSON 模式把整个响应交给调用方 */
            if (vpnctl_json_mode()) {
                vpnctl_print_json(resp);
            } else {
                printf("Login successful\n");
            }
            json_decref(resp);
            nb_harden_sockets();
            return 0;
        }

        vpnctl_json_str(resp, "userCode", user_code, sizeof(user_code));
        vpnctl_json_str(resp, "verificationURIComplete", uri, sizeof(uri));

        /*
         * JSON 模式只交出 Login 的响应就结束：WaitSSOLogin 会阻塞到用户在
         * 浏览器点完授权，塞进脚本的 stdout 里没有意义。
         */
        if (vpnctl_json_mode()) {
            vpnctl_print_json(resp);
            json_decref(resp);
            return 0;
        }
        json_decref(resp);

        /* 与官方 CLI 一致：给出可直接打开的完整 URL 和短码 */
        printf("Please open the following URL in your browser and authorize "
               "this device:\n\n  %s\n\n", uri[0] ? uri : "(no URL)");
        if (user_code[0]) {
            printf("Device code: %s\n", user_code);
        }
        if (no_wait) {
            printf("\nNot waiting (--no-wait). Authorize, then run `" PROG
                   " status`.\n");
            return 0;
        }
        if (user_code[0] == '\0') {
            return vpnctl_error(PROG, -EBADMSG,
                                "daemon asked for SSO but sent no userCode");
        }
        printf("\nWaiting for authorization...\n");
    }

    ret = nb_wait_sso(user_code);
    if (ret == 0) {
        nb_harden_sockets();
    }
    return ret;
}

/* ---------------- 入口 ---------------- */

static void usage(void) {
    printf("Usage: " PROG " <command> [args]\n\n"
           "Commands:\n"
           "  status                 show daemon, management and peers\n"
           "  up                     connect\n"
           "  down                   disconnect\n"
           "  login [-k KEY] [-m URL] [--no-wait]\n"
           "                         log in (setup key, or SSO if -k omitted)\n"
           "  logout                 deregister this peer\n"
           "  getconfig              show the daemon's configuration\n"
           "  features               show daemon feature flags\n"
           "  version                print the daemon version\n\n"
           "Global flags:\n"
           "  -o json | --json       output JSON instead of a table\n"
           "  --no-trunc             do not truncate table columns\n");
}

int main(int argc, char **argv) {
    static const char *const cmds[] = {
        "up",   "down",   "status",     "login",   "logout",
        "getconfig", "features", "version", "help",
    };
    int i, idx;
    int rc;

    /* 先摘全局开关再分发：和 tailscale/zerotier 控制台一致，开关可放任意位置 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0 ||
            (strcmp(argv[i], "-o") == 0 && i + 1 < argc &&
             strcmp(argv[i + 1], "json") == 0)) {
            vpnctl_set_json_mode(true);
            if (argv[i][1] == 'o') {
                memmove(&argv[i], &argv[i + 2],
                        sizeof(char *) * (size_t)(argc - i - 2));
                argc -= 2;
            } else {
                memmove(&argv[i], &argv[i + 1],
                        sizeof(char *) * (size_t)(argc - i - 1));
                argc -= 1;
            }
            i--;
        } else if (strcmp(argv[i], "--no-trunc") == 0) {
            vpnctl_set_no_trunc(true);
            memmove(&argv[i], &argv[i + 1],
                    sizeof(char *) * (size_t)(argc - i - 1));
            argc -= 1;
            i--;
        }
    }

    idx = vpnctl_match_cmd(argc, argv, cmds,
                           (int)(sizeof(cmds) / sizeof(cmds[0])));
    if (idx < 0) {
        usage();
        return 1;
    }

    switch (idx) {
    case 0:
        rc = cmd_up();
        break;
    case 1:
        rc = cmd_down();
        break;
    case 2:
        rc = cmd_status();
        break;
    case 3:
        rc = cmd_login(argc, argv);
        break;
    case 4:
        rc = cmd_logout();
        break;
    case 5:
        rc = cmd_getconfig();
        break;
    case 6:
        rc = cmd_features();
        break;
    case 7:
        rc = cmd_version();
        break;
    default:
        usage();
        rc = 0;
        break;
    }
    return rc == 0 ? 0 : 1;
}
