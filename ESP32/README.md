# ESP32-C3 固件：THP HTTPS 上报

设备端采样并上报到本仓库云端 Worker（`POST /api/v1/readings`）。  
ESP-IDF，新版 `i2c_master`

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
- **允许部分上报**：SHT40 失败时仅上报气压；BMP280 失败时仅上报温湿度。JSON **省略**缺失字段（不发 0 占位）。两者都失败才跳过本周期  

## 2. 目录

```
ESP32/
├── CMakeLists.txt
├── sdkconfig.defaults          # C3 / 4MB / TLS 证书捆绑包
├── main/
│   ├── main.c                  # 仅 boot：NVS → 传感器/Wi-Fi/队列 → 启动调度
│   ├── thp_sched.c/h           # 单周期流水线：校时→补传→LOCAL→MI→睡眠
│   ├── thp_wifi.c/h            # Wi-Fi STA（断线持续重连）
│   ├── thp_time.c/h            # SNTP + ISO-8601
│   ├── thp_sensors.c/h         # I2C + SHT40/BMP280 采样
│   ├── thp_queue.c/h           # 离线 RAM 环形队列
│   ├── thp_report.c/h          # HTTPS JSON 上报 / 重试 / 补传
│   ├── thp_mi.c/h              # 小米计 BLE 周期上报（THP_MI_ENABLE）
│   ├── thp_types.h             # 共享类型与量程校验
│   ├── i2c_config.h            # SDA/SCL/速率
│   ├── thp_tls_trust.h         # 内嵌 TLS 根证书
│   ├── thp_config.h.example    # 配置模板
│   └── thp_config.h            # 本地真实配置（gitignored，勿提交）
└── components/
    ├── sht40/                  # 温湿度驱动（CRC-8）
    ├── bmp280/                 # 气压驱动（0x76/0x77，t_fine 补偿）
    └── atc_ble/                # pvvx ATC BLE 扫描解析
```

### 调度模型

单 FreeRTOS 任务 `thp_cycle`，每 `THP_REPORT_PERIOD_MS` 跑一轮固定阶段：

1. **校时** — Wi-Fi 可用时 `esp_netif_sntp_start` + 等待同步  
2. **补传** — 最多 `THP_OFFLINE_FLUSH_MAX_PER_CYCLE` 条历史  
3. **LOCAL** — I2C 采样 → Token A 上报；失败入离线队列  
4. **MI**（若就绪）— 取本周期 BLE 最近一帧 → Token B 上报  
5. **睡眠** — 相对本周期起点补齐剩余时间，超时则立刻进入下一周期  

不再使用 LOCAL/MI 两条独立任务；TLS 与队列在单线程周期内串行，无跨任务抢锁。

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

部分上报示例（仅温湿度 / 仅气压）：

```json
{ "temperature": 23.40, "humidity": 48.20, "rssi": -55 }
{ "pressure": 1013.20, "rssi": -55 }
```

| 项 | 固件行为 |
| --- | --- |
| `device_id` | 可选发送；不一致会 403。默认省略，以 Token 绑定为准 |
| `measured_at` | 实时上报：NTP 同步成功才发送（ISO-8601 UTC）；失败则省略 |
| `ts` | **实时上报不发送**（入库时间以服务端为准）；**离线补传时发送采样时刻 UTC**，服务端按 `ts` 落点 |
| 部分字段 | 支持仅 T+H 或仅 P；缺省字段在 JSON 中整体省略；`temperature`/`humidity` 不得只报一个 |
| 字段范围 | 与服务端一致：T −40~85，H 0~100，P 300~1200；超范围字段**不写入本帧**，其余字段仍可上报 |
| 失败重试 | 网络/5xx：最多 3 次指数退避；**401/403/400 不重试** |
| 传感器热修复 | `present=false` 时上报任务仍启动，每周期尝试重新 init；成功后恢复对应字段 |
| 离线队列 | 暂态失败（含 Wi-Fi 断开）入 RAM 环形队列；恢复后**先补传再报当前**；容量 `THP_OFFLINE_QUEUE_LEN`（默认 288≈1 天），满则丢最旧；**每周期最多补 `THP_OFFLINE_FLUSH_MAX_PER_CYCLE`（默认 24）条**，避免长时间阻塞当前采样；**断电不保留**；无有效 UTC 戳的条目补传时不带 `ts` |

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
| `传感器未全部就绪...支持仅温湿度或仅气压` | 缺一路传感器仍可部分上报 |
| `部分采样: SHT40=... BMP280=... → 上报 温湿度/气压` | 本周期只带有效字段 |
| `已入离线队列 (n/N)` | 本周期上报失败，已缓存待补传 |
| `开始补传离线队列，共 n 条` | 网络恢复，按时间顺序补历史 |
| `补传成功 iso=...` | 一条历史数据已写入云端（带 `ts`） |
| `离线队列已清空` | 积压数据全部补完 |
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
2. 离线补传为 **RAM 环形队列**：断网期间会缓存并在恢复后按历史 `ts` 补传；**断电丢缓冲**。冷启动且从未 NTP 同步过的点可能无 `ts`（服务端按入库时间记）。Token 失效时会清空队列。  
3. TLS 使用系统证书捆绑包 + 内嵌 GTS 根证书校验公网 HTTPS；本地 `http://IP:8787` 无需证书。  
4. Token / API Base 更换后必须重新编译烧录（未做运行时配网/OTA）。  
5. **部分 Super Mini 天线较差**：默认把 `THP_WIFI_STA_TX_POWER_DBM` 设为 15（代码内再夹到 8~20）。若仍频繁断连，先降到 8~12，再查供电/距离/路由器信道；不要用超过 20 dBm “硬顶”。  

