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
| `src/lib/schema.js` | Worker 内幂等 DDL（缺表时自动建表） |

## 功能对照

- **设备上报** `POST /api/v1/readings`：Bearer Token；字段 temperature / humidity / pressure（+可选 measured_at、rssi）
- **人侧 API**：登录后会话 Cookie；**全部业务接口鉴权**
- **自建登录**：用户名密码（PBKDF2），会话 14 天滑动；**不用 Cloudflare Access**
- **首次引导**：`GET/POST /api/auth/bootstrap` 创建第一个 admin
- **Token**：Dash 内生成/吊销；明文仅一次；库中只存哈希；**已吊销可再删除记录**
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

### 4. 部署到 Cloudflare

```powershell
npm run deploy
npm run db:remote
```

部署后首次访问站点创建 admin，再按「本地调试」中的步骤配置设备与 Token。

### 5. 设备上报（概念）

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
| POST | `/api/auth/migrate` | 无（幂等建表，本地调试用） |
| POST | `/api/auth/login` | 无 |
| POST | `/api/auth/logout` | 会话 + CSRF |
| GET | `/api/auth/me` | 会话 |
| GET/POST | `/api/v1/devices` | 会话（POST 需 admin） |
| DELETE | `/api/v1/devices/:id` | 会话 + admin |
| GET/POST | `/api/v1/tokens` | 会话（POST 需 admin） |
| DELETE | `/api/v1/tokens/:id` | 会话 + admin（吊销） |
| DELETE | `/api/v1/tokens/:id?purge=1` | 会话 + admin（硬删除已吊销记录） |
| POST | `/api/v1/readings` | **设备 Token** |
| GET | `/api/v1/readings?device_id&from&to` | 会话 |
| GET | `/api/v1/latest?device_id=` | 会话 |
| GET | `/api/v1/export?device_id&from&to` | 会话 |

写操作（登录后的 POST/DELETE）需请求头 `X-CSRF-Token`（Dash 自动从 Cookie `thp_csrf` 带上）。

---

## 本地调试（Windows / wrangler dev）

本地开发**不需要** Cloudflare 账号、真实 `database_id`，也不连线上 D1。

### 原理

| 项目 | 本地行为 |
|------|----------|
| Worker | `wrangler dev` 在本机跑 Miniflare/workerd |
| D1 | 项目下 SQLite 文件：`.wrangler/state/v3/d1/miniflare-D1DatabaseObject/...` |
| 静态 Dash | `public/` 由 Worker Assets 提供，同源访问 `/api/*` |
| `wrangler.jsonc` 中的 `database_id` | **本地可保持占位符**；仅 `deploy` / `--remote` 需要真实 id |

本地 D1 与线上完全隔离：删掉 `.wrangler/` 等于清空本地库，不会影响线上。

### 标准流程（从零到能看图）

在项目根目录 `G:\esp32s3\esp32c3_sensors-THP-dash`：

```powershell
# 0) 进入 Node 环境（本仓库自带；若 PATH 已有 node/npm 可跳过）
. .\node_env.ps1

# 1) 安装依赖（首次）
npm install

# 2) 对【本地】D1 建表（必须；否则 no such table: users）
npm run db:local

# 3) 启动本地服务
npm run dev
# 等价：npx wrangler dev --local
# 指定端口：npx wrangler dev --local --ip 127.0.0.1 --port 8787
```

终端出现 `Ready on http://127.0.0.1:8787` 后，用浏览器打开该地址：

1. 首次进入 **初始化** 页 → 创建 admin 用户名/密码（≥8 位）  
2. 自动或手动 **登录** 进入 Dash  
3. **管理** → **新建设备**（如 `dev_lab1` + 名称）  
4. **生成上报 Token** → 明文**只显示一次**，先保存  
5. 选择设备与时间范围，查看综合/分项曲线、当前值  
6. **导出 CSV**、吊销 Token 等均可本地联调  

### npm 脚本

| 脚本 | 命令展开 | 用途 |
|------|----------|------|
| `npm run db:local` | `wrangler d1 execute thp-dash --file=./schema.sql --local` | 本地建表 |
| `npm run db:remote` | 同上 `--remote` | 线上建表 |
| `npm run dev` | `wrangler dev` | 本地开发服务 |
| `npm run deploy` | `wrangler deploy` | 部署 Worker |
| `npm run check` | `node --check src/...` | 源码语法检查 |

### 本地 HTTP 探测（PowerShell）

服务启动后另开一个终端：

```powershell
# 存活
Invoke-RestMethod http://127.0.0.1:8787/api/health

# 是否需要创建第一个用户（false=已有用户）
Invoke-RestMethod http://127.0.0.1:8787/api/auth/bootstrap

# Dash 是否可访问
(Invoke-WebRequest http://127.0.0.1:8787/ -UseBasicParsing).StatusCode
```

**手动建表（不依赖页面）：**

```powershell
Invoke-RestMethod -Method POST http://127.0.0.1:8787/api/auth/migrate
```

**创建 admin（引导接口，仅库内无用户时可用）：**

