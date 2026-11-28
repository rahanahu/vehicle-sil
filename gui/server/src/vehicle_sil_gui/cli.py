"""vehicle-sil-gui: starts the GUI server on 127.0.0.1 and opens the screen in the browser."""

from __future__ import annotations

import argparse
import asyncio
import errno
import html
import json
import logging
import math
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
from .contracts import PROTOCOL
from .paths import ConfigRoot, PathRejected
from .run_manager import END_BUDGET_S, RUNNER_SHUTDOWN_S
from .server import CHECK_TIMEOUT_S, GuiServer, Options
from .session import SILENT_AFTER_S
from .static import check_web_dir, find_web_dir
from .timelines import KEPT as TIMELINES_KEPT

EXIT_OK, EXIT_FAILED, EXIT_USAGE = 0, 1, 2
WSAEACCES, WSAEADDRINUSE, WSAEADDRNOTAVAIL = 10013, 10048, 10049
DEV_ORIGIN = re.compile(r"http://127\.0\.0\.1:\d{1,5}")
BROWSER_PAGE_S = 30  # how long the page that hands the URL to the browser is kept

DESCRIPTION = """\
Starts the browser GUI of vehicle-sil: a server on 127.0.0.1 that runs vehicle_sil_launch and shows its
progress. It prints the URL to open (with a token that changes on every start) and opens the browser.
Stop it with Ctrl+C; a running simulation is stopped first."""

EPILOG = """\
The launcher is --launcher, else $VEHICLE_SIL_LAUNCH, else vehicle_sil_launch on PATH; it must report the
features {required} in `version --json`. A path ending in .py runs with this Python.

exit codes: 0 stopped (Ctrl+C, SIGTERM), 1 failed while running, 2 usage error, or the launcher or the port
cannot be used (the message says what to do)

The ECUs' variables are read from the launcher's control server (a launcher with the feature control-server;
the server is started on a free port of 127.0.0.1 with a token made for each run). They need grpcio and protobuf
in this Python; without them, or with --no-variables, everything else works.

The screen can save the text of a configuration file (POST api/config) when the launcher's check says that the edit
keeps the commands that run (the networks, the mapping, external: the work of a network editor). Whoever has the
token can then change those parts of the configuration files, and no copy of the old content is kept. --no-save saves
nothing. --allow-command-changes also saves an edit that changes the commands, and new files: then whoever has the
token in the URL can run any command as this user (also the code of a --web screen and of its panels). --no-auth
disables token authentication for the HTTP API and WebSocket (while keeping 127.0.0.1, Host, Origin, CSP and --root
checks): any local process on this computer can then use the API (and with --allow-command-changes, run any command
as this user).

examples:
  vehicle-sil-gui examples/simulation.yaml          # root: the file's directory
  vehicle-sil-gui --root examples --no-browser --json
  vehicle-sil-gui --web my-screen/dist examples/simulation.yaml   # your own screen (docs/gui.md)""".replace(
    "{required}", ", ".join(launcher_module.REQUIRED_FEATURES))


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="vehicle-sil-gui", description=DESCRIPTION, epilog=EPILOG,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("config", nargs="?", help="a configuration file to show first (under --root)")
    p.add_argument("--root", help="the directory whose .yaml/.yml files the screen may use (default: the "
                                  "directory of CONFIG, else the current directory)")
    p.add_argument("--port", type=int, default=0, help="the port on 127.0.0.1 (default 0: a free one)")
    p.add_argument("--no-browser", action="store_true", help="do not open the browser")
    p.add_argument("--launcher", help="the path of vehicle_sil_launch")
    p.add_argument("--external-registry", metavar="URI",
                   help="connect each run to a user-started SIL Kit registry at silkit://<loopback-host>:<port> "
                        "(e.g. official sil-kit-registry started with --dashboard-uri for Vector SIL Kit Dashboard)")
    p.add_argument("--web", metavar="DIR",
                   help="serve your own screen: the directory of its built files, with index.html (default: the "
                        "sample screen). Only the files in DIR are served, with the same checks and CSP")
    p.add_argument("--dev-origin", help="also accept requests from this development server, "
                                        "http://127.0.0.1:<port> (the proxy of ng serve, Vite and the like)")
    p.add_argument("--json", action="store_true",
                   help=f'print {{"vehicle_sil_gui": {PROTOCOL}, "url": ..., "port": ..., "pid": ..., "instance": ...}} '
                        "on one line instead of the URL")
    p.add_argument("--stop-timeout", type=float, metavar="SECONDS",
                   help="on exit, wait this many seconds before signaling the launcher (default: wait for it)")
    p.add_argument("--participant-start-timeout-ms", type=int, default=30000,
                   help="how long the launcher waits for required participants to join")
    p.add_argument("--participant-stop-timeout-ms", type=int, default=10000,
                   help="how long the launcher waits for participants per shutdown stage")
    p.add_argument("--check-timeout", type=float, default=CHECK_TIMEOUT_S, metavar="SECONDS",
                   help=f"how long a check (check --json, which runs each ECU's --describe) may take before it "
                        f"is stopped (default {CHECK_TIMEOUT_S:g})")
    p.add_argument("--silent-after", type=float, default=SILENT_AFTER_S, metavar="SECONDS",
                   help="tell the screen when a running launcher has written nothing for this long (it writes a "
                        f"heartbeat every second; default {SILENT_AFTER_S:g})")
    p.add_argument("--keep-runs", type=int, default=TIMELINES_KEPT, metavar="N",
                   help="how many scenario timeline files to keep on the server (default "
                        f"{TIMELINES_KEPT}; failed runs are kept before passing ones)")
    p.add_argument("--no-variables", action="store_true",
                   help="do not start the launcher's control server (run --control): the screen then shows no "
                        "variables of the ECUs, and --start-paused stands still after the first step, not before it")
    p.add_argument("--no-save", action="store_true",
                   help="the screen saves no configuration file (POST api/config is refused with save-disabled); use "
                        "it when the screen is only for looking, or others may see the token")
    p.add_argument("--allow-command-changes", action="store_true",
                   help="the screen also saves an edit that changes the commands that run, and new files: whoever "
                        "has the token can then run any command as this user (not with --no-save)")
    p.add_argument("--no-auth", action="store_true",
                   help="disable token authentication for HTTP API and WebSocket connections: any local process on "
                        "this computer can control simulations and edit configurations under --root (127.0.0.1, Host, "
                        "Origin, CSP and --root checks still apply)")
    p.add_argument("--version", action="version", version=f"vehicle-sil-gui {__version__}")
    return p.parse_args(argv)


