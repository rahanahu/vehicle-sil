# /// script
# requires-python = ">=3.11"
# dependencies = ["aiohttp>=3.14,<4"]
# ///
"""画面の開発と試験のための代役のサーバー。本物の GUI サーバーではない。

本物と同じ形の入り口だけを持つ。

- GET  <mount>api/health  状態を JSON で返す
- GET  <mount>ws          WebSocket。決まった間隔で数値を JSON で流す
- GET  <mount>...         --static を渡したとき、ビルド済みの画面を配信する。
                          ファイルがなく、拡張子もないパスには index.html を返す（SPA の戻し先）。
                          api/ と ws の下は戻し先にしない（404）。
                          画面には厳しい CSP（Content-Security-Policy）を付ける（inline の script を禁じる）

127.0.0.1 だけで待つ。Host、Origin、トークンの検査はしない（本物のサーバーではする）。
本物の GUI サーバーができるまで、e2e の試験で画面を配信するのに使う。

    uv run gui/web/dev/stub_server.py [--port 8765] [--static gui/web/dist/browser] [--mount /]
"""

from __future__ import annotations

import argparse
import asyncio
import itertools
import math
import mimetypes
import sys
from pathlib import Path

from aiohttp import WSMsgType, web

HOST = "127.0.0.1"

# 本物のサーバーで付ける見込みの CSP。script は自分のファイルだけ。
# style の 'unsafe-inline' は、コンポーネントのスタイルを <style> で入れるために要る（nonce にするなら ngCspNonce）。
CSP = "; ".join(
    [
        "default-src 'self'",
        "script-src 'self'",
        "style-src 'self' 'unsafe-inline'",
        "img-src 'self' data:",
        "font-src 'self'",
        "connect-src 'self'",
        "object-src 'none'",
        "base-uri 'self'",
        "frame-ancestors 'none'",
    ]
)

# OS の登録に頼らない（Windows ではレジストリの設定で .js が text/plain になることがある）。
for _type, _ext in (
    ("text/javascript", ".js"),
    ("text/javascript", ".mjs"),
    ("text/css", ".css"),
    ("text/html", ".html"),
    ("application/json", ".json"),
    ("image/svg+xml", ".svg"),
    ("image/vnd.microsoft.icon", ".ico"),
    ("font/woff2", ".woff2"),
):
    mimetypes.add_type(_type, _ext)


def log(message: str) -> None:
    print(f"stub_server: {message}", file=sys.stderr, flush=True)


async def health(_request: web.Request) -> web.Response:
    return web.json_response({"status": "ok", "server": "stub"})


def make_ws_handler(interval: float):
    async def ws(request: web.Request) -> web.WebSocketResponse:
        # 転送の経路（開発サーバーの proxy など）で Host と Origin がどう届くかを残す。
        log(f"ws open host={request.headers.get('Host')} origin={request.headers.get('Origin')}")
        socket = web.WebSocketResponse(heartbeat=10)
        await socket.prepare(request)

        async def drain() -> None:
            async for message in socket:
                if message.type == WSMsgType.ERROR:
                    break

        reader = asyncio.create_task(drain())
        try:
            for seq in itertools.count(1):
                if socket.closed:
                    break
                t = seq * interval
                await socket.send_json(
                    {
                        "seq": seq,
                        "values": {
                            "speed": round(60 + 20 * math.sin(t / 5), 3),
                            "rpm": round(1500 + 500 * math.sin(t / 3), 1),
                            "coolant": round(80 + 5 * math.sin(t / 30), 2),
                        },
                    }
                )
                await asyncio.sleep(interval)
        except (ConnectionResetError, RuntimeError):
            pass
        finally:
            reader.cancel()
        log("ws closed")
        return socket

    return ws


def make_static_handler(root: Path):
    root = root.resolve()
    index = root / "index.html"
    headers = {"Content-Security-Policy": CSP}

    async def static(request: web.Request) -> web.StreamResponse:
        tail = request.match_info["tail"]
        candidate = (root / tail).resolve()
        if candidate.is_relative_to(root) and candidate.is_file():
            return web.FileResponse(candidate, headers=headers)
        if tail == "ws" or tail.startswith(("api/", "ws/")):
            raise web.HTTPNotFound()  # API の打ち間違いに画面を返さない
        if "." in tail.rsplit("/", 1)[-1]:
            raise web.HTTPNotFound()  # ない静的ファイルは 404（HTML を返さない）
        return web.FileResponse(index, headers={**headers, "Cache-Control": "no-cache"})

    return static


def build_app(args: argparse.Namespace) -> web.Application:
    mount = args.mount
    app = web.Application()
    app.router.add_get(f"{mount}api/health", health)
    app.router.add_get(f"{mount}ws", make_ws_handler(args.interval))
    if args.static is not None:
        if not (args.static / "index.html").is_file():
            raise SystemExit(f"stub_server: {args.static}/index.html がない。先に npm run build を実行する")
        app.router.add_get(mount + "{tail:.*}", make_static_handler(args.static))
    return app


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="画面の開発と試験のための代役のサーバー（本物の GUI サーバーではない）")
    parser.add_argument("--port", type=int, default=8765, help="待つポート（既定 8765）")
    parser.add_argument("--static", type=Path, help="ビルド済みの画面のディレクトリ（例: gui/web/dist/browser）")
    parser.add_argument("--mount", default="/", help="入り口を置くパス（既定 /。例: /gui/）")
    parser.add_argument("--interval", type=float, default=1.0, help="WebSocket で数値を流す間隔（秒）")
    args = parser.parse_args(argv)
    if not (args.mount.startswith("/") and args.mount.endswith("/")):
        parser.error("--mount は / で始まり / で終わる")
    return args


def main() -> None:
    args = parse_args()
    log(f"listening on http://{HOST}:{args.port}{args.mount}")
    web.run_app(build_app(args), host=HOST, port=args.port, print=None)


if __name__ == "__main__":
    main()