```powershell
$body = @{ username = 'admin'; password = 'change-me-now' } | ConvertTo-Json
Invoke-RestMethod -Method POST http://127.0.0.1:8787/api/auth/bootstrap -ContentType 'application/json' -Body $body
```

**登录并保存 Cookie（后续带 Cookie + CSRF 调业务 API）：**

```powershell
$session = New-Object Microsoft.PowerShell.Commands.WebRequestSession
$login = Invoke-RestMethod -Method POST http://127.0.0.1:8787/api/auth/login `
  -ContentType 'application/json' -WebSession $session `
  -Body (@{ username='admin'; password='change-me-now' } | ConvertTo-Json)
$csrf = $login.csrf
Invoke-RestMethod http://127.0.0.1:8787/api/auth/me -WebSession $session
Invoke-RestMethod http://127.0.0.1:8787/api/v1/devices -WebSession $session
```

**模拟设备上报（需先在 Dash 生成 Token）：**

```powershell
$token = 'thp_粘贴你生成的明文'
Invoke-RestMethod -Method POST http://127.0.0.1:8787/api/v1/readings `
  -Headers @{ Authorization = "Bearer $token" } `
  -ContentType 'application/json' `
  -Body '{"temperature":23.5,"humidity":48.2,"pressure":1013.2}'
```

**查询曲线 / 导出 CSV：**

```powershell
# 需已登录（-WebSession $session）并带上 $csrf 时写操作才需要
Invoke-RestMethod "http://127.0.0.1:8787/api/v1/readings?device_id=dev_lab1" -WebSession $session
Invoke-WebRequest "http://127.0.0.1:8787/api/v1/export?device_id=dev_lab1" -WebSession $session -OutFile thp_export.csv
```

### 本地 SQL（wrangler d1 --local）

```powershell
# 列出本地表
npx wrangler d1 execute thp-dash --local --command "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"

# 设备与 Token
npx wrangler d1 execute thp-dash --local --command "SELECT id,name,status,last_seen_at FROM devices"
npx wrangler d1 execute thp-dash --local --command "SELECT id,device_id,revoked_at FROM api_tokens"

# 最近读数
npx wrangler d1 execute thp-dash --local --command "SELECT ts,temperature,humidity,pressure FROM readings ORDER BY ts DESC LIMIT 10"

# 用户数 / 会话数
npx wrangler d1 execute thp-dash --local --command "SELECT (SELECT COUNT(*) FROM users) AS users, (SELECT COUNT(*) FROM sessions) AS sessions"
```

### 演示模式（不启 Worker）

只想看 UI、不连 API 时：

1. 用浏览器直接打开 `public/index.html`，或  
2. 在已部署/本地站点 URL 后加 `?demo=1`  

登录页出现后点击 **演示模式**：使用 `public/js/demo.js` 的本地模拟数据（设备、约 24h 曲线、假 Token、CSV 下载）。  
**不会**写入 D1，也不能验证真实鉴权/上报。

### 常见问题

| 现象 | 原因 | 处理 |
|------|------|------|
| `no such table: users` | 本地 D1 未建表 | `npm run db:local`；或 `POST /api/auth/migrate`；bootstrap 路径也会自动补建 |
| `Binding DB not found` / 配置错误 | `wrangler.jsonc` 未正确加载或未在项目根执行 | 在项目根运行 `npm run dev`；确认存在 `d1_databases` 且 `binding: "DB"` |
| 端口被占用 | 8787 已被占用 | `npx wrangler dev --local --port 8788` |
| 登录 401「用户名或密码错误」 | 未引导创建用户，或密码不对 | `GET /api/auth/bootstrap` 看 `needsBootstrap`；必要时清本地库后重建 admin |
| 页面能开但接口 401 | 未登录或会话过期 | 重新登录；写操作需 Dash 自动带的 `X-CSRF-Token` |
| 上报 401 | Token 错误/已吊销 | 在 Dash 重新生成 Token |
| 上报 400 | 字段超范围 | 温度 −40–85°C，湿度 0–100%，气压 300–1200 hPa |
| 改了 `schema.sql` 本地表没变 | DDL 不会自动重放 | 手动再执行 `npm run db:local`，或删 `.wrangler` 后重建（本地数据会清空） |
| 想重置本地数据 | — | 停止 `wrangler dev` → 删除 `.wrangler/` → `npm run db:local` → 重新引导 admin |
| 线上 deploy 报 D1 id 无效 | 仍是占位符 | `wrangler d1 create thp-dash` 后把真实 id 写入 `wrangler.jsonc`，再 `db:remote` |

### 本地调试检查清单

- [ ] `npm run db:local` 成功  
- [ ] `npm run dev` 显示 Ready  
- [ ] `/api/health` 返回 `ok: true`  
- [ ] `/api/auth/bootstrap` 返回 JSON（而不是 500）  
- [ ] 浏览器可创建 admin 并登录  
- [ ] 能新建设备、生成 Token  
- [ ] 用 Token `POST /api/v1/readings` 返回 201  
- [ ] Dash 出现当前值与曲线  
- [ ] CSV 导出文件含表头 `timestamp,temperature,humidity,pressure`  

---

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
