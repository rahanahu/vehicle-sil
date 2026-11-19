"""Saving the text of a configuration file (POST api/config, config_save.py): which files may be written, the launcher's
check of the text (whether it runs the same commands as the saved file) that decides it, the stamp, and the replacement
of the file; and the options --no-save and --allow-command-changes with `allowed.save`.

The launcher is text_launcher.py: the check of a text to save is made up from marks in the text (its module text says
which). The real launcher's part is test_real_text_check.py."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat
import sys
from pathlib import Path

import pytest
from conftest import CONFIGS, Client, gui_server, run_async
from vehicle_sil_gui.config_save import (TEMP_PREFIX, check_text, check_unchanged, decide, existing_file, new_file,
                                         replace_file, save_request)
from vehicle_sil_gui.launcher import InvalidRequest
from vehicle_sil_gui.paths import MAX_DEPTH, ConfigFile, ConfigRoot, PathRejected
from vehicle_sil_gui.refusal import Refused
from vehicle_sil_gui.server import MAX_TEXT

TEXT_LAUNCHER = Path(__file__).resolve().parent / "text_launcher.py"
POSIX = sys.platform != "win32"
FILE = "simulation.yaml"
ROOT_USER = POSIX and hasattr(os, "geteuid") and os.geteuid() == 0


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@pytest.fixture
def root(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    return root


def log_of(path: Path) -> list[dict]:
    if not path.exists():
        return []
    entries = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    return [e for e in entries if e["argv"][:1] == ["check"]]


def serve(tmp_path: Path, root: Path, args: tuple = (), replay_mistakes: bool = False):
    log = tmp_path / "text-launcher.jsonl"
    return gui_server(tmp_path, root=root, launcher=TEXT_LAUNCHER, env={"TEXT_LAUNCHER_LOG": str(log)},
                      args=args, replay_mistakes=replay_mistakes), log


def refused(status: int, reply: dict, expected: tuple[int, str]) -> str:
    assert (status, reply["error"]["code"]) == expected, reply
    return reply["error"]["message"]


def leftovers(directory: Path) -> list[str]:
    return sorted(p.name for p in directory.iterdir() if p.name.startswith(TEMP_PREFIX))


# ------------------------------------------------------------ the pure parts


def test_what_a_body_may_hold() -> None:
    assert save_request({"file": "a.yaml", "text": "é\r\n", "stamp": None}, 10) == ("a.yaml", "é\r\n".encode(), None)
    stamp = "0" * 64
    assert save_request({"file": "a.yaml", "text": "", "stamp": stamp}, 10)[2] == stamp
    for body in ({"file": "a.yaml", "text": "x"},  # stamp is required (null for a new file)
                 {"file": "a.yaml", "text": "x", "stamp": "A" * 64},
                 {"file": "a.yaml", "text": "x", "stamp": "0" * 63},
                 {"file": "a.yaml", "text": "x", "stamp": 5},
                 {"file": "a.yaml", "text": None, "stamp": None},
                 {"file": "a.yaml", "stamp": None},
                 {"file": "a.yaml", "text": "\ud800", "stamp": None},
                 {"file": "a.yaml", "text": "x", "stamp": None, "variant": "v"}):
        with pytest.raises(InvalidRequest) as e:
            save_request(body, 10)
        assert e.value.code == "invalid-field", body
    with pytest.raises(Refused) as e:
        save_request({"file": "a.yaml", "text": "x" * 11, "stamp": None}, 10)
    assert (e.value.status, e.value.code) == (413, "text-too-large")


def check(result: str = "passed", commands: object = "same", *notes: tuple[str, str]) -> dict:
    findings = [{"severity": "note", "code": code, "message": message} for code, message in notes]
    if result != "passed":
        findings.insert(0, {"severity": "error", "code": "syntax", "message": "line 3: a tab"})
    return {"result": result, "commands": commands, "findings": findings}


def test_the_decision_is_the_launcher_s_commands() -> None:
    decide("a.yaml", check("passed", "same"), False)
    decide("a.yaml", check("failed", "same"), False)  # "same" does not depend on the result
    for commands in ("changed", "not_compared", None, "other"):
        decide("a.yaml", check("passed", commands), True)
        with pytest.raises(Refused) as e:
            decide("a.yaml", check("passed", commands, ("commands-changed", "the commands differ")), False)
        assert (e.value.status, e.value.code) == (403, "commands-changed")
        assert e.value.args[0].startswith("the GUI server saves only an edit that keeps the commands that run (the "
                                          "commands differ): make this edit in a text editor")
        assert e.value.detail["commands"] == commands
    with pytest.raises(Refused) as e:
        decide("a.yaml", check("passed", None), False)
    assert "(commands is null)" in str(e.value) and "--allow-command-changes" in str(e.value)
    # What the launcher cannot read is never saved, whatever the options
    for result in ("unreadable", "usage"):
        for allowed in (False, True):
            with pytest.raises(Refused) as e:
                decide("a.yaml", check(result, None), allowed)
            assert (e.value.status, e.value.code) == (422, "config-unreadable")
            assert "(line 3: a tab)" in str(e.value) and e.value.detail["result"] == result


def test_where_a_new_file_may_be_made(tmp_path: Path) -> None:
    (tmp_path / "sub" / "scenarios").mkdir(parents=True)
    (tmp_path / ".hidden").mkdir()
    (tmp_path / "taken.yaml").write_text("x", encoding="utf-8")
    deep = tmp_path.joinpath(*[f"d{n}" for n in range(MAX_DEPTH + 1)])
    deep.mkdir(parents=True)
    root = ConfigRoot(tmp_path)
    made = new_file(root, "sub/new.yaml")
    assert made == ConfigFile("sub/new.yaml", tmp_path.resolve() / "sub" / "new.yaml")
    assert new_file(root, "/".join(f"d{n}" for n in range(MAX_DEPTH)) + "/a.yml").relative.endswith("/a.yml")
    for relative, code in (("new.json", "not-a-configuration"), ("sim.dashboard.yaml", "not-a-configuration"),
                           ("SIM.Dashboard.YAML", "not-a-configuration"), (".new.yaml", "not-a-configuration"),
                           (".hidden/new.yaml", "not-a-configuration"),
                           ("sub/scenarios/new.yaml", "not-a-configuration"),
                           ("/".join(f"d{n}" for n in range(MAX_DEPTH + 1)) + "/a.yaml", "not-a-configuration"),
                           ("missing/new.yaml", "file-not-found"), ("taken.yaml/new.yaml", "file-not-found"),
                           ("../new.yaml", "outside-root"), ("", "invalid-path"), (5, "invalid-path")):
        with pytest.raises(PathRejected) as e:
            new_file(root, relative)
        assert e.value.code == code, (relative, str(e.value))
    with pytest.raises(Refused) as e:
        new_file(root, "taken.yaml")
    assert (e.value.status, e.value.code) == (409, "config-changed")


@pytest.mark.skipif(not POSIX, reason="symbolic links (POSIX)")
def test_where_an_existing_file_may_be_saved(tmp_path: Path) -> None:
    """The same places as for a new file: what GET api/config reads is not enough (a dashboard file or a scenario file
    can have the marker in a comment)."""
    marked = b"vehicle_sil_simulation: 1\n"
    deep = "/".join(f"d{n}" for n in range(MAX_DEPTH + 1))
    names = ("sim.yaml", "sim.dashboard.yaml", "sub/scenarios/s.yaml", f"{deep}/a.yaml",
             "/".join(f"d{n}" for n in range(MAX_DEPTH)) + "/ok.yaml")
    for name in names:
        (tmp_path / name).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / name).write_bytes(marked)
    root = ConfigRoot(tmp_path)
    assert existing_file(root, "sim.yaml") == ConfigFile("sim.yaml", tmp_path.resolve() / "sim.yaml")
    assert existing_file(root, names[-1]).relative == names[-1]
    for name in names[1:-1]:
        with pytest.raises(PathRejected) as e:
            existing_file(root, name)
        assert e.value.code == "not-a-configuration", name


def test_the_text_stays_readable() -> None:
    check_text("a.yaml", b"vehicle_sil_simulation: 1\n")
    for data, said in ((b"vehicle_sil_simulation: 1\n\0", "a NUL character at byte 26"),
                       (b"key: 1\n", "has no vehicle_sil_simulation in its first 4096 bytes")):
        with pytest.raises(Refused) as e:
            check_text("a.yaml", data)
        assert (e.value.status, e.value.code) == (422, "config-unreadable") and said in str(e.value)


def test_a_new_file_through_links(tmp_path: Path) -> None:
    outside, base = tmp_path / "outside", tmp_path / "root"
    outside.mkdir()
    (base / ".private").mkdir(parents=True)
    (base / "real").mkdir()
    (base / "out").symlink_to(outside)
    (base / "inner").symlink_to(base / ".private")
    (base / "alias").symlink_to(base / "real")
    (base / "dangling.yaml").symlink_to(base / "nowhere.yaml")
    (base / "real" / "sim.yaml").write_text("vehicle_sil_simulation: 1\n", encoding="utf-8")
    (base / "link.yaml").symlink_to(base / "real" / "sim.yaml")
    root = ConfigRoot(base)
    assert new_file(root, "real/new.yaml").absolute == base.resolve() / "real" / "new.yaml"
    # A file that is a link is listed, and its file is written; a linked directory is not entered by the listing
    assert existing_file(root, "link.yaml").absolute == base.resolve() / "real" / "sim.yaml"
    with pytest.raises(PathRejected) as e:
        existing_file(root, "alias/sim.yaml")
    assert e.value.code == "not-a-configuration" and "linked directory" in str(e.value)
    for relative, code in (("out/new.yaml", "outside-root"), ("inner/new.yaml", "not-a-configuration"),
                           ("alias/new.yaml", "not-a-configuration")):
        with pytest.raises(PathRejected) as e:
            new_file(root, relative)
        assert e.value.code == code, relative
    with pytest.raises(Refused):  # a link, even one that leads nowhere, is a file that is there
        new_file(root, "dangling.yaml")


def test_the_file_is_replaced_as_bytes_keeping_its_mode(tmp_path: Path) -> None:
    target = tmp_path / "sim.yaml"
    target.write_bytes(b"old\n")
    target.chmod(0o640)
    data = "﻿vehicle_sil_simulation: 1\r\n# 日本語\r\nno end".encode()
    replace_file(target, data)
    assert target.read_bytes() == data and leftovers(tmp_path) == []
    if POSIX:
        assert stat.S_IMODE(target.stat().st_mode) == 0o640
    new = tmp_path / "new.yaml"
    replace_file(new, b"vehicle_sil_simulation: 1\n")
    assert new.read_bytes() == b"vehicle_sil_simulation: 1\n"
    if POSIX:  # a new file has this user's default mode (the umask applies), not the 0600 of a temporary file
        umask = os.umask(0)
        os.umask(umask)
        assert stat.S_IMODE(new.stat().st_mode) == 0o666 & ~umask


@pytest.mark.skipif(not POSIX or ROOT_USER, reason="modes that this user cannot write (POSIX, not root)")
def test_a_file_that_cannot_be_replaced_leaves_nothing(tmp_path: Path) -> None:
    target = tmp_path / "sim.yaml"
    target.write_bytes(b"old\n")
    target.chmod(0o444)  # read-only: not replaced, as on Windows
    with pytest.raises(PermissionError):
        replace_file(target, b"new\n")
    assert target.read_bytes() == b"old\n" and leftovers(tmp_path) == []
    target.chmod(0o644)
    tmp_path.chmod(0o555)  # the directory: no file can be made next to it
    try:
        with pytest.raises(PermissionError):
            replace_file(target, b"new\n")
    finally:
        tmp_path.chmod(0o755)
    assert target.read_bytes() == b"old\n" and leftovers(tmp_path) == []


def test_a_new_file_does_not_replace_one_made_meanwhile(tmp_path: Path) -> None:
    target = tmp_path / "new.yaml"
    replace_file(target, b"mine\n", new=True)
    assert target.read_bytes() == b"mine\n" and leftovers(tmp_path) == []
    with pytest.raises(FileExistsError):  # another program made it after the checks
        replace_file(target, b"again\n", new=True)
    assert target.read_bytes() == b"mine\n" and leftovers(tmp_path) == []


def test_unchanged_compares_the_bytes(tmp_path: Path) -> None:
    target = tmp_path / "sim.yaml"
    target.write_bytes(b"a\r\n")
    config = ConfigFile("sim.yaml", target)
    check_unchanged(config, sha(b"a\r\n"), 10)
    for stamp in (sha(b"a\n"), None):
        with pytest.raises(Refused) as e:
            check_unchanged(config, stamp, 10)
        assert e.value.code == "config-changed"
    check_unchanged(ConfigFile("new.yaml", tmp_path / "new.yaml"), None, 10)


# ------------------------------------------------------------ POST api/config


def test_an_edit_that_keeps_the_commands_is_saved(tmp_path: Path, root: Path) -> None:
    saved = (root / FILE).read_bytes()
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        status, reply = await c.get(f"api/config?file={FILE}")
        assert status == 200, reply
        text = "﻿" + reply["text"].replace("\n", "\r\n") + "# 編集した\r\nno line end"
        status, reply = await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(saved)})
        assert status == 200, reply
        assert reply == {"file": FILE, "stamp": sha(text.encode())}
        assert (root / FILE).read_bytes() == text.encode()  # as sent: the BOM and the line ends too
        assert leftovers(root) == []
        (checked,) = log_of(log)
        assert checked["argv"][:5] == ["check", "--json", "--text-from-stdin", "--descriptions",
                                       checked["argv"][4]]
        assert checked["argv"][-4:] == ["--compare-commands", FILE, "--", FILE]
        assert (checked["stdin"], checked["request"]) == (text, {"vehicle_sil_run_request": 1})
        # Read again, it is the text that was saved, with the stamp that the save answered
        status, reply = await c.get(f"api/config?file={FILE}")
        assert (status, reply["text"], reply["stamp"]) == (200, text, sha(text.encode()))
        # The old stamp is no longer the file's
        refused(*await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(saved)}),
                (409, "config-changed"))

    with server as gui:
        run_async(main, gui)


def test_an_edit_that_changes_the_commands_is_refused_by_default(tmp_path: Path, root: Path) -> None:
    saved = (root / FILE).read_bytes()
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        for mark, commands in (("changed", "changed"), ("not compared", "not_compared"), ("null", None)):
            text = saved.decode() + f"# stand-in: commands {mark}\n"
            status, reply = await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(saved)})
            message = refused(status, reply, (403, "commands-changed"))
            assert reply["error"]["detail"]["commands"] == commands
            if commands == "changed":
                assert message.startswith("the GUI server saves only an edit that keeps the commands that run (the "
                                          f"commands that run differ from {FILE}: participant 'Engine': command "
                                          "differs): ")
            assert message.endswith("make this edit in a text editor, or start the GUI with "
                                    "--allow-command-changes")
        # A text that the launcher cannot read
        text = saved.decode() + "# stand-in: unreadable text\n"
        status, reply = await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(saved)})
        assert "stand-in: unreadable" in refused(status, reply, (422, "config-unreadable"))
        assert (root / FILE).read_bytes() == saved and leftovers(root) == []
        # A new file is refused before anything is checked: there is nothing to compare it with
        message = refused(*await c.post("api/config", {"file": "new.yaml", "text": saved.decode(), "stamp": None}),
                          (403, "commands-changed"))
        assert message.startswith("new.yaml would be a new file (stamp is null)")
        assert not (root / "new.yaml").exists() and len(log_of(log)) == 4

    with server as gui:
        run_async(main, gui)


def test_with_command_changes_allowed(tmp_path: Path, root: Path) -> None:
    saved = (root / FILE).read_bytes()
    server, log = serve(tmp_path, root, args=("--allow-command-changes",))

    async def main(c: Client) -> None:
        text = saved.decode() + "# stand-in: commands changed\n"
        status, reply = await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(saved)})
        assert status == 200 and (root / FILE).read_text(encoding="utf-8") == text, reply
        # Still never what the launcher cannot read
        bad = text + "# stand-in: unreadable text\n"
        refused(*await c.post("api/config", {"file": FILE, "text": bad, "stamp": sha(text.encode())}),
                (422, "config-unreadable"))
        # A new file: checked with nothing to compare, and made
        status, reply = await c.post("api/config", {"file": "sub/new.yaml", "text": text, "stamp": None})
        assert status == 404 and reply["error"]["code"] == "file-not-found", reply  # its directory is missing
        (root / "sub").mkdir()
        status, reply = await c.post("api/config", {"file": "sub/new.yaml", "text": text, "stamp": None})
        assert status == 200 and reply == {"file": "sub/new.yaml", "stamp": sha(text.encode())}, reply
        assert (root / "sub" / "new.yaml").read_text(encoding="utf-8") == text
        assert "--compare-commands" not in log_of(log)[-1]["argv"]
        assert log_of(log)[-1]["argv"][-2:] == ["--", "sub/new.yaml"]
        # Made once: it is there now
        refused(*await c.post("api/config", {"file": "sub/new.yaml", "text": text, "stamp": None}),
                (409, "config-changed"))
        for name in ("sim.dashboard.yaml", "scenarios/new.yaml", ".new.yaml"):
            refused(*await c.post("api/config", {"file": name, "text": text, "stamp": None}),
                    (400, "not-a-configuration"))
        # A text that is not a simulation configuration is not written as one
        message = refused(*await c.post("api/config", {"file": "other.yaml", "text": "key: 1\n", "stamp": None}),
                          (422, "config-unreadable"))
        assert "has no vehicle_sil_simulation in its first 4096 bytes" in message
        assert not (root / "other.yaml").exists()
        status, reply = await c.get("api/app")
        assert status == 200

    with server as gui:
        run_async(main, gui)
        assert "--allow-command-changes: the screen can save edits that change the commands" in gui.stderr_text()


def test_files_that_are_not_saved(tmp_path: Path, root: Path) -> None:
    files = {"other.yaml": b"key: 1\n", ".hidden/x.yaml": b"vehicle_sil_simulation: 1\n",
             "notes.txt": b"vehicle_sil_simulation: 1\n",
             "sim.dashboard.yaml": b"# vehicle_sil_simulation: 1\npanels: []\n",
             "scenarios/marked.yaml": b"# vehicle_sil_simulation: 1\nsteps: []\n"}
    for name, data in files.items():
        (root / name).parent.mkdir(exist_ok=True)
        (root / name).write_bytes(data)
    server, log = serve(tmp_path, root, args=("--allow-command-changes",))

    async def main(c: Client) -> None:
        text = "vehicle_sil_simulation: 1\n"
        for name, data in files.items():
            refused(*await c.post("api/config", {"file": name, "text": text, "stamp": sha(data)}),
                    (400, "not-a-configuration"))
        message = refused(*await c.post("api/config", {"file": FILE, "text": text + "\0", "stamp": sha(b"")}),
                          (422, "config-unreadable"))
        assert "a NUL character at byte 26" in message
        refused(*await c.post("api/config", {"file": "../x.yaml", "text": text, "stamp": sha(b"")}),
                (400, "outside-root"))
        refused(*await c.post("api/config", {"file": "missing.yaml", "text": text, "stamp": sha(b"")}),
                (404, "file-not-found"))
        message = refused(*await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(b"")}),
                          (409, "config-changed"))
        assert message.startswith(f"{FILE} has changed since the screen read it")
        refused(*await c.post("api/config", {"file": FILE, "text": text, "stamp": None}), (409, "config-changed"))
        message = refused(*await c.post("api/config", {"file": FILE, "text": text}), (400, "invalid-field"))
        assert message.startswith("stamp is missing")
        message = refused(*await c.post("api/config", {"file": FILE, "text": text, "stamp": None, "strict": True}),
                          (400, "invalid-field"))
        assert message == "api/config takes no 'strict': the fields it takes are file, text, stamp"
        largest = "#" * MAX_TEXT
        refused(*await c.post("api/config", {"file": FILE, "text": largest + "x", "stamp": None}),
                (413, "text-too-large"))
        assert log_of(log) == [] and leftovers(root) == []  # none reached the launcher

    with server as gui:
        run_async(main, gui)


@pytest.mark.skipif(not POSIX or ROOT_USER, reason="modes that this user cannot write (POSIX, not root)")
def test_a_file_that_cannot_be_replaced(tmp_path: Path, root: Path) -> None:
    saved = (root / FILE).read_bytes()
    (root / FILE).chmod(0o444)
    server, _ = serve(tmp_path, root)

    async def main(c: Client) -> None:
        text = saved.decode() + "# edited\n"
        message = refused(*await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(saved)}),
                          (409, "config-busy"))
        assert message.startswith(f"{FILE} could not be replaced (the file is read-only for this user)")
        assert (root / FILE).read_bytes() == saved and leftovers(root) == []

    try:
        with server as gui:
            run_async(main, gui)
    finally:
        (root / FILE).chmod(0o644)


def test_a_file_that_changes_during_the_check_is_not_replaced(tmp_path: Path, root: Path) -> None:
    """Another program saves the file while the launcher checks the text: the stamp is compared again before the
    file is replaced, so that program's content stays."""
    saved = (root / FILE).read_bytes()
    server, _ = serve(tmp_path, root)

    async def main(c: Client) -> None:
        text = saved.decode() + "# stand-in: the file changes during the check\n"
        message = refused(*await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(saved)}),
                          (409, "config-changed"))
        assert message.startswith(f"{FILE} has changed since the screen read it")
        assert (root / FILE).read_bytes() == saved + b"# changed by another program\n"
        assert leftovers(root) == []

    with server as gui:
        run_async(main, gui)


