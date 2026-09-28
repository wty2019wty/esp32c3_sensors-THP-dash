# THP 多设备固件（ESP32）

按 **多设备源表** 建模的 ESP32-C3 固件：本机 SHT40+BMP280 + N 台 ATC/BTHome BLE 温湿度计，
各自 Token 上报到 THP Dash（`POST /api/v1/readings`）。协议与云端 Worker 一致。

## 硬件

| 器件 | 作用 | 接口 |
| --- | --- | --- |
| ESP32-C3 | MCU + Wi-Fi + BLE | — |
| SHT40 | 本机温湿度 | I2C 0x44（备用 0x45），SDA=GPIO8 SCL=GPIO9 |
| BMP280 | 本机气压 | I2C 0x76（备用 0x77） |
| ATC/BTHome ×N | 远端温湿度 | BLE 被动扫描（不连接） |

VCC **3.3V（严禁 5V）**。I2C 走线建议短、外接 4.7kΩ 上拉；GPIO8 板载 LED 可能干扰 SDA。

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

| source_id | kind | token 来源 | name 默认 |
|-----------|------|------------|-----------|
| 0 | LOCAL | `THP_LOCAL_TOKEN` | `local` |
| 1..N | BLE | `THP_BLE_DEVICES[i].token` | 表项 `.name` |

- `thp_reading_t.source_id` 贯穿上报 / 离线队列 / 补传，Token 在 `thp_report` 内按 id 解析
- Token 为占位符（`thp_replace_me*`）或过短时，该源 `ready=false`，启动即跳过
- Token 401/403：只清该源积压，并 `thp_source_set_ready(false)` 停用

atc_ble：`atc_ble_init(devs, n)` 表驱动；广播按 **adv MAC** 匹配，加密帧用该表项 BindKey；
窗口环带 `dev_index`，`atc_ble_pop_window_best` 取距 T 最近帧；BTHome 防重放 counter **按设备**记；
HTTP `perform` 前 `stop_scan`，结束后 `resume_scan_if_wanted`。

## 目录

```
ESP32/
├── README.md                    # 本文：硬件 / 架构 / 配置 / 调度 / 排查
├── CMakeLists.txt / sdkconfig.defaults / partitions.csv
├── main/
│   ├── main.c                   # boot：NVS → 源表/传感器/队列/Wi-Fi → 调度
│   ├── thp_sources.c/h          # 多源注册表（LOCAL + BLE[]）
│   ├── thp_types.h              # reading / sample / 结果码 / 量程
│   ├── thp_config.h.example     # 配置模板（复制为 thp_config.h）
│   ├── thp_sched.c/h            # 周期流水线
│   ├── thp_local.c/h            # I2C 本机采样（SHT40/BMP280，中值+热修复）
│   ├── thp_ble.c/h              # 多设备 BLE 网关编排
│   ├── thp_wifi.c/h / thp_time.c/h / thp_queue.c/h / thp_report.c/h
│   └── thp_tls_trust.h / i2c_config.h
└── components/
    ├── sht40/ bmp280/           # 驱动
    └── atc_ble/                 # 多设备 ATC/BTHome 扫描解析
```

## 配置

```powershell
cd ESP32
copy main\thp_config.h.example main\thp_config.h
```

编辑 `main/thp_config.h`（**勿提交**，已 gitignore）：

