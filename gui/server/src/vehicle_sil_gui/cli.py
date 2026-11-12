"""vehicle-sil-gui: starts the GUI server on 127.0.0.1 and opens the screen in the browser."""

from __future__ import annotations

import argparse
import asyncio
import html
import json
import logging
import os
import re
import secrets
import signal
import socket
import sys
import tempfile
import threading
import webbrowser
from pathlib import Path

from aiohttp import web

from . import __version__, launcher as launcher_module
from .paths import ConfigRoot, PathRejected
from .server import CHECK_TIMEOUT_S, END_BUDGET_S, PROTOCOL, RUNNER_SHUTDOWN_S, GuiServer, Options
from .session import SILENT_AFTER_S
from .static import find_web_dir

EXIT_OK, EXIT_FAILED, EXIT_USAGE = 0, 1, 2
DEV_ORIGIN = re.compile(r"http://127\.0\.0\.1:\d{1,5}")
BROWSER_PAGE_S = 30  # how long the page that hands the URL to the browser is kept

DESCRIPTION = """\
Starts the browser GUI of vehicle-sil: a server on 127.0.0.1 that runs vehicle_sil_launch and shows its
progress. It prints the URL to open (with a token that changes on every start) and opens the browser.
Stop it with Ctrl+C; a running simulation is stopped first."""

EPILOG = """\
The launcher is --launcher, else $VEHICLE_SIL_LAUNCH, else vehicle_sil_launch on PATH; it must report the
features events and stdin-commands in `version --json`. A path ending in .py runs with this Python.

exit codes: 0 stopped (Ctrl+C, SIGTERM), 1 failed while running, 2 usage error, or the launcher or the port
cannot be used (the message says what to do)

The ECUs' variables are read from the launcher's control server (a launcher with the feature control-server;
the server is started on a free port of 127.0.0.1 with a token made for each run). They need grpcio and protobuf
in this Python; without them, or with --no-variables, everything else works.

examples:
  vehicle-sil-gui examples/simulation.yaml          # root: the file's directory
  vehicle-sil-gui --root examples --no-browser --json"""


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="vehicle-sil-gui", description=DESCRIPTION, epilog=EPILOG,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("config", nargs="?", help="a configuration file to show first (under --root)")
    p.add_argument("--root", help="the directory whose .yaml/.yml files the screen may use (default: the "
                                  "directory of CONFIG, else the current directory)")
    p.add_argument("--port", type=int, default=0, help="the port on 127.0.0.1 (default 0: a free one)")
    p.add_argument("--no-browser", action="store_true", help="do not open the browser")
    p.add_argument("--launcher", help="the path of vehicle_sil_launch")
    p.add_argument("--dev-origin", help="also accept requests from this development server, "
                                        "http://127.0.0.1:<port> (ng serve's proxy)")
    p.add_argument("--json", action="store_true",
                   help='print {"vehicle_sil_gui": 1, "url": ..., "port": ...} on one line instead of the URL')
    p.add_argument("--stop-timeout", type=float, default=10.0, metavar="SECONDS",
                   help="on exit, how long to wait for a running simulation to stop before the launcher is "
                        "asked to stop with a signal and then killed (default 10)")
    p.add_argument("--check-timeout", type=float, default=CHECK_TIMEOUT_S, metavar="SECONDS",
                   help=f"how long a check (check --json, which runs each ECU's --describe) may take before it "
                        f"is stopped (default {CHECK_TIMEOUT_S:g})")
    p.add_argument("--silent-after", type=float, default=SILENT_AFTER_S, metavar="SECONDS",
                   help="tell the screen when a running launcher has written nothing for this long (it writes a "
                        f"heartbeat every second; default {SILENT_AFTER_S:g})")
    p.add_argument("--no-variables", action="store_true",
                   help="do not start the launcher's control server (run --control): the screen then shows no "
                        "variables of the ECUs, and --start-paused stands still after the first step, not before it")
    p.add_argument("--version", action="version", version=f"vehicle-sil-gui {__version__}")
    return p.parse_args(argv)


def usage_error(message: str) -> int:
    print(f"vehicle-sil-gui: error: {message}", file=sys.stderr)
    return EXIT_USAGE


