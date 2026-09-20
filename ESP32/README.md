# ESP32-C3 固件：THP HTTPS 上报

设备端采样并上报到本仓库云端 Worker（`POST /api/v1/readings`）。  
硬件与量测口径遵循根目录 [REQUIREMENTS.md](../REQUIREMENTS.md) §4 / §10；驱动风格与参考工程 `G:\esp32s3\esp32c3_sensors` 对齐（ESP-IDF，新版 `i2c_master`）。

## 1. 硬件

| 器件 | 作用 | I2C |
| --- | --- | --- |
| ESP32-C3（如 Super Mini） | MCU + Wi-Fi | — |
| SHT40 | **温度、湿度** | 0x44（备用 0x45） |
| BMP280 / GY-91 | **气压** | 0x76（备用 0x77） |

接线（与参考工程相同）：

| 信号 | GPIO |
| --- | --- |
| SDA | GPIO8 |
| SCL | GPIO9 |
| VCC | **3.3V（严禁 5V）** |
| GND | GND |

量测口径（必须遵守）：

- `temperature` / `humidity` **仅**来自 SHT40  
- `pressure` **仅**来自 BMP280（hPa）  
- BMP280 内部温度**不写入**业务字段，仅用于气压补偿  

## 2. 目录

```
ESP32/
├── CMakeLists.txt
├── sdkconfig.defaults          # C3 / 4MB / 关蓝牙 / TLS 证书捆绑包
├── main/
│   ├── main.c                  # I2C + Wi-Fi + SNTP + HTTPS 上报
│   ├── i2c_config.h            # SDA/SCL/速率
│   ├── thp_config.h.example    # 配置模板
│   └── thp_config.h            # 本地真实配置（gitignored，勿提交）
└── components/
    ├── sht40/                  # 温湿度驱动（CRC-8）
    └── bmp280/                 # 气压驱动（0x76/0x77，t_fine 补偿）
```

## 3. 配置

```powershell
cd ESP32
copy main\thp_config.h.example main\thp_config.h
```

编辑 `main/thp_config.h`：

| 宏 | 说明 |
| --- | --- |
| `THP_WIFI_SSID` / `THP_WIFI_PASSWORD` | STA Wi-Fi |
| `THP_WIFI_STA_TX_POWER_DBM` | STA 最大发射功率，**默认 15**（8~20）。部分 Super Mini 天线差，默认 20 dBm 易连不上；仍失败可降到 8~12（对齐 `esp32c3-ir-web-ESP32-C3`） |
| `THP_API_BASE` | Worker 根地址，**不含** `/api/...`。本地：`http://<电脑局域网IP>:8787`；线上：`https://xxx.workers.dev` |
| `THP_DEVICE_TOKEN` | Dash 管理页生成的上报 Token（`thp_...`，明文只显示一次） |
| `THP_DEVICE_ID` | 可选；须与 Token 绑定设备一致，一般留空 |
| `THP_REPORT_PERIOD_MS` | 默认 `5*60*1000`（5 分钟，288 条/天） |
| `THP_NTP_SERVER_LIST` | 多 NTP 源；**每次上报前** `esp_netif_sntp_start()` 重启查询并等待同步（默认阿里云 / cn.pool / 国家授时中心 / pool） |
| `THP_NTP_SYNC_TIMEOUT_MS` | 单次上报前校时超时，默认 20000ms；失败但已有可信时间则沿用 |

> 安全：`thp_config.h` 已被 `.gitignore` 忽略；**不要**把 Token 提交进仓库（REQUIREMENTS.md §12.7）。

## 4. 构建与烧录（ESP-IDF）

需已安装 ESP-IDF（参考机：`D:\esp\v6.1\esp-idf`）：

```powershell
$env:PYTHONUTF8=1
D:\esp\v6.1\esp-idf\export.ps1
cd G:\esp32s3\esp32c3_sensors-THP-dash\ESP32
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

将 `COMx` 换成实际串口（设备管理器 / `idf.py list-ports`）。

## 5. 云端前置

1. 根目录启动 Worker：`npm run db:local` + `npm run dev`（或已 `deploy`）  
2. Dash 登录 → 管理 → **新建设备** → **生成上报 Token**  
3. Token 粘贴到 `THP_DEVICE_TOKEN`；本地联调时 `THP_API_BASE` 用电脑局域网 IP（ESP32 访问不到你电脑的 `127.0.0.1`）  
4. 烧录后串口应出现 `上报 HTTP 201` 与 `ok` 响应  

也可用 `python tools/submit_readings.py --token thp_xxx` 先验证云端链路。

## 6. 上报协议（与 Worker 一致）

```http
POST {THP_API_BASE}/api/v1/readings
Authorization: Bearer <device_token>
Content-Type: application/json