| 宏 | 说明 |
| --- | --- |
| `THP_WIFI_SSID` / `THP_WIFI_PASSWORD` | STA |
| `THP_WIFI_STA_TX_POWER_DBM` | 默认 15（8~20）。天线差的 Super Mini 可降到 8~12 |
| `THP_API_BASE` | Worker 根地址，**不含** `/api/...`。本地 `http://<电脑局域网IP>:8787`；线上 `https://...`。**不要写 127.0.0.1** |
| `THP_LOCAL_TOKEN` | 本机 I2C 源 Token |
| `THP_LOCAL_DEVICE_ID` | 可选；须与 Token 绑定设备一致，一般留空 |
| `THP_BLE_ENABLE` | 1=启用 BLE 源表 |
| `THP_BLE_DEVICES` | BLE 设备表：`name` / `mac` / `bindkey` / `token` / `device_id` |
| `THP_REPORT_PERIOD_MS` | 默认 5 分钟 |
| `THP_REPORT_MAX_RETRIES` / `THP_REPORT_RETRY_BASE_MS` | 网络/5xx 退避，默认 3 次 / 2s 基值 |
| `THP_OFFLINE_QUEUE_LEN` | 离线队列容量，默认 256 |
| `THP_OFFLINE_FLUSH_MAX_PER_CYCLE` | 每周期最多补传条数，默认 24 |
| `THP_HTTP_TIMEOUT_MS` | 单次 HTTP 超时，默认 15000 |
| `THP_CYCLE_DEADLINE_MARGIN_MS` | 周期 deadline 余量，默认 20000 |
| `THP_NTP_*` | 多 NTP 源 / 同步超时 / 每 N 周期真重同步 |
| `THP_TLS_TRUST` | TLS 信任：0=BUNDLE（默认） 1=PINNED 2=NONE（调试） |
| `THP_BLE_SCAN_OPEN_BEFORE_MS` / `CLOSE_AFTER_MS` | BLE 窗 T−5s ~ T+10s |
| `THP_BLE_MAX_AGE_MS` | 样本距 T 最大接受距离，默认 3 个周期 |
| `THP_BLE_HEAP_MIN_REPORT` | heap 低于此值只入队不 HTTP，默认 70000 |

设备表示例：

```c
#define THP_LOCAL_TOKEN     "thp_..."
#define THP_LOCAL_DEVICE_ID "ESP32-C3"   /* 可选 */
#define THP_BLE_ENABLE 1
#define THP_BLE_DEVICES { \
  { .name="客厅", .mac="A4:C1:38:..", .bindkey="..", .token="thp_...", .device_id="room1" }, \
  { .name="卧室", .mac="A4:C1:38:..", .bindkey="",   .token="thp_...", .device_id="" }, \
}
```

- `mac` 显示序（如 `A4:C1:38:E2:4E:43`）；`bindkey` 32 hex，明文可 `""` 或全 0
- 每台 BLE 在 Dash **各建设备、各生成 Token**
- 建议 ≤4 台（每台每周期一条 HTTPS）；硬上限 `ATC_BLE_MAX_DEVICES=8`

### TLS 信任怎么选

`esp_http_client` 中 **`crt_bundle_attach` 优先于 `cert_pem`**，两者不可叠写。

| 模式 | 值 | 何时用 |
| --- | --- | --- |
| BUNDLE | 0 | 默认。系统证书包，覆盖公有 CA |
| PINNED | 1 | bundle 报 `No matching trusted root`（GTS 链，idf#18674）；信任 `thp_tls_trust.h` 内嵌根 |
| NONE | 2 | **仅调试**。需 sdkconfig 开 `CONFIG_ESP_TLS_INSECURE` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` |

兼容旧宏 `THP_HTTP_SKIP_VERIFY=1` ≡ `TRUST_NONE`。换域名/换 CA 时更新 `thp_tls_trust.h` 内嵌 PEM。
默认 `CONFIG_LWIP_IPV6=n`，规避 Cloudflare AAAA 连不通。

## 调度（窗内零 HTTP/NTP）

单 FreeRTOS 任务 `thp_cycle`（栈 8KB）。BLE 窗：T 前 `THP_BLE_SCAN_OPEN_BEFORE_MS`（默认 5s）开扫，T+`THP_BLE_SCAN_CLOSE_AFTER_MS`（默认 10s）关窗。

```
T−5s          T              T+10s         之后
 │ 开 BLE 窗    │ 仅 I2C 采样   │ 关窗 + 校时   │ LOCAL → 各 BLE → 补传 → 睡眠
 └──────── 纯扫描，禁止 HTTP/NTP ────────┘
