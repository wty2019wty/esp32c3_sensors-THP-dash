#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
本地联调：bootstrap → login → 建设备 → 生成 Token → 上报 N 条 → 拉 latest/readings。

不依赖浏览器，适合刚 wrangler dev 起来时的冒烟测试。

  python tools/local_e2e.py
  python tools/local_e2e.py --url http://127.0.0.1:8787 --count 8
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from typing import Any

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from submit_readings import synth_reading, iso  # type: ignore  # noqa: E402


def call(
    base: str,
    path: str,
    method: str = "GET",
    body: dict | None = None,
    cookies: dict[str, str] | None = None,
    csrf: str | None = None,
    token: str | None = None,
) -> tuple[int, Any, dict[str, str]]:
    url = base.rstrip("/") + path
    headers = {"Accept": "application/json", "User-Agent": "thp-local-e2e/1.0"}
    data = None
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if cookies:
        headers["Cookie"] = "; ".join(f"{k}={v}" for k, v in cookies.items())
    if csrf:
        headers["X-CSRF-Token"] = csrf
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            status = resp.status
            set_cookie = resp.headers.get_all("Set-Cookie") or []
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", errors="replace")
        status = e.code
        set_cookie = e.headers.get_all("Set-Cookie") if e.headers else []
    except urllib.error.URLError as e:
        print(f"网络错误 {url}: {e.reason}", file=sys.stderr)
        raise SystemExit(2)

    parsed = None
    if raw:
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = raw
    return status, parsed, parse_set_cookie(set_cookie or [])


def parse_set_cookie(items: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in items:
        first = item.split(";")[0]
        if "=" in first:
            k, v = first.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description="THP Dash 本地端到端冒烟")
    ap.add_argument("--url", default="http://127.0.0.1:8787")
    ap.add_argument("--username", default="e2e_admin")
    ap.add_argument("--password", default="e2e-pass-12345")
    ap.add_argument("--count", type=int, default=5)
    args = ap.parse_args()
    base = args.url

    print(f"== THP local e2e → {base} ==")

    st, health, _ = call(base, "/api/health")
    print(f"health: {st} {health}")
    if st != 200:
        raise SystemExit("Worker 未就绪，请先 npm run dev")

    st, bs, _ = call(base, "/api/auth/bootstrap")
    print(f"bootstrap status: {st} {bs}")
    cookies: dict[str, str] = {}
    csrf = ""

    if isinstance(bs, dict) and bs.get("needsBootstrap"):
        st, body, _ = call(
            base,
            "/api/auth/bootstrap",
            method="POST",
            body={"username": args.username, "password": args.password},
        )
        print(f"bootstrap create: {st} {body}")
        if st >= 400 and "已初始化" not in str(body):
            raise SystemExit(f"引导失败: {body}")

    st, login, ck = call(
        base,
        "/api/auth/login",
        method="POST",
        body={"username": args.username, "password": args.password},
    )
    print(f"login: {st} user={login.get('user') if isinstance(login, dict) else login}")
    if st != 200 or not isinstance(login, dict):
        print(
            "提示：本地 D1 已有其它 admin。\n"
            "  用 Dash 已创建的账号：python tools/local_e2e.py --username <你设的用户名> --password <密码>\n"
            "  或重置本地库：停止 dev → 删除 .wrangler/ → npm run db:local → 再跑本脚本",
            file=sys.stderr,
        )
        raise SystemExit(f"登录失败: {login}")
    cookies.update(ck)
    csrf = login.get("csrf") or cookies.get("thp_csrf") or ""

    dev_id = f"dev_e2e_{int(datetime.now().timestamp()) % 100000}"
    st, dev, ck = call(
        base,
        "/api/v1/devices",
        method="POST",
        body={"id": dev_id, "name": "E2E 测试设备"},
        cookies=cookies,
        csrf=csrf,
    )
    cookies.update(ck)
    print(f"create device: {st} {dev}")
    if st >= 400 and "已存在" not in str(dev):
        raise SystemExit(f"建设备失败: {dev}")

    st, tok, ck = call(
        base,
        "/api/v1/tokens",
        method="POST",
        body={"deviceId": dev_id, "name": "e2e"},
        cookies=cookies,
        csrf=csrf,
    )
    cookies.update(ck)
    print(f"create token: {st} id={tok.get('token', {}).get('id') if isinstance(tok, dict) else tok}")
    if not isinstance(tok, dict) or not tok.get("secret"):
        raise SystemExit(f"生成 Token 失败: {tok}")
    secret = tok["secret"]
    print(f"  secret(仅此次): {secret[:16]}...")

    ok = 0
    for i in range(args.count):
        r = synth_reading(i, 23.0, 52.0, 1013.0)
        payload = {**r, "device_id": dev_id, "measured_at": iso(datetime.now(timezone.utc))}
        st, body, _ = call(base, "/api/v1/readings", method="POST", body=payload, token=secret)
        print(f"  reading[{i}]: {st} {r} {body if st >= 400 else ''}")
        if 200 <= st < 300:
            ok += 1
    print(f"readings ok: {ok}/{args.count}")
    if ok == 0:
        raise SystemExit("上报全部失败")

    st, latest, _ = call(base, f"/api/v1/latest?device_id={dev_id}", cookies=cookies)
    print(f"latest: {st} {json.dumps(latest, ensure_ascii=False)[:300]}")

    st, series, _ = call(base, f"/api/v1/readings?device_id={dev_id}", cookies=cookies)
    pts = series.get("pointCount") if isinstance(series, dict) else None
    print(f"series: {st} points={pts} granularity={series.get('granularity') if isinstance(series, dict) else None}")

    print("== E2E 通过 ==")


if __name__ == "__main__":
    main()
