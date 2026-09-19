# THP Dash（Cloudflare 服务端 + Dash）

ESP32-C3 温湿度气压监控：**CF Worker API + D1 + Pages Dash**。  
需求基线见 [REQUIREMENTS.md](./REQUIREMENTS.md)。  
本期交付 **Cloudflare 服务与 Web Dash**；ESP32 固件后续接入。

## 架构

```
ESP32-C3 (SHT40 + BMP280)
    │  HTTPS POST /api/v1/readings
    │  Authorization: Bearer <device_token>
    ▼
Cloudflare Worker ── D1 (readings 永久保留)
    ▲
Pages Dash（自建登录会话 Cookie）
    设备选择 / 当天默认·可选范围 / 综合·分项图 / CSV / Token 管理
```

## 目录

| 路径 | 说明 |
|------|------|
| `schema.sql` | D1 表结构（users/sessions/devices/api_tokens/readings） |
| `wrangler.jsonc` | Worker + D1 + 静态资源绑定 |
| `src/` | Worker 源码（鉴权、上报、查询、降采样、CSV） |
| `public/` | Dash（`index.html` + `css` + `js`），含本地演示模式 |
| `REQUIREMENTS.md` | 完整需求与已拍板决策 |

## 功能对照

- **设备上报** `POST /api/v1/readings`：Bearer Token；字段 temperature / humidity / pressure（+可选 measured_at、rssi）
- **人侧 API**：登录后会话 Cookie；**全部业务接口鉴权**
- **自建登录**：用户名密码（PBKDF2），会话 14 天滑动；**不用 Cloudflare Access**
- **首次引导**：`GET/POST /api/auth/bootstrap` 创建第一个 admin
- **Token**：Dash 内生成/吊销；明文仅一次；库中只存哈希
- **查询** `GET /api/v1/readings`：默认当天，自动降采样
- **导出** `GET /api/v1/export`：CSV 列 `timestamp,temperature,humidity,pressure`；长范围与曲线同一降采样
- **Dash**：综合三线一张 ↔ 分项三张；多设备；管理面板

## 部署（Cloudflare）

### 1. 准备

- 安装 Node.js 18+
- `npx wrangler login`（或本机已登录 Wrangler）

### 2. 创建 D1

```powershell
npx wrangler d1 create thp-dash
```

将输出的 `database_id` 填入 `wrangler.jsonc` 的 `d1_databases[0].database_id`。

### 3. 建表

```powershell
# 本地（wrangler dev）
npx wrangler d1 execute thp-dash --file=./schema.sql --local

# 线上
npx wrangler d1 execute thp-dash --file=./schema.sql --remote
```

### 4. 本地开发

```powershell
npm install
npm run dev
```

打开控制台提示的本地 URL：

1. 进入 **首次初始化**，创建 admin  
2. 登录 → **管理** → 新建设备 → **生成上报 Token**（仅显示一次）  
3. 图表、导出、Token 吊销均可在 Dash 操作  

无 Worker 时：打开 `public/index.html`，点 **演示模式**（或 URL 加 `?demo=1`）预览界面。

### 5. 部署

```powershell
npm run deploy
npx wrangler d1 execute thp-dash --file=./schema.sql --remote
```

部署后首次访问站点创建 admin，再按上节配置设备 Token。

### 6. 设备上报（概念）

```http
POST /api/v1/readings
Authorization: Bearer thp_<secret>
Content-Type: application/json

{
  "temperature": 23.4,
  "humidity": 48.2,
  "pressure": 1013.2,
  "measured_at": "2026-01-01T12:00:00.000Z"
}
```

- 温湿度口径 **SHT40**，气压 **BMP280**  
- `device_id` 以 Token 绑定为准  

## API 一览

| 方法 | 路径 | 鉴权 |
|------|------|------|
| GET | `/api/health` | 无（存活探测） |
| GET/POST | `/api/auth/bootstrap` | 无（仅库内无用户时可创建） |
| POST | `/api/auth/login` | 无 |
| POST | `/api/auth/logout` | 会话 + CSRF |
| GET | `/api/auth/me` | 会话 |
| GET/POST | `/api/v1/devices` | 会话（POST 需 admin） |
| DELETE | `/api/v1/devices/:id` | 会话 + admin |
| GET/POST | `/api/v1/tokens` | 会话（POST 需 admin） |
| DELETE | `/api/v1/tokens/:id` | 会话 + admin |
| POST | `/api/v1/readings` | **设备 Token** |
| GET | `/api/v1/readings?device_id&from&to` | 会话 |
| GET | `/api/v1/latest?device_id=` | 会话 |
| GET | `/api/v1/export?device_id&from&to` | 会话 |

写操作（登录后的 POST/DELETE）需请求头 `X-CSRF-Token`（Dash 自动从 Cookie `thp_csrf` 带上）。

## 自动降采样（v1）

| 范围 | 粒度 |
|------|------|
| ≤ 24h（含当天） | 原始 5 分钟 |
| ≤ 7 天 | 30 分钟 |
| ≤ 30 天 | 2 小时 |
| ≤ 90 天 | 6 小时 |
| ≤ 1 年 | 1 天 |
| > 1 年 | 1 周 |

原始数据 **永久** 存于 D1；降采样仅作用于读路径（曲线与 CSV 一致）。

## 安全要点

- 密码：PBKDF2-SHA256（迭代 210000）  
- 设备 Token / 会话：只存 SHA-256 哈希  
- 会话 Cookie：HttpOnly + Secure + SameSite=Lax  
- 登录失败限速（Cache API，尽力而为）  
- Token 明文仅生成响应中出现一次  

## ESP32-C3（待做）

- SHT40：温度、湿度  
- BMP280：气压  
- 每 5 分钟 HTTPS 上报 + Bearer Token  
- 参考 `REQUIREMENTS.md` §4 / §10  

## 许可证

见 `LICENSE`。