```

| 阶段 | 时刻 | 行为 |
|------|------|------|
| 0 开 BLE 窗 | T−5s | 所有 BLE 设备共用一次扫描；`window_open(est_ref)` |
| 1 LOCAL 采样 | T | **仅 I2C**，记 `sample_mono_ms`；不发 HTTP |
| 2 等窗 | T→T+10s | 纯扫描收集；禁止 NTP/HTTP |
| 3 关窗 + 校时 | T+10s | 关窗 → `thp_time_sync_before_report()`（已同步则每 `THP_NTP_RESYNC_EVERY_CYCLES` 才真同步） |
| 4 上报 | 之后 | LOCAL 组帧 → 各 BLE 最佳帧 → 补传 |
| 5 睡眠 | — | 睡到下一 T′−5s；超时则立刻开下一窗 |

周期开始 `thp_ble_scan_window_realign(cycle_ref_ms)`，取帧与时间戳共用同一 ref。
deadline = `period − THP_CYCLE_DEADLINE_MARGIN_MS`（默认 20s）；耗尽不再 HTTP，读数仍入队；
补传还要求剩余 ≥ 10s。

**设计意图**：C3 单射频，HTTP `perform` 会 `stop_scan`。若 LOCAL 在窗内带重试上报，TLS 超时×4 可吃掉整个扫描窗（现场 `win_n=0`）。故采样与上报解耦。

## 时间戳（采样 mono 回填）

| 场景 | `measured_at` | `ts` |
|------|---------------|------|
| 实时上报 | 采样时刻 UTC | **不发送** |
| 离线补传 | 采样时刻 UTC | 采样时刻 UTC（服务端按 `ts` 落点） |

- LOCAL：`sample_mono_ms` = T 时刻 `esp_timer_get_time()/1000`
- BLE：`mi.ts_ms` = **帧接收** mono，不是组帧/HTTP 时刻
- `thp_time_fill_sample_iso()`：NTP 已同步时用「当前墙钟 − mono 偏移」回填；偏移 <0 或 >3h 退回「现在」
- NTP 未就绪：`has_iso=false`，实时可退回组帧时刻；补传无 `ts` 则服务端按入库时间

## 构建烧录

```powershell
$env:PYTHONUTF8=1
D:\esp\v6.1\esp-idf\export.ps1
cd G:\esp32s3\esp32c3_sensors-THP-dash\ESP32
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

- 产物：`build/esp32c3_thp_multi.bin`、`esp32c3_thp_multi_flashed.bin`
- 目标：`esp32c3`，Flash 4MB，factory 2MB（`partitions.csv`）
- BLE：NimBLE（Bluedroid 关），Wi-Fi+BLE 共存，仅扫描不连接
- `COMx` 用 `idf.py list-ports` 查

## 上报协议

```http
POST {THP_API_BASE}/api/v1/readings
Authorization: Bearer <source_token>
Content-Type: application/json

{ "temperature": 23.40, "humidity": 48.20, "pressure": 1013.20,
  "measured_at": "2026-01-01T12:00:00.000Z", "rssi": -55 }
```

| 项 | 行为 |
| --- | --- |
| 部分字段 | 允许仅 T+H 或仅 P；缺省字段 JSON **整体省略**；T/H 不得只报一个 |
| 范围 | T −40~85，H 0~100，P 300~1200；超范围字段不写入 |
| `device_id` | 可选发送；须与 Token 绑定一致 |
| `measured_at` / `ts` | 见上文「时间戳」 |
| 失败重试 | 网络/5xx：有限指数退避；**401/403/400 不重试** |
| Token 401/403 | 清**该源**积压并停用该源（需更新 Token 重编译） |