def usage_error(message: str) -> int:
    print(f"vehicle-sil-gui: error: {message}", file=sys.stderr)
    return EXIT_USAGE


def _bind_error_reason(exc: OSError) -> str:
    """Locale-independent ASCII reason for a failed bind() on 127.0.0.1."""
    winerror = getattr(exc, "winerror", None)
    if exc.errno == errno.EADDRINUSE or winerror == WSAEADDRINUSE:
        return "address already in use"
    if exc.errno == errno.EACCES or winerror == WSAEACCES:
        return "permission denied or port already in use"
    if exc.errno == errno.EADDRNOTAVAIL or winerror == WSAEADDRNOTAVAIL:
        return "address not available"
    if exc.strerror and exc.strerror.isascii():
        return exc.strerror
    if winerror is not None:
        return f"WinError {winerror}"
    if exc.errno is not None:
        return errno.errorcode.get(exc.errno, f"errno {exc.errno}")
    return "bind failed"


def bind(port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if sys.platform == "win32":  # no other program may bind the same port and take the connections
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    if sys.platform != "win32" and port != 0:  # allow immediate restart on a fixed port across TIME_WAIT
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("127.0.0.1", port))
        sock.listen(128)
    except OSError:
        sock.close()
        raise
    sock.setblocking(False)
    return sock


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(level=logging.WARNING, stream=sys.stderr, format="vehicle-sil-gui: %(message)s")
    if args.dev_origin is not None and not DEV_ORIGIN.fullmatch(args.dev_origin):
        return usage_error(f"--dev-origin {args.dev_origin}: write http://127.0.0.1:<port> (only a development "
                           "server on this computer)")
    if args.external_registry is not None and (
        problem := launcher_module.external_registry_uri_error(args.external_registry)
    ):
        return usage_error(problem)
    if not 0 <= args.port <= 65535:
        return usage_error(f"--port {args.port}: write 0 (a free port) or a port from 1 to 65535")
    if args.port == 80:  # a browser leaves :80 out of Host and Origin, which the checks compare
        return usage_error("--port 80 cannot be used: choose another port, or 0 for a free one")
    if args.stop_timeout is not None and (not math.isfinite(args.stop_timeout) or args.stop_timeout < 0):
        return usage_error("--stop-timeout must be a finite number of 0 or more seconds")
    if not (1 <= args.participant_start_timeout_ms <= 10**12 and
            1 <= args.participant_stop_timeout_ms <= 10**12):
        return usage_error("participant timeouts must be integers from 1 to 1000000000000 ms")
    if not args.check_timeout > 0:
        return usage_error("--check-timeout must be more than 0 seconds")
    if not args.silent_after >= 1:
        return usage_error("--silent-after must be 1 second or more (the launcher writes a heartbeat every second)")
    if args.keep_runs < 1:
        return usage_error("--keep-runs must be 1 or more")
    if args.no_save and args.allow_command_changes:
        return usage_error("--no-save and --allow-command-changes cannot be used together: --no-save saves nothing; "
                           "leave out one of them")
    web_dir = Path(args.web) if args.web is not None else find_web_dir()
    if args.web is not None and (problem := check_web_dir(web_dir)):
        return usage_error(problem)
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
    extra_features = (launcher_module.FEATURE_EXTERNAL_REGISTRY,) if args.external_registry else ()
    try:
        return asyncio.run(
            serve(args, config_root.root, initial, launcher_module.Launcher(found, extra_features=extra_features), web_dir)
        )
    except KeyboardInterrupt:
        return EXIT_OK


