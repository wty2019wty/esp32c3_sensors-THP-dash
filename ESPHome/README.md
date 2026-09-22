# 不好用！！！

# ESPHome 环境与 THP 上报配置

本目录用 **ESPHome** 读取 SHT40 / BMP280 与小米计 BLE（MI），按与 `ESP32/` 固件相同的协议上报到 Cloudflare Worker（`POST /api/v1/readings`）。

## 1. 环境

```powershell
python -m venv esphome-env

## Linux/macOS: source esphome-env/bin/activate
## Windows:     esphome-env\Scripts\activate


# 系统环境变量
ESPHOME_DATA_DIR = G:\esp32s3\esp32c3_sensors-THP-dash\ESPHome\.esphome
PLATFORMIO_CORE_DIR = G:\esp32s3\esp32c3_sensors-THP-dash\ESPHome\.platformio

# 文件软连接
C:\Users\Administrator\AppData\Local\esphome  ==>  G:\esp32s3\esp32c3_sensors-THP-dash\ESPHome\esphome

#安装
pip install esphome -i https://pypi.tuna.tsinghua.edu.cn/simple

# 验证版本
esphome version



$env:ESPHOME_DATA_DIR = "G:\esp32s3\esp32c3_sensors-THP-dash\ESPHome\.esphome"
$env:PLATFORMIO_CORE_DIR = "G:\esp32s3\esp32c3_sensors-THP-dash\ESPHome\.platformio"

pip install esphome -i https://pypi.tuna.tsinghua.edu.cn/simple
esphome version
```

或使用本目录 `start.ps1`。

## 2. 文件

| 文件 | 说明 |
| --- | --- |
| `sensor.yaml` | 主配置：SHT40 + BMP280 + BTHome 小米计 → 双 Token 上报 |
| `secrets.yaml.example` | 密钥模板（可提交） |
| `secrets.yaml` | 本地真实密钥（**gitignore**，勿提交） |

首次：

```powershell
copy secrets.yaml.example secrets.yaml
# 填 Wi-Fi / API Token / 小米计 MAC 与 bindkey
```

Token 在 Dash「管理 → 生成上报 Token」：**LOCAL** 与 **MI 各一枚**。`Authorization` 写成 `Bearer thp_xxx`。

## 3. 常用命令

```powershell
esphome-env\Scripts\activate

# 只校验 yaml（推荐先跑）
esphome config sensor.yaml

# 完整编译
esphome compile sensor.yaml

# 编译 + USB 烧录 + 串口日志
esphome run sensor.yaml

# 仅看日志 / 清理缓存
esphome logs sensor.yaml
esphome clean sensor.yaml
```

## 4. 上报口径（与 Worker / ESP32/ 一致）

| 通道 | 传感器 | Token | 字段 |
| --- | --- | --- | --- |
| LOCAL | SHT40 温湿度 + BMP280 气压 | `thp_local_auth` | `temperature`+`humidity` 同报；`pressure` 可选；`device_id`/`measured_at`/`rssi` |
| MI | 小米计 BLE 温湿度 | `thp_mi_auth` | `temperature`+`humidity` |

- 周期默认 **5 分钟**（`substitutions.report_interval`）
- 部分上报：缺气压只报 T+H；T+H 无效则跳过
- **不做**离线 RAM 队列 / 补传 / BLE 扫描窗对齐（比 `ESP32/` 固件简化）

## 5. 小米计广播格式（重要）

ESPHome 内置组件对应关系：

| 表头固件广播 | ESPHome 组件 | 加密 |
| --- | --- | --- |
| **BTHome v2**（推荐） | `bthome_mithermometer` | `bindkey` |
| ATC1441 | `atc_mithermometer` | 无（明文） |
| pvvx Custom（0x181A） | **无内置** | AtcMiCodec 自定义 |
| 原厂 MiBeacon | `xiaomi_ble` / `xiaomi_lywsd03mmc` | `bindkey` |

本仓库 `ESP32/` 自研固件用的是 **pvvx Custom（可带 AtcMiCodec 加密）**，那是自写 `atc_ble` 组件。  
**ESPHome 版请把表头刷成 / 切换为 BTHome v2**（[pvvx ATC_MiThermometer](https://github.com/pvvx/ATC_MiThermometer) 广播格式选 BTHome），并把 `thp_mi_bindkey` 填到 `secrets.yaml`。

若表头是 ATC1441 明文，把 `sensor.yaml` 里 `bthome_mithermometer` 换成：

```yaml
  - platform: atc_mithermometer
    id: mi
    mac_address: !secret thp_mi_mac
    temperature:
      name: "MI Temperature"
      id: mi_temp
    humidity:
      name: "MI Humidity"
      id: mi_hum
    battery_level:
      name: "MI Battery"
      id: mi_batt
      entity_category: diagnostic
```

## 6. 接线与注意

| 信号 | GPIO |
| --- | --- |
| SDA | 8 |
| SCL | 9 |
| VCC | **3.3V（严禁 5V）** |
| GND | GND |

- GPIO8/9 为 strapping 脚，ESPHome 会告警，与原固件相同，可忽略
- Super Mini 天线差时可再调 `wifi.power_save_mode` / 缩短 I2C 走线
- 上线前确认 Worker 可达：`thp_readings_url` 用 `https://dash.486520.xyz/api/v1/readings` 或你的域名

## 7. 本地链路自检（不烧录）

```powershell
# 校验配置
esphome config sensor.yaml

# 可选：用仓库工具模拟上报，确认 Token/字段
cd ..
python tools/submit_readings.py --token thp_粘贴明文 --count 2 -v
```