### 不变量（必须保持）

1. 温湿度仅 SHT40 / BLE 计；气压仅 BMP280；BMP 温度不进业务字段
2. 部分上报：缺字段 JSON 整体省略；T/H 不得只报一个；metrics 全空则丢弃（禁止 `{,"rssi"…}`）
3. 范围校验：T −40~85，H 0~100，P 300~1200
4. 实时不带 `ts`；`measured_at` / 补传 `ts` 均为**采样时刻** UTC
5. 401/403/400 不重试；网络/5xx 有限指数退避
6. HTTP perform 期间停 BLE 扫描；**扫描窗内不发起 HTTP/NTP**
7. Token 401/403：仅清该源积压并停用该源
8. 离线队列仅 RAM、条目带 `source_id`；`peek→网络→成功再 pop` 单任务串行

## 串口要点

| 日志 | 含义 |
| --- | --- |
| `THP 多设备固件启动` | boot 完成 |
| `源表` / `已启动调度 thp_cycle（local + BLE multi）` | 多源注册完成 |
| `采样[local] ...（HTTP 留到 BLE 窗结束后）` | LOCAL 采样与上报解耦 |
| `BLE[i] 窗口样本 ...` | 第 i 台设备取到帧 |
| `上报[客厅] HTTP 201` | 按源名称/Token 上报成功 |
| `Token 失效 src=...，清除积压并停用该源` | 清该源积压并停用 |
| `上报前 NTP 同步 OK / 超时...沿用已有系统时间` | 校时结果 |
| `部分采样` / `已入离线队列` | 部分字段 / 入队 |
| `SHT40/BMP280 本周期读全失败，摘除句柄待热修复` | I2C 失败摘挂载 |

## 云端前置

1. 根目录启动 Worker：`npm run db:local` + `npm run dev`（或已 deploy）
2. Dash：**每个源** 新建设备 → 生成上报 Token
3. Token 填入 `THP_LOCAL_TOKEN` / `THP_BLE_DEVICES[].token`
4. 本地联调时 `THP_API_BASE` 用电脑局域网 IP
5. 烧录后串口应出现 `上报[...] HTTP 201`

也可用 `python tools/submit_readings.py --token thp_xxx` 先验证云端链路。

## HTTPS 连不上排查

现象类似 `esp-tls: Failed to open new connection in specified timeout` / `ESP_ERR_HTTP_CONNECT`：

| 步骤 | 动作 |
| --- | --- |
| 1 | `THP_API_BASE` 须为 `https://域名`（不要 `127.0.0.1`） |
| 2 | 默认 `CONFIG_LWIP_IPV6=n`：规避 Cloudflare AAAA 在 IPv6 不通时连挂 |
| 3 | 预检 `DNS -> IPv4` + `TCP connect OK` 仍失败 → TLS/证书；DNS/TCP 失败 → 路由/运营商 |
| 4 | `No matching trusted root certificate found` + `-0x3000` → `THP_TLS_TRUST` 改 `1`（PINNED） |
| 5 | `THP_HTTP_TIMEOUT_MS` 可调到 30000~60000（CMCC + Cloudflare 偶尔极慢） |
| 6 | `heap_free` < 70KB：TLS 可能起不来；确认未再链 OLED/IMU 等大组件 |
| 7 | 仍不行：临时改 `http://<电脑局域网IP>:8787` 验证上报链路 |
| 8 | 极端：路由器/运营商对 Cloudflare 不友好，改 `*.workers.dev` 或换网络 |

## 限制

1. 离线队列仅 RAM，断电丢
2. Token / Wi-Fi / 设备表 / TLS 模式变更需重编译烧录（未做运行时配网/OTA）
3. BLE 设备数建议 ≤4，硬上限 8
4. 加密 beacon 必须填对 BindKey；明文可留空
5. `THP_TLS_TRUST_NONE` 勿用于生产
