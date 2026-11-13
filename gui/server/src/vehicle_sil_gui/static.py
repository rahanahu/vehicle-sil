"""The screen: the files of one directory only (the sample screen, or the one given with --web), with the media
type chosen here by extension."""

from __future__ import annotations

import os
from pathlib import Path

# Not the OS's registry (on Windows it can make .js text/plain). Other extensions are not served.
MEDIA_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".map": "application/json",
    ".webmanifest": "application/manifest+json",
    ".txt": "text/plain; charset=utf-8",
    ".svg": "image/svg+xml",
    ".ico": "image/vnd.microsoft.icon",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".webp": "image/webp",
    ".avif": "image/avif",
    ".ttf": "font/ttf",
    ".otf": "font/otf",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
}

RESERVED = ("api", "ws", "panels")  # never answered with index.html: a wrong path there is a 404, not the screen

NOT_BUILT = """<!doctype html>
<html lang="ja">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>画面がありません - vehicle-sil</title>
<style>body{font-family:system-ui,sans-serif;max-width:40rem;margin:2rem auto;padding:0 1rem;line-height:1.6}
code,pre{background:#8882;padding:0 .2em}pre{padding:.5em}</style></head>
<body>
<h1>画面がありません</h1>
<p>GUI サーバーは動いていますが、ビルドした画面が見つかりません。リポジトリの <code>gui/web</code> でビルドしてから、サーバーを起動し直してください（Node.js が要ります）。</p>
<pre>cd gui/web
npm ci
npm run build</pre>
<p>The GUI server is running, but the built screen was not found. Build it in <code>gui/web</code> as above
(<code>npm ci</code>, <code>npm run build</code>), then start the server again.</p>
</body></html>
"""


def find_web_dir() -> Path | None:
    """The screen packaged with this module (vehicle_sil_gui/web), else the build of the repository
    (gui/web/dist/browser), when this module runs from the repository's source tree."""
    here = Path(__file__).resolve().parent
    for candidate in (here / "web", here.parents[2] / "web" / "dist" / "browser"):
        if (candidate / "index.html").is_file():
            return candidate
    return None


def check_web_dir(directory: Path) -> str | None:
    """Why a directory given with --web cannot be served, or None."""
    if not directory.is_dir():
        return f"--web {directory}: not a directory (give the directory of your built screen, with its index.html)"
    index = directory / "index.html"
    if index.is_symlink() or not index.is_file():
        return (f"--web {directory}: there is no index.html in it (a file, not a link): give the directory that "
                "your build writes, such as dist/")
    return None


class StaticFiles:
    def __init__(self, directory: Path | None):
        self.directory = directory.resolve() if directory else None

    def lookup(self, url_path: str) -> tuple[Path | None, str | None, int]:
        """(file, media type, status) for a request path (already percent-decoded, starting with '/').
        status 200 with file None means: answer with the not-built page."""
        relative = url_path.lstrip("/")
        parts = relative.split("/") if relative else []
        if parts and parts[0] in RESERVED:
            return None, None, 404
        if any(p in ("..", ".") or p.startswith(".") or "\\" in p or "\x00" in p or ":" in p for p in parts):
            return None, None, 404
        if self.directory is None:
            return None, None, 200
        name = parts[-1] if parts else ""
        has_extension = "." in name
        if not parts or not has_extension:
            candidate = self.directory / Path(*parts) if parts else self.directory
            if not (candidate.is_file() and candidate.suffix.lower() in MEDIA_TYPES):
                return self._file(self.directory / "index.html")  # the screen's own routes
        suffix = Path(name).suffix.lower()
        if suffix not in MEDIA_TYPES:
            return None, None, 404
        return self._file(self.directory / Path(*parts))

    def _file(self, path: Path) -> tuple[Path | None, str | None, int]:
        try:
            resolved = path.resolve(strict=True)
        except (OSError, RuntimeError):
            return None, None, 404
        # The file itself, by the asked name: not through a link (a symbolic link, a junction) even to a file of
        # this directory, nor through another name of it (a Windows short name, a trailing dot).
        if os.path.normcase(resolved) != os.path.normcase(path) or not resolved.is_file():
            return None, None, 404
        media = MEDIA_TYPES.get(resolved.suffix.lower())
        if media is None:
            return None, None, 404
        return resolved, media, 200
