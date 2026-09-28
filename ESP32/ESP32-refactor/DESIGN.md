# ESP32-refactor：多设备建模 THP 固件

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
│ thp_sched  单任务周期流水线                               │
│   校时 → LOCAL 采样上报 → 等 BLE 窗 → 各 BLE 上报 → 补传   │
└───────────────┬─────────────────────────────────────────┘
                │ thp_reading_t { source_id, T/H/P, iso, rssi }
    ┌───────────┼──────────────┬──────────────────────────┐
    ▼           ▼              ▼                          ▼
 thp_local   thp_ble       thp_queue                 thp_report
 SHT40/BMP   多设备网关     RAM 环形                  HTTPS + Token
    │           │                                      │
    │      atc_ble (NimBLE)                            │
    │      多 MAC / 多 BindKey / 窗口环按设备            │
    ▼           ▼                                      ▼
 I2C 传感器   ATC/BTHome 广播                      Cloudflare Worker
```

## 多设备模型

### 源注册表 `thp_sources`

| source_id | kind | token 来源 |
|-----------|------|------------|
| 0 | LOCAL | `THP_LOCAL_TOKEN` |
| 1..N | BLE | `THP_BLE_DEVICES[i].token` |

`thp_reading_t.source_id` 贯穿上报 / 离线队列 / 补传，Token 在 `thp_report` 内按 id 解析。

### atc_ble 多设备

- `atc_ble_init(devs, n)`：设备表（MAC 显示序 + 可选 BindKey）
- 广播按 **adv MAC** 匹配表项；加密帧用该表项 BindKey 解密
- 窗口环缓存带 `dev_index`；`atc_ble_pop_window_best(dev_index, ref)` 按设备取距 T 最近帧
- BTHome 加密防重放 counter **按设备** 记

### 配置（`thp_config.h`）

```c
#define THP_LOCAL_TOKEN  "thp_..."
#define THP_BLE_DEVICES { \
  { .name="客厅", .mac="A4:C1:38:..", .bindkey="..", .token="thp_..." }, \
  { .name="卧室", .mac="A4:C1:38:..", .bindkey="",   .token="thp_..." }, \
}
```

## 调度（窗内零 HTTP）

| 阶段 | 时刻 | 行为 |
|------|------|------|
| 0 开 BLE 窗 | T−5s | 所有配置的 BLE 设备共用一次扫描 |
| 1 LOCAL 采样 | T | **仅 I2C**，不发 HTTP（避免 TLS 重试 + stop_scan 饿死窗口） |
| 2 等窗 | T→T+10s | 纯扫描收集；禁止 NTP/HTTP |
| 3 关窗 + 校时 | T+10s | 关 BLE 窗 → SNTP（已同步短路） |
| 4 上报 | 之后 | LOCAL → 各 BLE 最佳帧 → 补传 |
| 5 睡眠 | — | 睡到下一 T′−5s |

deadline = `period − margin`；耗尽时不再 HTTP，读数仍入队。

**设计意图**：C3 单射频，HTTP `perform` 会 `stop_scan`。若 LOCAL 在窗内带重试上报，TLS 超时×4 可吃掉整个 15s 扫描窗（现场已复现 `win_n=0`）。故采样与上报解耦。

## 相对原版的行为差异

| 项 | 原版 | 重构 |
|----|------|------|
| BLE 设备数 | 1（`THP_MI_*`） | N（`THP_BLE_DEVICES[]`） |
| Token | LOCAL/MI 两套宏 | 源表按 `source_id` |
| Token 失效清队列 | 整表清空 | **只清该 source** 的积压 |
| 解析过滤 | 单 MAC | 表驱动 MAC + 每设备 BindKey |

## 不变量（必须保持）

1. 温湿度仅 SHT40 / BLE 计；气压仅 BMP280；BMP 温度不进业务字段
2. 部分上报：缺字段 JSON 整体省略；T/H 不得只报一个；metrics 全空则丢弃该帧
3. 范围校验：T −40~85，H 0~100，P 300~1200
4. 实时上报不带 `ts`；`measured_at` / 离线补传 `ts` 均为**采样时刻** UTC
   （LOCAL=T 采样 mono，BLE=帧接收 mono；NTP 后按 mono 回填，不用组帧时刻）
5. 401/403/400 不重试；网络/5xx 有限指数退避
6. HTTP perform 期间停 BLE 扫描；**扫描窗内不发起 HTTP/NTP**
7. Token 401/403：仅清该源积压，并停用该源（`thp_source_set_ready(false)`），避免每周期空打
