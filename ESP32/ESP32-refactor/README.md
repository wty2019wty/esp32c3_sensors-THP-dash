# THP 多设备固件（ESP32-refactor）

按 **多设备源表** 建模的 ESP32-C3 固件：本机 SHT40+BMP280 + N 台 ATC/BTHome BLE 温湿度计，
各自 Token 上报到 THP Dash（`POST /api/v1/readings`）。

设计说明见 [DESIGN.md](./DESIGN.md)。协议与云端与原 `../` 工程一致。

**状态**：重构完成，可直接替代原单设备固件部署。产物 `build/esp32c3_thp_multi.bin`。

## 硬件

| 器件 | 作用 | 接口 |
| --- | --- | --- |
| ESP32-C3 | MCU + Wi-Fi + BLE | — |
| SHT40 | 本机温湿度 | I2C 0x44（备用 0x45），SDA=GPIO8 SCL=GPIO9 |
| BMP280 | 本机气压 | I2C 0x76（备用 0x77） |
| ATC/BTHome ×N | 远端温湿度 | BLE 被动扫描（不连接） |

VCC **3.3V（严禁 5V）**。I2C 走线建议短、外接 4.7kΩ 上拉；GPIO8 板载 LED 可能干扰 SDA。

## 目录

```
ESP32-refactor/
├── DESIGN.md                    # 架构 / 调度 / 不变量
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
    ├── sht40/ bmp280/           # 驱动（与原工程相同语义）
    └── atc_ble/                 # 多设备 ATC/BTHome 扫描解析
```

## 配置

```powershell
cd ESP32\ESP32-refactor
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

每台 BLE 设备在 Dash **各建一个 device、各生成一个 Token**，填入表中对应 `.token`。
Token 占位符（`thp_replace_me*`）会被源表拒绝并跳过该源。

### TLS 信任怎么选

| 模式 | 何时用 |
| --- | --- |
| `0` BUNDLE | 默认。系统证书包，覆盖公有 CA |
| `1` PINNED | bundle 报 `No matching trusted root`（GTS 链，idf#18674）；信任 `thp_tls_trust.h` 内嵌根 |
| `2` NONE | **仅调试**。需在 `sdkconfig.defaults` 打开 `CONFIG_ESP_TLS_INSECURE` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` |

换域名/换 CA 时同步更新 `thp_tls_trust.h` 内嵌 PEM。

## 构建烧录

```powershell
$env:PYTHONUTF8=1
D:\esp\v6.1\esp-idf\export.ps1
cd G:\esp32s3\esp32c3_sensors-THP-dash\ESP32\ESP32-refactor
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

产物：`build/esp32c3_thp_multi.bin`、`esp32c3_thp_multi_flashed.bin`。
`COMx` 换成实际串口（`idf.py list-ports`）。

## 上报协议（不变）

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
| `measured_at` | **采样时刻** UTC（LOCAL=T 采样 mono，BLE=帧接收 mono） |
| `ts` | **实时不发送**；离线补传发送采样时刻 UTC |
| 失败重试 | 网络/5xx：有限指数退避；**401/403/400 不重试** |
| Token 401/403 | 清**该源**积压并停用该源（需更新 Token 重编译） |

## 调度摘要

```
T−5s          T              T+10s         之后
 │ 开 BLE 窗    │ 仅 I2C 采样   │ 关窗 + 校时   │ LOCAL → 各 BLE → 补传 → 睡眠
 └──────── 纯扫描，禁止 HTTP/NTP ────────┘
```

单任务 `thp_cycle`：**扫描窗内不做 HTTP/NTP**，避免 TLS 重试饿死 BLE 窗口。
deadline = period − 20s；耗尽时不再 HTTP，读数仍入队。

## 串口要点

| 日志 | 含义 |
| --- | --- |
| `THP 多设备固件启动` | boot 完成 |
| `源表` / `已启动调度 thp_cycle（local + BLE multi）` | 多源注册完成 |
| `采样[local] ...（HTTP 留到 BLE 窗结束后）` | LOCAL 采样与上报解耦 |
| `BLE[i] 窗口样本 ...` | 第 i 台设备取到帧 |
| `上报[客厅] HTTP 201` | 按源名称/Token 上报成功 |
| `Token 失效 src=...，清除积压并停用该源` | 清该源积压并停用（需更新 Token 重编译） |
| `上报前 NTP 同步 OK / 超时...沿用已有系统时间` | 校时结果 |
| `部分采样` / `已入离线队列` | 与原版同义 |
| `SHT40/BMP280 本周期读全失败，摘除句柄待热修复` | I2C 失败摘挂载，下周期 re-probe |

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
| 2 | 工程默认 `CONFIG_LWIP_IPV6=n`：规避 Cloudflare AAAA 在 IPv6 不通时连挂 |
| 3 | 看预检：`DNS -> IPv4` 成功且 `TCP connect OK` 仍失败 → TLS/证书；DNS/TCP 失败 → 路由/运营商 |
| 4 | `No matching trusted root certificate found` + `-0x3000` → 将 `THP_TLS_TRUST` 改为 `1`（PINNED） |
| 5 | `THP_HTTP_TIMEOUT_MS` 可调到 30000~60000（CMCC + Cloudflare 偶尔极慢） |
| 6 | `heap_free` < 70KB：TLS 可能起不来；确认未再链 OLED/IMU 等大组件 |
| 7 | 仍不行：临时改 `http://<电脑局域网IP>:8787` 验证上报链路 |
| 8 | 极端：路由器/运营商对 Cloudflare 不友好，改 `*.workers.dev` 或换网络 |

## 限制

1. 离线队列仅 RAM，断电丢
2. Token / Wi-Fi / 设备表 / TLS 模式变更需重编译烧录（未做运行时配网/OTA）
3. BLE 设备数建议 ≤4（每台每周期一条 HTTPS；deadline 预算有限），硬上限 8
4. 加密 beacon 必须填对 BindKey；明文可留空
5. `THP_TLS_TRUST_NONE` 勿用于生产