async def serve(args: argparse.Namespace, root: Path, initial: str | None, launcher: launcher_module.Launcher,
                web_dir: Path | None) -> int:
    stop_timeout_s = args.stop_timeout
    await launcher.probe()
    if not launcher.usable:
        for p in launcher.problems:
            print(f"vehicle-sil-gui: error: {p['message']}", file=sys.stderr)
        return EXIT_USAGE
    try:
        sock = bind(args.port)
    except OSError as e:
        return usage_error(f"cannot listen on 127.0.0.1:{args.port} ({_bind_error_reason(e)}): choose another --port, "
                           "or 0 for a free one")
    port = sock.getsockname()[1]
    token = "" if args.no_auth else secrets.token_urlsafe(32)
    server = GuiServer(Options(root=root, launcher=launcher, port=port, token=token, initial_file=initial,
                               dev_origin=args.dev_origin, web_dir=web_dir, stop_timeout_s=stop_timeout_s,
                               participant_start_timeout_ms=args.participant_start_timeout_ms,
                               participant_stop_timeout_ms=args.participant_stop_timeout_ms,
                               external_registry_uri=args.external_registry,
                               check_timeout_s=args.check_timeout, silent_after_s=args.silent_after,
                               keep_runs=args.keep_runs, variables=not args.no_variables, save=not args.no_save,
                               command_changes_allowed=args.allow_command_changes, auth=not args.no_auth))
    runner = web.AppRunner(server.app(), access_log=None, handle_signals=False, shutdown_timeout=RUNNER_SHUTDOWN_S)
    await runner.setup()
    stop = asyncio.Event()
    restore_exc = install_exception_handler(asyncio.get_running_loop())
    signals = install_signal_handlers(stop, server)
    page: Path | None = None
    try:
        await web.SockSite(runner, sock).start()
        if args.no_auth:
            url = f"http://127.0.0.1:{port}/"
            dev_url = f"{args.dev_origin}/" if args.dev_origin else None
        else:
            url = f"http://127.0.0.1:{port}/#token={token}"
            dev_url = f"{args.dev_origin}/#token={token}" if args.dev_origin else None
        def emit_warnings() -> None:
            if args.no_auth:
                print("vehicle-sil-gui: WARNING: --no-auth: token authentication is disabled; any local process on "
                      "this computer can use the GUI API and control simulations (127.0.0.1, Host, Origin, CSP and "
                      "--root checks still apply)", file=sys.stderr, flush=True)
            if args.allow_command_changes:
                if args.no_auth:
                    print("vehicle-sil-gui: WARNING: --allow-command-changes with --no-auth: token authentication is "
                          "disabled and the screen can save edits that change the commands that run, and new files, so "
                          "any local process on this computer can run any command as this user",
                          file=sys.stderr, flush=True)
                else:
                    print("vehicle-sil-gui: --allow-command-changes: the screen can save edits that change the "
                          "commands that run, and new files, so whoever has the token in the URL can run any command "
                          "as this user", file=sys.stderr, flush=True)

        if args.json:
            # Flush startup warnings to stderr before the stdout JSON readiness line unblocks callers.
            emit_warnings()
            line = {
                "vehicle_sil_gui": PROTOCOL,
                "url": url,
                "port": port,
                "pid": os.getpid(),
                "instance": server.instance,
            }
            if dev_url:
                line["dev_url"] = dev_url
            if args.no_auth:
                line["auth"] = False
            print(json.dumps(line), flush=True)
        else:
            print(url, flush=True)
            if dev_url:
                print(f"vehicle-sil-gui: through the development server: {dev_url}", file=sys.stderr, flush=True)
            if args.no_auth:
                print("vehicle-sil-gui: open the URL above; Ctrl+C stops the server", file=sys.stderr, flush=True)
            else:
                print("vehicle-sil-gui: open the URL above (it has the token); Ctrl+C stops the server",
                      file=sys.stderr, flush=True)
            emit_warnings()
        if not args.no_browser:
            if args.no_auth:
                open_url(dev_url or url)
            else:
                page = open_browser(dev_url or url)
                asyncio.get_running_loop().call_later(BROWSER_PAGE_S, remove, page)
        await stop.wait()
        return EXIT_OK
    finally:
        try:  # an explicit deadline also bounds the rest of the server's shutdown
            async with asyncio.timeout(None if stop_timeout_s is None else stop_timeout_s + END_BUDGET_S):
                await server.shutdown()
        except TimeoutError:
            server.kill_all()
        await runner.cleanup()
        signals()
        restore_exc()
        if page is not None:
            remove(page)


