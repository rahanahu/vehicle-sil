"""Tests for vehicle_sil_gui.cli port-conflict handling and locale-independent ASCII errors."""

from __future__ import annotations

import asyncio
import errno
import socket
from pathlib import Path

import pytest

from vehicle_sil_gui import cli


class _UsableLauncher:
    usable = True
    problems: list[dict[str, str]] = []

    async def probe(self) -> None:
        return None


def _win_oserror(*, err: int | None, strerror: str | None, winerror: int | None) -> OSError:
    exc = OSError(err, strerror)
    if winerror is not None:
        exc.winerror = winerror  # type: ignore[attr-defined]
    return exc


def test_bind_error_reason_is_locale_independent_ascii() -> None:
    jp_msg = "通常、各ソケット アドレスに対してプロトコル、ネットワーク アドレス、またはポートのどれか 1 つのみを使用できます。"
    assert cli._bind_error_reason(OSError(errno.EADDRINUSE, jp_msg)) == "address already in use"
    assert cli._bind_error_reason(_win_oserror(err=13, strerror=jp_msg, winerror=cli.WSAEADDRINUSE)) == (
        "address already in use"
    )
    assert cli._bind_error_reason(_win_oserror(err=None, strerror=jp_msg, winerror=cli.WSAEACCES)) == (
        "permission denied or port already in use"
    )
    assert cli._bind_error_reason(OSError(errno.EACCES, jp_msg)) == "permission denied or port already in use"
    assert cli._bind_error_reason(_win_oserror(err=None, strerror=jp_msg, winerror=cli.WSAEADDRNOTAVAIL)) == (
        "address not available"
    )
    assert cli._bind_error_reason(_win_oserror(err=None, strerror=jp_msg, winerror=10050)) == "WinError 10050"
    assert cli._bind_error_reason(OSError(errno.EINVAL, jp_msg)) == "EINVAL"
    assert cli._bind_error_reason(OSError(errno.EINVAL, "custom ascii reason")) == "custom ascii reason"
    assert cli._bind_error_reason(OSError()) == "bind failed"


def test_serve_real_port_conflict_returns_exit_2_with_ascii_message(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        holder.bind(("127.0.0.1", 0))
        holder.listen(1)
        busy_port = holder.getsockname()[1]

        args = cli.parse_args(["--port", str(busy_port), "--no-browser"])
        rc = asyncio.run(cli.serve(args, tmp_path, None, _UsableLauncher(), None))  # type: ignore[arg-type]
    finally:
        holder.close()

    captured = capsys.readouterr()
    assert rc == cli.EXIT_USAGE == 2
    assert captured.out == ""
    assert captured.err.isascii()
    assert (
        f"vehicle-sil-gui: error: cannot listen on 127.0.0.1:{busy_port} (address already in use): "
        "choose another --port, or 0 for a free one"
    ) in captured.err


def test_serve_localized_winerror_port_conflict_outputs_pure_ascii(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    jp_err = _win_oserror(
        err=errno.EADDRINUSE,
        strerror="通常、各ソケット アドレスに対してプロトコル、ネットワーク アドレス、またはポートのどれか 1 つのみを使用できます。",
        winerror=cli.WSAEADDRINUSE,
    )

    def _fail_bind(port: int) -> socket.socket:
        raise jp_err

    monkeypatch.setattr(cli, "bind", _fail_bind)
    args = cli.parse_args(["--port", "18080", "--no-browser"])
    rc = asyncio.run(cli.serve(args, tmp_path, None, _UsableLauncher(), None))  # type: ignore[arg-type]

    captured = capsys.readouterr()
    assert rc == cli.EXIT_USAGE == 2
    assert captured.err.isascii()
    assert "cannot listen on 127.0.0.1:18080 (address already in use)" in captured.err


def test_cli_external_registry_validation_and_missing_feature(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    rc_bad = cli.main(["--external-registry", "silkit://0.0.0.0:8500", "--no-browser"])
    err_bad = capsys.readouterr().err
    assert rc_bad == cli.EXIT_USAGE == 2
    assert "loopback address" in err_bad

    rc_path = cli.main(["--external-registry", "silkit://127.0.0.1:8500/sim", "--no-browser"])
    err_path = capsys.readouterr().err
    assert rc_path == cli.EXIT_USAGE == 2
    assert "silkit://<loopback-host>:<port>" in err_path

    from vehicle_sil_gui import __version__
    from vehicle_sil_gui.launcher import REQUIRED_FEATURES

    old_launcher = tmp_path / "old_launcher.py"
    old_launcher.write_text(
        "import json\n"
        f"print(json.dumps({{'vehicle_sil_launch': 1, 'vehicle_sil': {__version__!r}, "
        f"'features': {list(REQUIRED_FEATURES)!r}}}))\n",
        encoding="utf-8",
    )
    rc_feat = cli.main([
        "--launcher", str(old_launcher),
        "--root", str(tmp_path),
        "--external-registry", "silkit://127.0.0.1:8500",
        "--no-browser",
    ])
    err_feat = capsys.readouterr().err
    assert rc_feat == cli.EXIT_USAGE == 2
    assert "has no 'external-registry'" in err_feat


def test_keep_runs_validation_and_server_wiring(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    for bad in ("0", "-2"):
        rc = cli.main(["--keep-runs", bad, "--no-browser"])
        assert rc == cli.EXIT_USAGE == 2
        assert "--keep-runs must be 1 or more" in capsys.readouterr().err

    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options

    server = GuiServer(Options(
        root=tmp_path,
        launcher=Launcher(tmp_path / "vehicle_sil_launch"),
        port=0,
        token="t",
        keep_runs=7,
    ))
    assert server.runs.timelines.kept == 7


def test_exception_handler_suppresses_disconnect_tracebacks_to_debug_and_delegates_others(
    caplog: pytest.LogCaptureFixture,
) -> None:
    import logging

    loop = asyncio.new_event_loop()
    seen_other: list[dict[str, object]] = []
    loop.set_exception_handler(lambda _loop, ctx: seen_other.append(ctx))
    try:
        restore = cli.install_exception_handler(loop)
        caplog.set_level(logging.DEBUG, logger="vehicle_sil_gui")

        for exc in (
            ConnectionResetError("reset"),
            ConnectionAbortedError("aborted"),
            BrokenPipeError("pipe"),
            _win_oserror(err=None, strerror="win reset", winerror=10054),
            _win_oserror(err=None, strerror="win abort", winerror=10053),
            _win_oserror(err=None, strerror="netname", winerror=64),
            _win_oserror(err=None, strerror="op aborted", winerror=995),
        ):
            loop.call_exception_handler({"message": "Fatal error on transport", "exception": exc})

        assert seen_other == []
        assert any("client connection closed" in r.message for r in caplog.records)

        # Unrelated exceptions and task/future exceptions still delegate to the previous handler.
        loop.call_exception_handler({"message": "boom", "exception": RuntimeError("bad")})
        fut = loop.create_future()
        loop.call_exception_handler({"message": "task", "exception": ConnectionResetError("in task"), "future": fut})
        assert len(seen_other) == 2

        restore()
        loop.call_exception_handler({"message": "after restore", "exception": ConnectionResetError("after")})
        assert len(seen_other) == 3
    finally:
        loop.close()
