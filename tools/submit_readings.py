#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
THP Dash 上报测试脚本（模拟 ESP32-C3 设备）

仅用 Python 标准库，向 POST /api/v1/readings 提交温湿度气压数据。

前置：
  1. 本地：npm run dev（http://127.0.0.1:8787）或已部署的 Worker URL
  2. Dash 登录 → 管理 → 新建设备 → 生成上报 Token（明文只显示一次）

示例：
  python tools/submit_readings.py --token thp_xxx
  python tools/submit_readings.py --url http://127.0.0.1:8787 --token thp_xxx --count 5
  python tools/submit_readings.py --token thp_xxx --backfill-hours 24 --interval-min 5
  python tools/submit_readings.py --token thp_xxx --temp 25.5 --hum 50 --pres 1013
  python tools/submit_readings.py --token thp_xxx --list-devices --session-cookie "..." --csrf "..."
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from typing import Any


def iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def request(
    base: str,
    path: str,
    method: str = "GET",
    token: str | None = None,
    body: dict[str, Any] | None = None,
    cookie: str | None = None,
    csrf: str | None = None,
) -> tuple[int, Any]:
    url = base.rstrip("/") + path
    data = None
    headers = {"Accept": "application/json", "User-Agent": "thp-submit-readings/1.0"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if cookie:
        headers["Cookie"] = cookie
    if csrf:
        headers["X-CSRF-Token"] = csrf
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json; charset=utf-8"

    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            status = resp.status
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", errors="replace")
        status = e.code
    except urllib.error.URLError as e:
        print(f"[网络错误] {method} {url}: {e.reason}", file=sys.stderr)
        raise SystemExit(2)

    parsed: Any
    try:
        parsed = json.loads(raw) if raw else None
    except json.JSONDecodeError:
        parsed = raw
    return status, parsed


def synth_reading(i: int, base_temp: float, base_hum: float, base_pres: float) -> dict[str, float]:
    """SHT40 + BMP280 风格：日周期 + 少量噪声"""
    phase = i / 12.0  # ~1h per 12 samples @5min
    temp = base_temp + 2.2 * math.sin(phase) + random.uniform(-0.15, 0.15)
    hum = base_hum + 7.0 * math.cos(phase / 1.3) + random.uniform(-0.6, 0.6)
    pres = base_pres + 3.5 * math.sin(phase / 3.0) + random.uniform(-0.25, 0.25)
    return {
        "temperature": round(temp, 2),
        "humidity": round(max(0.0, min(100.0, hum)), 2),
        "pressure": round(pres, 2),
    }


def post_one(args: argparse.Namespace, reading: dict[str, float], ts: datetime | None) -> tuple[int, Any]:
    payload: dict[str, Any] = dict(reading)
    if args.device_id:
        payload["device_id"] = args.device_id
    payload["measured_at"] = iso(ts) if ts else iso(datetime.now(timezone.utc))
    if ts is not None and args.send_ts:
        payload["ts"] = iso(ts)
    return request(args.url, "/api/v1/readings", method="POST", token=args.token, body=payload)


def run_forever(args: argparse.Namespace) -> None:
    print(f"[模式] 持续上报 → {args.url}/api/v1/readings  间隔 {args.interval}s")
    i = 0
    while True:
        reading = synth_reading(i, args.temp, args.hum, args.pres)
        status, body = post_one(args, reading, None)
        now = datetime.now(timezone.utc).strftime("%H:%M:%S")
        ok = 200 <= status < 300
        mark = "OK" if ok else "FAIL"
        print(f"[{now}] {mark} HTTP {status}  T={reading['temperature']}°C  "
              f"H={reading['humidity']}%  P={reading['pressure']}hPa  {body}")
        if not ok and args.stop_on_error:
            raise SystemExit(1)
        i += 1
        time.sleep(args.interval)


def run_batch(args: argparse.Namespace) -> None:
    count = args.count
    use_hist = args.backfill_hours > 0
    interval_min = args.interval_min
    total = count
    if use_hist:
        total = int(args.backfill_hours * 60 / interval_min) or 1
        if count > 0:
            total = count

    print(f"[模式] 批量提交 {total} 条 → {args.url}")
    print(f"       Token={args.token[:12]}..." if args.token else "       Token=(空)")
    if use_hist:
        print(f"       回填近 {args.backfill_hours}h，步长 {interval_min}min，ts={'发送' if args.send_ts else '由服务端接收时间决定'}")

    ok_n = 0
    fail_n = 0
    now = datetime.now(timezone.utc)

    for i in range(total):
        if use_hist:
            ts = now - timedelta(minutes=interval_min * (total - 1 - i))
        else:
            ts = now

        # 固定值或合成波形
        if args.temp is not None and args.hum is not None and args.pres is not None and args.fixed:
            reading = {
                "temperature": args.temp,
                "humidity": args.hum,
                "pressure": args.pres,
            }
        else:
            reading = synth_reading(
                i if not use_hist else int((ts - now).total_seconds() // 300),
                args.temp,
                args.hum,
                args.pres,
            )

        status, body = post_one(args, reading, ts if use_hist else None)
        if 200 <= status < 300:
            ok_n += 1
            if args.verbose or total <= 10 or (i + 1) % max(1, total // 10) == 0:
                print(f"  [{i+1}/{total}] HTTP {status}  {reading}  ts={iso(ts) if use_hist else 'server'}")
        else:
            fail_n += 1
            print(f"  [{i+1}/{total}] HTTP {status}  {body}", file=sys.stderr)
            if args.stop_on_error:
                raise SystemExit(1)
        if not use_hist and args.interval > 0 and i + 1 < total:
            time.sleep(args.interval)

    print(f"[完成] 成功 {ok_n}，失败 {fail_n}")
    if fail_n:
        raise SystemExit(1)


def run_admin_probe(args: argparse.Namespace) -> None:
    """可选：用会话 Cookie 查设备列表（辅助确认 device_id）"""
    if not args.cookie:
        print("需要 --session-cookie（浏览器 Cookie 头，含 thp_session=...）", file=sys.stderr)
        raise SystemExit(2)
    status, body = request(
        args.url,
        "/api/v1/devices",
        method="GET",
        cookie=args.cookie,
        csrf=args.csrf,
    )
    print(f"HTTP {status}")
    print(json.dumps(body, ensure_ascii=False, indent=2))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="THP Dash 设备上报测试（模拟 ESP32-C3 SHT40+BMP280）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--url", default="http://127.0.0.1:8787", help="Worker / wrangler dev 基地址")
    p.add_argument("--token", default="", help="设备 API Token（Dash 生成，thp_...）")
    p.add_argument("--device-id", default="", help="可选；必须与 Token 绑定设备一致，否则 403")

    p.add_argument("--count", type=int, default=1, help="无 --backfill-hours 时的上报次数")
    p.add_argument("--interval", type=float, default=0, help="多次上报间隔秒（0=不等待）")
    p.add_argument("--interval-min", type=float, default=5, help="回填采样间隔（分钟）")
    p.add_argument("--backfill-hours", type=float, default=0, help=">0 时按历史时间回填多条")
    p.add_argument("--send-ts", action="store_true", help="回填时在 body 中带上服务端可接受的 ts 字段")
    p.add_argument("--loop", action="store_true", help="持续上报（模拟在线设备）")
    p.add_argument("--loop-interval", dest="interval", type=float, help="同 --interval（loop 用）")

    p.add_argument("--temp", type=float, default=23.0, help="基准温度 °C")
    p.add_argument("--hum", type=float, default=52.0, help="基准湿度 %RH")
    p.add_argument("--pres", type=float, default=1013.0, help="基准气压 hPa")
    p.add_argument("--fixed", action="store_true", help="不合成波形，固定使用 --temp/--hum/--pres")

    p.add_argument("--list-devices", action="store_true", help="用会话查询设备列表（调试）")
    p.add_argument("--cookie", default="", help="会话 Cookie，如 thp_session=xxx; thp_csrf=yyy")
    p.add_argument("--csrf", default="", help="X-CSRF-Token（列表 GET 可省略）")

    p.add_argument("-v", "--verbose", action="store_true", help="打印每条结果")
    p.add_argument("--stop-on-error", action="store_true", help="失败即退出")
    p.add_argument("--health", action="store_true", help="先探测 /api/health")
    return p


def main(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    only_health = args.health and not args.token and not args.list_devices and not args.loop and args.backfill_hours <= 0

    if args.health:
        status, body = request(args.url, "/api/health")
        print(f"health HTTP {status}: {body}")
        if status != 200:
            raise SystemExit(1)
        if only_health:
            return

    if args.list_devices:
        run_admin_probe(args)
        return

    if not args.token:
        print("必须提供 --token（在 Dash「管理」中生成上报 Token）", file=sys.stderr)
        print("示例: python tools/submit_readings.py --token thp_abc... --count 3 -v", file=sys.stderr)
        raise SystemExit(2)

    # 合法性粗检（与 Worker 校验一致）
    if not (-40 <= args.temp <= 85):
        print(f"temperature {args.temp} 超出 -40..85", file=sys.stderr)
        raise SystemExit(2)
    if not (0 <= args.hum <= 100):
        print(f"humidity {args.hum} 超出 0..100", file=sys.stderr)
        raise SystemExit(2)
    if not (300 <= args.pres <= 1200):
        print(f"pressure {args.pres} 超出 300..1200", file=sys.stderr)
        raise SystemExit(2)

    if args.backfill_hours > 0:
        args.send_ts = True  # 回填默认带 ts，否则全部落在“现在”
        run_batch(args)
        return

    if args.loop:
        run_forever(args)
        return

    run_batch(args)


if __name__ == "__main__":
    main()
