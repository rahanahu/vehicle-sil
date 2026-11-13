"""The screen: the files of one directory only (the sample screen, or the one given with --web), with the media
type chosen here by extension."""

from __future__ import annotations

import os
import stat
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


MAX_FILE_BYTES = 64 * 1024 * 1024  # a file of a built screen; a larger one is refused (it is read into memory)


class TooLarge(Exception):
    """A file of the screen above MAX_FILE_BYTES."""


def check_web_dir(directory: Path) -> str | None:
    """Why a directory given with --web cannot be served, or None."""
    if not directory.is_dir():
        return f"--web {directory}: not a directory (give the directory of your built screen, with its index.html)"
    index = directory / "index.html"
    if index.is_symlink() or not index.is_file():
        return (f"--web {directory}: there is no index.html in it (a file, not a link): give the directory that "
                "your build writes, such as dist/")
    project = [name for name in ("package.json", "node_modules") if (directory / name).exists()]
    if project:
        return (f"--web {directory}: it has {' and '.join(project)}, so it is the project and not its build: give "
                f"the directory that the build writes, such as {directory / 'dist'} (everything in the directory "
                "given is served)")
    return None


class StaticFiles:
    def __init__(self, directory: Path | None):
        self.directory = directory.resolve() if directory else None

    def lookup(self, url_path: str, page_load: bool = True) -> tuple[Path | None, str | None, int]:
        """(file, media type, status) for a request path (already percent-decoded, starting with '/').
        status 200 with file None means: answer with the not-built page. A path without an extension is the
        screen's own route: index.html, but only when the browser loads a page (page_load); another request
        there (a fetch whose base URL is a route) is a 404, not a page in place of JSON."""
        relative = url_path.lstrip("/")
        parts = relative.split("/") if relative else []
        if parts and parts[0] in RESERVED:
            return None, None, 404
        if any(p in ("..", ".") or p.startswith(".") or "\\" in p or "\x00" in p or ":" in p for p in parts):
            return None, None, 404
        if self.directory is None:
            return None, None, 200
        if not parts:
            return self._file(self.directory / "index.html")
        if "." not in parts[-1]:  # a file without an extension has no media type: never served
            return self._file(self.directory / "index.html") if page_load else (None, None, 404)
        if Path(parts[-1]).suffix.lower() not in MEDIA_TYPES:
            return None, None, 404
        return self._file(self.directory / Path(*parts))

    def _checked(self, path: Path) -> Path | None:
        """The file, when the asked name names it itself: not through a link (a symbolic link, a junction) even
        to a file of this directory, nor through another name of it (a Windows short name, a trailing dot)."""
        try:
            resolved = path.resolve(strict=True)
        except (OSError, RuntimeError):
            return None
        if os.path.normcase(resolved) != os.path.normcase(path) or not resolved.is_file():
            return None
        return resolved

    def _file(self, path: Path) -> tuple[Path | None, str | None, int]:
        resolved = self._checked(path)
        media = MEDIA_TYPES.get(resolved.suffix.lower()) if resolved is not None else None
        if media is None:
            return None, None, 404
        return resolved, media, 200

    def read(self, path: Path) -> bytes | None:
        """The content of a file that lookup gave, or None. Opened first and then checked again, so that a link
        put in its place (or in place of a directory above it) between lookup and the reading is not followed.
        Raises TooLarge above MAX_FILE_BYTES."""
        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            fd = os.open(path, flags)
        except OSError:
            return None
        with os.fdopen(fd, "rb") as f:
            try:
                opened = os.fstat(f.fileno())
                named = self._checked(path)
                now = os.stat(named) if named is not None else None
            except OSError:
                return None
            if now is None or not stat.S_ISREG(opened.st_mode):
                return None
            # The same file as the one the checked name names (where the system tells the file's number).
            if opened.st_ino and now.st_ino and (opened.st_dev, opened.st_ino) != (now.st_dev, now.st_ino):
                return None
            if opened.st_size > MAX_FILE_BYTES:
                raise TooLarge(path)
            body = f.read(MAX_FILE_BYTES + 1)
        if len(body) > MAX_FILE_BYTES:
            raise TooLarge(path)
        return body