def open_url(url: str) -> None:
    """Opens a token-less URL directly in the browser (no secret to keep off the command line)."""
    threading.Thread(target=webbrowser.open, args=(url,), daemon=True).start()


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


_DISCONNECT_WINERRORS = frozenset({64, 995, 10053, 10054})
_DISCONNECT_TYPES = (ConnectionResetError, ConnectionAbortedError, BrokenPipeError)


def _is_client_disconnect(context: dict[str, object]) -> bool:
    if "future" in context:
        return False
    exc = context.get("exception")
    if isinstance(exc, _DISCONNECT_TYPES):
        return True
    return isinstance(exc, OSError) and getattr(exc, "winerror", None) in _DISCONNECT_WINERRORS


def install_exception_handler(loop: asyncio.AbstractEventLoop):
    """Logs abrupt client socket disconnects (common on Windows ProactorEventLoop) at debug level instead
    of printing a traceback to stderr."""
    previous = loop.get_exception_handler()
    gui_log = logging.getLogger("vehicle_sil_gui")

    def handler(current_loop: asyncio.AbstractEventLoop, context: dict[str, object]) -> None:
        if _is_client_disconnect(context):
            exc = context.get("exception")
            gui_log.debug("client connection closed (%s): %s", context.get("message", "transport"), exc)
            return
        if previous is not None:
            previous(current_loop, context)
        else:
            current_loop.default_exception_handler(context)

    loop.set_exception_handler(handler)

    def restore() -> None:
        if loop.get_exception_handler() is handler:
            loop.set_exception_handler(previous)

    return restore


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
