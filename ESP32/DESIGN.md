# ESP32-refactor：多设备建模 THP 固件

> 状态：重构完成（`bb68930` → `312d637`）。协议与云端 Worker 与原 `ESP32/` 一致，可直接替换部署。

## 目标

在原 `ESP32/` 固件语义（协议、调度、离线补传、TLS、部分上报）不变的前提下，
把 **数据源** 抽象为可配置的多设备表：

- **LOCAL**：板载 SHT40（T/H）+ BMP280（P），固定 1 路
- **BLE**：0..N 台 pvvx ATC / BTHome v2 温湿度计，每台独立 MAC / BindKey / 上报 Token

云端 Worker 协议零改动：每个源一条 `POST /api/v1/readings`，Token 对应 Dash 上各自的 device。

## 风格锚点

嵌入式固件工程文档：短、准、可对照实现。无营销文案。

## 架构

```
┌─────────────────────────────────────────────────────────┐
│ thp_sched  单任务周期流水线 thp_cycle                     │
│   开窗 → LOCAL 采样 → 等窗关 → 校时 → LOCAL/各 BLE/补传   │
└───────────────┬─────────────────────────────────────────┘
                │ thp_reading_t { source_id, T/H/P, iso, rssi }
    ┌───────────┼──────────────┬──────────────────────────┐
    ▼           ▼              ▼                          ▼
 thp_local   thp_ble       thp_queue                 thp_report
 SHT40/BMP   多设备网关     RAM 环形(按源)            HTTPS + Token
    │           │                                      │
    │      atc_ble (NimBLE)                            │
    │      多 MAC / 多 BindKey / 窗口环按设备            │
    ▼           ▼                                      ▼
 I2C 传感器   ATC/BTHome 广播                      Cloudflare Worker
```

模块职责：

| 模块 | 职责 |
|------|------|
| `thp_sources` | 源注册表：id / kind / name / token / device_id / ready |
| `thp_types` | `thp_reading_t` / `thp_sample_t` / 量程 / deadline 工具 |
| `thp_sched` | 周期流水线、deadline 预算、开窗/关窗时序 |
| `thp_local` | I2C 中值采样、读失败摘挂载、热修复重 probe |
| `thp_ble` | 多设备编排：窗口 open/realign/close、逐源取帧上报 |
| `thp_queue` | 离线 RAM 环形；`clear_source` 按源压缩 |
| `thp_report` | JSON 组装、TLS 信任、重试/补传、Token 失效停用 |
| `thp_time` | SNTP、ISO-8601、**按采样 mono 回填**时间戳 |
| `thp_wifi` | STA + RSSI |
| `atc_ble` | 表驱动扫描解析（ATC/BTHome 明文+加密） |

## 多设备模型

### 源注册表 `thp_sources`

| source_id | kind | token 来源 | name 默认 |
|-----------|------|------------|-----------|
| 0 | LOCAL | `THP_LOCAL_TOKEN` | `local` |
| 1..N | BLE | `THP_BLE_DEVICES[i].token` | 表项 `.name` |

- `thp_reading_t.source_id` 贯穿上报 / 离线队列 / 补传，Token 在 `thp_report` 内按 id 解析。
- Token 为占位符（`thp_replace_me*`）或过短时，该源 `ready=false`，启动即跳过。
- Token 401/403：只清该源积压，并 `thp_source_set_ready(false)` 停用，避免每周期空打。

### atc_ble 多设备

- `atc_ble_init(devs, n)`：设备表（MAC 显示序 + 可选 BindKey）
- 广播按 **adv MAC** 匹配表项；加密帧用该表项 BindKey 解密
- 窗口环缓存带 `dev_index`；`atc_ble_pop_window_best(dev_index, ref)` 按设备取距 T 最近帧
- BTHome 加密防重放 counter **按设备** 记
- 扫描互斥：HTTP `perform` 前 `atc_ble_stop_scan()`，结束后 `resume_scan_if_wanted()`

### 配置（`thp_config.h`，模板 `thp_config.h.example`）

```c
#define THP_LOCAL_TOKEN  "thp_..."
#define THP_LOCAL_DEVICE_ID "ESP32-C3"   /* 可选，须与 Token 绑定一致 */

#define THP_BLE_ENABLE 1
#define THP_BLE_DEVICES { \
  { .name="客厅", .mac="A4:C1:38:..", .bindkey="..", .token="thp_...", .device_id="room1" }, \
  { .name="卧室", .mac="A4:C1:38:..", .bindkey="",   .token="thp_...", .device_id="" }, \
}
```

- `mac` 显示序（如 `A4:C1:38:E2:4E:43`）
- `bindkey` 32 hex；明文 beacon 可 `""` 或全 0
- 建议 ≤4 台（每台每周期一条 HTTPS，deadline 预算有限）；硬上限 `ATC_BLE_MAX_DEVICES=8`

## 调度（窗内零 HTTP/NTP）

单 FreeRTOS 任务 `thp_cycle`（栈 8KB）。BLE 窗：周期起点 T 前 `THP_BLE_SCAN_OPEN_BEFORE_MS`（默认 5s）开扫，T+`THP_BLE_SCAN_CLOSE_AFTER_MS`（默认 10s）关窗。

