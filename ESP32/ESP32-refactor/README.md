# THP 多设备固件（ESP32-refactor）

按 **多设备源表** 建模的 ESP32-C3 固件：本机 SHT40+BMP280 + N 台 ATC/BTHome BLE 温湿度计，
各自 Token 上报到 THP Dash（`POST /api/v1/readings`）。

设计说明见 [DESIGN.md](./DESIGN.md)。协议与云端与原 `../` 工程一致。

## 硬件

| 器件 | 作用 | 接口 |
| --- | --- | --- |
| ESP32-C3 | MCU + Wi-Fi + BLE | — |
| SHT40 | 本机温湿度 | I2C 0x44（备用 0x45），SDA=GPIO8 SCL=GPIO9 |
| BMP280 | 本机气压 | I2C 0x76（备用 0x77） |
| ATC/BTHome ×N | 远端温湿度 | BLE 被动扫描（不连接） |

VCC **3.3V（严禁 5V）**。

## 目录

```
ESP32-refactor/
├── DESIGN.md
├── CMakeLists.txt / sdkconfig.defaults / partitions.csv
├── main/
│   ├── main.c                 # boot
│   ├── thp_sources.c/h        # 多源注册表（LOCAL + BLE[]）
│   ├── thp_types.h            # reading / sample / 结果码
│   ├── thp_config.h.example   # 配置模板（复制为 thp_config.h）
│   ├── thp_sched.c/h          # 周期流水线
│   ├── thp_local.c/h          # I2C 本机采样（SHT40/BMP280）
│   ├── thp_ble.c/h            # 多设备 BLE 网关编排
│   ├── thp_wifi.c/h / thp_time.c/h / thp_queue.c/h / thp_report.c/h
│   └── thp_tls_trust.h / i2c_config.h
└── components/
    ├── sht40/ bmp280/         # 驱动（与原工程相同）
    └── atc_ble/               # 多设备 ATC/BTHome 扫描解析
```

## 配置

```powershell
cd ESP32\ESP32-refactor
copy main\thp_config.h.example main\thp_config.h
```

编辑 `main/thp_config.h`：

| 宏 | 说明 |
| --- | --- |
| `THP_WIFI_SSID` / `THP_WIFI_PASSWORD` | STA |
| `THP_WIFI_STA_TX_POWER_DBM` | 默认 15（8~20） |
| `THP_API_BASE` | Worker 根地址，不含 `/api/...` |
| `THP_LOCAL_TOKEN` | 本机 I2C 源 Token |
| `THP_BLE_DEVICES` | BLE 设备表：`name` / `mac` / `bindkey` / `token` / `device_id` |
| `THP_REPORT_PERIOD_MS` | 默认 5 分钟 |
| 其余 NTP / 离线队列 / HTTP | 与原工程同名同义 |

每台 BLE 设备在 Dash **各建一个 device、各生成一个 Token**，填入表中对应 `.token`。

## 构建烧录

```powershell
$env:PYTHONUTF8=1
D:\esp\v6.1\esp-idf\export.ps1
cd G:\esp32s3\esp32c3_sensors-THP-dash\ESP32\ESP32-refactor
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

## 上报协议（不变）

```http
POST {THP_API_BASE}/api/v1/readings
Authorization: Bearer <source_token>
Content-Type: application/json

{ "temperature": 23.40, "humidity": 48.20, "pressure": 1013.20,
  "measured_at": "2026-01-01T12:00:00.000Z", "rssi": -55 }
```

- 部分字段可省略（仅 T+H 或仅 P）
- 实时不带 `ts`；离线补传带 `ts`
- 范围：T −40~85，H 0~100，P 300~1200

## 调度摘要

单任务 `thp_cycle`：T−5s 开 BLE 窗（一次扫描收所有设备）→ T 校时+LOCAL 上报 → T+10s
关窗后逐 BLE 设备上报 → 补传 → 睡到下窗。当前周期新鲜数据优先。

## 串口要点

| 日志 | 含义 |
| --- | --- |
| `源表: local + N BLE` | 多源注册完成 |
| `BLE[i] 窗口样本 ...` | 第 i 台设备取到帧 |
| `上报[客厅] HTTP 201` | 按源名称/Token 上报成功 |
| `Token 失效 source=...` | 仅清该源离线积压 |
| `部分采样` / `已入离线队列` | 与原版同义 |

## 限制

1. 离线队列仅 RAM，断电丢
2. Token / Wi-Fi / 设备表变更需重编译烧录
3. BLE 设备数建议 ≤4（每台每周期一条 HTTPS；deadline 预算有限）
4. 加密 beacon 必须填对 BindKey；明文可留空