def bind(port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if sys.platform == "win32":  # no other program may bind the same port and take the connections
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    sock.bind(("127.0.0.1", port))
    sock.listen(128)
    sock.setblocking(False)
    return sock


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(level=logging.WARNING, stream=sys.stderr, format="vehicle-sil-gui: %(message)s")
    if args.dev_origin is not None and not DEV_ORIGIN.fullmatch(args.dev_origin):
        return usage_error(f"--dev-origin {args.dev_origin}: write http://127.0.0.1:<port> (only a development "
                           "server on this computer)")
    if not 0 <= args.port <= 65535:
        return usage_error(f"--port {args.port}: write 0 (a free port) or a port from 1 to 65535")
    if args.port == 80:  # a browser leaves :80 out of Host and Origin, which the checks compare
        return usage_error("--port 80 cannot be used: choose another port, or 0 for a free one")
    if args.stop_timeout < 0:
        return usage_error("--stop-timeout must be 0 or more seconds")
    if not args.check_timeout > 0:
        return usage_error("--check-timeout must be more than 0 seconds")
    if not args.silent_after >= 1:
        return usage_error("--silent-after must be 1 second or more (the launcher writes a heartbeat every second)")
    config = Path(args.config) if args.config else None
    if config is not None and not config.is_file():
        return usage_error(f"{config}: no such file")
    root = Path(args.root) if args.root else (config.resolve().parent if config else Path.cwd())
    try:
        config_root = ConfigRoot(root)
    except (OSError, NotADirectoryError) as e:
        return usage_error(f"--root {root}: not a directory ({e})")
    initial = None
    if config is not None:
        try:
            initial = config_root.relative(config)
        except PathRejected as e:
            return usage_error(f"{config}: {e} (give --root a directory that contains it)")
    try:
        found = launcher_module.find(args.launcher)
    except launcher_module.LauncherNotFound as e:
        return usage_error(str(e))
    try:
        return asyncio.run(serve(args, config_root.root, initial, launcher_module.Launcher(found)))
    except KeyboardInterrupt:
        return EXIT_OK


async def serve(args: argparse.Namespace, root: Path, initial: str | None, launcher: launcher_module.Launcher) -> int:
    await launcher.probe()
    if not launcher.usable:
        for p in launcher.problems:
            print(f"vehicle-sil-gui: error: {p['message']}", file=sys.stderr)
        return EXIT_USAGE
    try:
        sock = bind(args.port)
    except OSError as e:
        return usage_error(f"cannot listen on 127.0.0.1:{args.port} ({e.strerror}): choose another --port, "
                           "or 0 for a free one")
    port = sock.getsockname()[1]
    token = secrets.token_urlsafe(32)
    server = GuiServer(Options(root=root, launcher=launcher, port=port, token=token, initial_file=initial,
                               dev_origin=args.dev_origin, web_dir=find_web_dir(), stop_timeout_s=args.stop_timeout,
                               check_timeout_s=args.check_timeout, silent_after_s=args.silent_after,
                               variables=not args.no_variables))
    runner = web.AppRunner(server.app(), access_log=None, handle_signals=False, shutdown_timeout=RUNNER_SHUTDOWN_S)
    await runner.setup()
    stop = asyncio.Event()
    signals = install_signal_handlers(stop, server)
    page: Path | None = None
    try:
        await web.SockSite(runner, sock).start()
        url = f"http://127.0.0.1:{port}/#token={token}"
        dev_url = f"{args.dev_origin}/#token={token}" if args.dev_origin else None
        if args.json:
            line = {"vehicle_sil_gui": PROTOCOL, "url": url, "port": port}
            if dev_url:
                line["dev_url"] = dev_url
            print(json.dumps(line), flush=True)
        else:
            print(url, flush=True)
            if dev_url:
                print(f"vehicle-sil-gui: through the development server: {dev_url}", file=sys.stderr)
            print("vehicle-sil-gui: open the URL above (it has the token); Ctrl+C stops the server", file=sys.stderr)
        if not args.no_browser:
            page = open_browser(dev_url or url)
            asyncio.get_running_loop().call_later(BROWSER_PAGE_S, remove, page)
        await stop.wait()
        return EXIT_OK
    finally:
        try:  # each step of the end is bounded; this is the bound of all of them
            async with asyncio.timeout(args.stop_timeout + END_BUDGET_S):
                await server.shutdown()
        except TimeoutError:
            server.kill_all()
        await runner.cleanup()
        signals()
        if page is not None:
            remove(page)


def open_browser(url: str) -> Path:
    """Opens the URL through a file that only this user can read: the browser's command line, which other
    users of the computer can see, has the file's path and not the token."""
    fd, name = tempfile.mkstemp(prefix="vehicle-sil-gui-", suffix=".html")  # mode 0600
    link = html.escape(url, quote=True)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(f'<!doctype html><html><head><meta charset="utf-8"><meta http-equiv="refresh" content="0;url={link}">'
                f'<title>vehicle-sil-gui</title></head><body><p><a href="{link}">vehicle-sil-gui</a></p></body></html>\n')
    page = Path(name)
    # Not awaited: a browser command may wait until the browser is closed.
    threading.Thread(target=webbrowser.open, args=(page.as_uri(),), daemon=True).start()
    return page


def remove(page: Path) -> None:
    try:
        page.unlink()
    except OSError:
        pass


def install_signal_handlers(stop: asyncio.Event, server: GuiServer):
    """SIGINT, SIGTERM, SIGHUP (the terminal closed); on Windows Ctrl+C and Ctrl+Break / closing the console.
    The first stops the server, a second one skips the waiting for the simulation to stop."""
    loop = asyncio.get_running_loop()

    def on_signal() -> None:
        if stop.is_set():
            loop.create_task(server.shutdown())  # the second call hurries
        stop.set()

    if sys.platform == "win32":
        names = [signal.SIGINT, signal.SIGBREAK]
        previous = {s: signal.signal(s, lambda *_: loop.call_soon_threadsafe(on_signal)) for s in names}

        def restore() -> None:
            for s, handler in previous.items():
                signal.signal(s, handler)
        return restore
    names = [signal.SIGINT, signal.SIGTERM, signal.SIGHUP]
    for s in names:
        loop.add_signal_handler(s, on_signal)

    def remove() -> None:
        for s in names:
            loop.remove_signal_handler(s)
    return remove


if __name__ == "__main__":
    sys.exit(main())