def test_a_saved_file_is_described_again(tmp_path: Path, root: Path) -> None:
    """The descriptions of the saved file are made again before the next check of its text (the executables of
    what is saved now describe themselves), also when its modification time and size would say nothing."""
    saved = (root / FILE).read_bytes()
    server, log = serve(tmp_path, root, replay_mistakes=True)  # the saved file is no longer the recorded one

    async def main(c: Client) -> None:
        status, reply = await c.post("api/check", {"file": FILE, "text": saved.decode()})
        assert status == 200 and ["--write-descriptions" in e["argv"] for e in log_of(log)] == [True, False], reply
        before = (root / FILE).stat()
        text = saved.decode().replace("\n", "\r", 1)  # the same size
        status, reply = await c.post("api/config", {"file": FILE, "text": text, "stamp": sha(saved)})
        assert status == 200, reply
        os.utime(root / FILE, ns=(before.st_atime_ns, before.st_mtime_ns))  # the same modification time
        checks = len(log_of(log))
        # The text now differs from the recordings: the describe is all that this looks at
        await c.post("api/check", {"file": FILE, "text": text})
        assert "--write-descriptions" in log_of(log)[checks]["argv"]

    with server as gui:
        run_async(main, gui)


def test_no_save(tmp_path: Path, root: Path) -> None:
    saved = (root / FILE).read_bytes()
    server, log = serve(tmp_path, root, args=("--no-save",))

    async def main(c: Client) -> None:
        message = refused(*await c.post("api/config", {"file": FILE, "text": saved.decode(), "stamp": sha(saved)}),
                          (403, "save-disabled"))
        assert message.startswith("the GUI server was started with --no-save")
        status, reply = await c.get("api/session")
        assert status == 200
        assert reply["allowed"]["save"] == {"ok": False, "reason": "save-disabled", "message": message,
                                            "caution": None}
        assert log_of(log) == []

    with server as gui:
        run_async(main, gui)


def test_allowed_save(tmp_path: Path, root: Path) -> None:
    expected = {"ok": True, "reason": None, "message": None}
    for args in ((), ("--allow-command-changes",)):
        server, _ = serve(tmp_path, root, args=args)

        async def main(c: Client) -> dict:
            status, reply = await c.get("api/session")
            assert status == 200
            sock = await c.ws()
            hello = (await sock.until(lambda m: m["type"] == "hello"))[-1]
            await sock.close()
            assert hello["allowed"]["save"] == reply["allowed"]["save"]
            return reply["allowed"]["save"]

        with server as gui:
            save = run_async(main, gui)
        caution = save.pop("caution")
        assert save == expected
        if not args:
            assert caution is None
        else:
            assert caution["code"] == "command-changes-allowed"
            assert "whoever has its token can run any command as this user" in caution["message"]


def test_the_two_options_together_are_a_usage_error(tmp_path: Path) -> None:
    with gui_server(tmp_path, args=("--no-save", "--allow-command-changes"), expect_start=False) as gui:
        assert gui.proc.returncode == 2
        assert "--no-save and --allow-command-changes cannot be used together" in gui.stderr_text()