{
  "temperature": 23.40,
  "humidity": 48.20,
  "pressure": 1013.20,
  "measured_at": "2026-01-01T12:00:00.000Z",
  "rssi": -55
}
```

| 项 | 固件行为 |
| --- | --- |
| `device_id` | 可选发送；不一致会 403。默认省略，以 Token 绑定为准 |
| `measured_at` | NTP 同步成功才发送（ISO-8601 UTC）；失败则省略，**入库以服务端时间为准** |
| `ts` | 生产固件不发送（避免伪造时间轴） |
| 字段范围 | 与服务端一致：T −40~85，H 0~100，P 300~1200；超范围**不发送** |
| 失败重试 | 网络/5xx：最多 3 次指数退避；**401/403/400 不重试**（避免无意义重发） |

## 7. 串口日志要点

| 日志 | 含义 |
| --- | --- |
| `Wi-Fi STA max TX power = 15 dBm` | 已按配置限制发射功率（天线差的 Super Mini 用） |
| `Wi-Fi 已连接，IP=...` | STA 就绪 |
| `NTP 服务器 4 个: [0] ntp.aliyun.com ...` | 启动时已配置多源校时 |
| `上报前 NTP 同步 OK  UTC=...` | 提交前系统时间已刷新，body 会带 `measured_at` |
| `上报前 NTP 超时...沿用已有系统时间` | 本次未校时成功，仍可能上报 |
| `探测 https://... heap_free=...` | 连通性预检开始 |
| `DNS host -> IPv4 a.b.c.d:443` | 解析成功（只查 A 记录） |
| `TCP connect ... OK` | 网络可达；若仍 HTTPS 失败则是 TLS/证书 |
| `TCP connect ... 失败 errno=...` | 路由/防火墙/域名/运营商问题 |
| `SNTP 时间已同步` | 将发送 `measured_at` |
| `SHT40/BMP280 初始化成功` | 传感器在位 |
| `上报 HTTP 201` + `ok: true` | 已写入 D1 |
| `HTTP 请求失败: ESP_ERR_HTTP_CONNECT` | TCP/TLS 未建立（见下方排查） |
| `Token 无效或已吊销` | Dash 重新生成 Token 并改配置重烧 |

### HTTPS 连不上（ESP_ERR_HTTP_CONNECT）排查

现象类似：

```text
esp-tls: Failed to open new connection in specified timeout
transport_base: Failed to open a new connection
HTTP_CLIENT: Connection failed, sock < 0
```



| 步骤 | 动作 |
| --- | --- |
| 1 | 确认 `THP_API_BASE` 是 **https://域名**（不要 `127.0.0.1`，ESP32 上那是指它自己） |
| 2 | 工程已默认 `CONFIG_LWIP_IPV6=n`：Cloudflare 有 AAAA，家用宽带 IPv6 不通时 lwIP 会连挂超时 |
| 3 | 看串口预检：`DNS -> IPv4` 成功且 `TCP connect OK` 仍失败 → TLS/证书；DNS/TCP 失败 → 路由/运营商 |
| 4 | 若日志为 `No matching trusted root certificate found` + `-0x3000`：Cloudflare 给的是 **Google Trust Services** 证书（GTS Root R4）。固件已在 `main/thp_tls_trust.h` **内嵌该根证书**（`cert_pem`），并开启 `CROSS_SIGNED_VERIFY`。换域名/换 CA 时请更新该头文件 |
| 5 | `THP_HTTP_TIMEOUT_MS` 默认 40000；CMCC + Cloudflare 偶尔极慢，可再调到 60000 |
| 6 | `heap_free` 很低（&lt;80KB）时 TLS 可能起不来：确认未再链 OLED/IMU 等大组件 |
| 7 | 仍不行：临时把 `THP_API_BASE` 改成 `http://<电脑局域网IP>:8787` 验证上报链路（本地 `npm run dev`） |
| 8 | 极端情况：路由器/运营商对 Cloudflare 不友好，可改用 `*.workers.dev` 或给设备走可出境的网络 |

## 8. 与参考工程的关系

| 项 | `esp32c3_sensors` | 本目录 `ESP32/` |
| --- | --- | --- |
| 框架 | ESP-IDF v6.x | 相同 |
| 驱动 | 自研 sht40/bmp280 + IMU/OLED | 仅 sht40/bmp280（THP 口径） |
| 网络 | 无 | Wi-Fi STA + SNTP + HTTPS Client |
| 输出 | OLED 多传感器页 | 云端 `POST /api/v1/readings` |
| 采样 | 50ms 环境 / 200Hz IMU | 5 分钟上报一帧（采样后立即 POST） |

## 9. 已知限制

1. I2C 走线建议短、外接 4.7kΩ 上拉；GPIO8 板载 LED 可能干扰 SDA。  
2. 未做离线本地缓存队列：断网期间的数据不会补传（v1 与需求一致，服务端不依赖设备队列）。  
3. TLS 使用系统证书捆绑包校验公网 HTTPS；本地 `http://IP:8787` 无需证书。  
4. Token 更换后必须重新编译烧录（未做运行时配网/OTA）。  
5. **部分 Super Mini 天线较差**：默认把 `THP_WIFI_STA_TX_POWER_DBM` 设为 15（代码内再夹到 8~20）。若仍频繁断连，先降到 8~12，再查供电/距离/路由器信道；不要用超过 20 dBm “硬顶”。
