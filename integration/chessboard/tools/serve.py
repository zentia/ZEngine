"""宣发页专用静态服务（不要用 `python -m http.server` 直接起）。

原因：Windows 上 Python 的 `mimetypes` 常把 `.mjs` 识别成 `text/plain`，
浏览器对 ES module 强制严格 MIME 检查，于是 `import './zbrd.mjs'` 会直接失败
（页面停留在"载入中"）。本脚本显式声明 `.mjs` / `.js` / `.json` / `.zbrd`。

用法（仓库根执行）：
    python integration/chessboard/tools/serve.py
    # 数据验证页  http://127.0.0.1:8080/web/
    # 沉浸式宣发页 http://127.0.0.1:8080/web/starry.html
    # 12 个画境   http://127.0.0.1:8080/web/starry.html?world=<worlds.json 里的 id>
    #              （页面右上角下拉框也可切换，视觉包在 web/worlds.json）

或在 chessboard 目录内：
    python tools/serve.py --port 8080
"""

from __future__ import annotations

import argparse
import functools
import http.server
import os
import socketserver

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MIME = {
    ".mjs": "text/javascript; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".zbrd": "application/octet-stream",
    ".css": "text/css; charset=utf-8",
    ".html": "text/html; charset=utf-8",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".png": "image/png",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, **MIME}

    def guess_type(self, path):
        extension = os.path.splitext(path)[1].lower()
        if extension in MIME:
            return MIME[extension]
        return super().guess_type(path)

    def end_headers(self):
        # 改 spec / 重新生成 ZBRD 后刷新即可看到变化，避免浏览器缓存旧几何。
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, format, *args):  # noqa: A002 - 与基类签名保持一致
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> int:
    parser = argparse.ArgumentParser(description="serve the ZEngine chessboard promo page")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="127.0.0.1")
    args = parser.parse_args()
    handler = functools.partial(Handler, directory=ROOT)
    with Server((args.bind, args.port), handler) as server:
        print("ZEngine chessboard: http://%s:%d/web/" % (args.bind, args.port))
        print("serving %s (Ctrl+C to stop)" % ROOT)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