| 阶段 | 时刻 | 行为 |
|------|------|------|
| 0 开 BLE 窗 | T−5s | 所有配置的 BLE 设备共用一次扫描；`window_open(est_ref)` |
| 1 LOCAL 采样 | T | **仅 I2C**，记 `sample_mono_ms`；不发 HTTP（避免 TLS 重试 + stop_scan 饿死窗口） |
| 2 等窗 | T→T+10s | 纯扫描收集；禁止 NTP/HTTP |
| 3 关窗 + 校时 | T+10s | 关 BLE 窗 → `thp_time_sync_before_report()`（已同步则每 `THP_NTP_RESYNC_EVERY_CYCLES` 才真同步） |
| 4 上报 | 之后 | LOCAL 组帧上报 → 各 BLE 最佳帧 → 补传 |
| 5 睡眠 | — | 睡到下一 T′−5s；超时则立刻开下一窗 |

周期开始时 `thp_ble_scan_window_realign(cycle_ref_ms)`，取帧与时间戳共用同一 ref。

deadline = `cycle_start + period − THP_CYCLE_DEADLINE_MARGIN_MS`（默认 margin 20s）。
耗尽时不再 HTTP，读数仍入队。补传还要求剩余 ≥ `THP_FLUSH_MIN_REMAIN_MS`（10s）。

**设计意图**：C3 单射频，HTTP `perform` 会 `stop_scan`。若 LOCAL 在窗内带重试上报，TLS 超时×4 可吃掉整个 15s 扫描窗（现场已复现 `win_n=0`）。故采样与上报解耦。

## 时间戳（采样 mono 回填）

| 场景 | `measured_at` | `ts` |
|------|---------------|------|
| 实时上报 | 采样时刻 UTC | **不发送** |
| 离线补传 | 采样时刻 UTC | 采样时刻 UTC（服务端按 `ts` 落点） |

- LOCAL：`sample_mono_ms` = T 时刻 `esp_timer_get_time()/1000`
- BLE：`mi.ts_ms` = **帧接收** mono，不是组帧/HTTP 时刻
- `thp_time_fill_sample_iso()`：NTP 已同步时，用「当前墙钟 − mono 偏移」回填采样时刻；偏移 <0 或 >3h 则退回「现在」
- NTP 未就绪：`has_iso=false`，实时可退回组帧时刻 `thp_time_format_iso()`；补传无 `ts` 则服务端按入库时间

## TLS 信任（BUNDLE / PINNED / NONE 三选一）

`esp_http_client` 中 **`crt_bundle_attach` 优先于 `cert_pem`**，两者不可叠写。

| `THP_TLS_TRUST` | 值 | 行为 |
|-----------------|----|------|
| `BUNDLE` | 0（默认） | 系统证书捆绑包，覆盖公有 CA |
| `PINNED` | 1 | 仅 `thp_tls_trust.h` 内嵌根 PEM（现 GTS Root R4） |
| `NONE` | 2 | 不校验（仅调试；需 sdkconfig 开 `ESP_TLS_INSECURE` + `SKIP_SERVER_CERT_VERIFY`） |

- 兼容旧宏：`THP_HTTP_SKIP_VERIFY=1` 等价 `TRUST_NONE`
- bundle 对 GTS 链报 `No matching trusted root`（espressif/esp-idf#18674）时改 `PINNED`
- 换域名/换 CA：更新 `thp_tls_trust.h` 内嵌 PEM，勿手写臆造证书
- `CONFIG_LWIP_IPV6=n`：规避 Cloudflare AAAA 在家用宽带连不通

## 相对原版的行为差异

| 项 | 原版 `ESP32/` | 重构 `ESP32-refactor` |
|----|---------------|------------------------|
| BLE 设备数 | 1（`THP_MI_*`） | N（`THP_BLE_DEVICES[]`） |
| Token | LOCAL/MI 两套宏 | 源表按 `source_id` |
| Token 失效清队列 | 整表清空 | **只清该 source** 的积压并停用该源 |
| 解析过滤 | 单 MAC | 表驱动 MAC + 每设备 BindKey |
| LOCAL 采样/上报 | 同阶段 | **采样在 T，上报在关窗后** |
| 时间戳 | 组帧时刻 | **采样 mono 回填** |
| TLS | 内嵌 GTS + bundle | **三选一**（BUNDLE/PINNED/NONE） |
| I2C 读失败 | 等下周期 | 失败摘句柄，下周期热修复 re-probe |

## 不变量（必须保持）

1. 温湿度仅 SHT40 / BLE 计；气压仅 BMP280；BMP 温度不进业务字段
2. 部分上报：缺字段 JSON 整体省略；T/H 不得只报一个；metrics 全空则丢弃该帧（禁止 `{,"rssi"…}`）
3. 范围校验：T −40~85，H 0~100，P 300~1200；超范围字段不写入本帧
4. 实时上报不带 `ts`；`measured_at` / 离线补传 `ts` 均为**采样时刻** UTC
   （LOCAL=T 采样 mono，BLE=帧接收 mono；NTP 后按 mono 回填，不用组帧时刻）
5. 401/403/400 不重试；网络/5xx 有限指数退避（base 2s × 2^n，默认最多 3 次）
6. HTTP perform 期间停 BLE 扫描；**扫描窗内不发起 HTTP/NTP**
7. Token 401/403：仅清该源积压，并停用该源（`thp_source_set_ready(false)`），避免每周期空打
8. 离线队列仅 RAM、条目带 `source_id`；`peek→网络→成功再 pop` 单任务串行

## 构建产物

- 目标：`esp32c3`，Flash 4MB，factory 2MB（`partitions.csv`）
- 产物：`build/esp32c3_thp_multi.bin` / `esp32c3_thp_multi_flashed.bin`
- BLE：NimBLE（Bluedroid 关），Wi-Fi+BLE 共存，仅扫描不连接
