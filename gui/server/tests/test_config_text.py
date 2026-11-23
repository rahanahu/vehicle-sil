"""The text of a configuration file: the check of an unsaved text (POST api/check with text), which starts no process
because the launcher takes each executable's --describe output from the descriptions that a check of the saved file
wrote; describing again (POST api/describe); and reading the text (GET api/config).

The launcher is text_launcher.py (the recordings of the saved file's checks, and the options of saved descriptions);
the real launcher's part is test_real_text_check.py."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

import pytest
from conftest import CONFIGS, Client, gui_server, run_async, scaled
from vehicle_sil_gui.server import MAX_TEXT

TEXT_LAUNCHER = Path(__file__).resolve().parent / "text_launcher.py"
POSIX = sys.platform != "win32"
FILE = "simulation.yaml"


@pytest.fixture
def root(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    return root


class Log:
    """What text_launcher.py was started with: its checks, in order."""

    def __init__(self, path: Path):
        self.path = path

    def checks(self) -> list[dict]:
        if not self.path.exists():
            return []
        entries = [json.loads(line) for line in self.path.read_text(encoding="utf-8").splitlines()]
        return [e for e in entries if e["argv"][:1] == ["check"]]


def serve(tmp_path: Path, root: Path, args: tuple = ()):
    log = Log(tmp_path / "text-launcher.jsonl")
    return gui_server(tmp_path, root=root, launcher=TEXT_LAUNCHER, env={"TEXT_LAUNCHER_LOG": str(log.path)},
                      args=args), log


def touch(path: Path) -> None:
    """A newer modification time: the file's stamp changes, its text does not (the recordings still match)."""
    stat = path.stat()
    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))


def option(argv: list[str], name: str) -> str:
    return argv[argv.index(name) + 1]


def unsaved(root: Path, *lines: str) -> str:
    """The saved text with an edit (CRLF and characters beyond ASCII among it), and lines for the stand-in."""
    return (root / FILE).read_text(encoding="utf-8") + "# 編集中\r\n" + "".join(f"{line}\n" for line in lines)


def refused(status: int, reply: dict, expected: tuple[int, str]) -> str:
    assert (status, reply["error"]["code"]) == expected, reply
    return reply["error"]["message"]


def test_a_text_is_checked_from_stdin_with_the_descriptions_of_the_saved_file(tmp_path: Path, root: Path) -> None:
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        text = unsaved(root)
        status, reply = await c.post("api/check", {"file": FILE, "text": text})
        assert status == 200, reply
        assert {k: v for k, v in reply.items() if k != "check"} == \
            {"file": FILE, "disable": [], "manual": [], "scenario": None, "exit_code": 0}
        assert reply["check"]["result"] == "passed"
        # First the saved file is checked (its executables describe themselves), which saves the descriptions
        describe, checked = log.checks()
        saved = option(describe["argv"], "--write-descriptions")
        assert describe["argv"] == ["check", "--json", "--write-descriptions", saved, "--request",
                                    option(describe["argv"], "--request"), "--", FILE]
        assert (describe["stdin"], describe["request"]) == (None, {"vehicle_sil_run_request": 1})
        # Then the text, on standard input as it was sent, with those descriptions
        assert checked["argv"] == ["check", "--json", "--text-from-stdin", "--descriptions", saved, "--request",
                                   option(checked["argv"], "--request"), "--", FILE]
        assert (checked["stdin"], checked["descriptions"]) == (text, True)
        # The server's files are named nowhere in the answer
        assert saved not in json.dumps(reply) and "vehicle-sil-gui-descriptions" not in json.dumps(reply)
        # Again: the descriptions are there, made from the file as it is
        status, reply = await c.post("api/check", {"file": FILE, "text": text})
        assert status == 200 and len(log.checks()) == 3, reply
        # The saved file has changed: its descriptions are made again before the text is checked
        touch(root / FILE)
        status, reply = await c.post("api/check", {"file": FILE, "text": text})
        assert status == 200, reply
        assert ["--write-descriptions" in e["argv"] for e in log.checks()[3:]] == [True, False]
        assert option(log.checks()[3]["argv"], "--write-descriptions") == saved  # the same file, written again
        return saved

    with server as gui:
        saved = run_async(main, gui)
    assert not Path(saved).parent.exists()  # the server's end removes them


def test_a_text_check_leaves_the_names_and_the_targets_of_the_saved_file(tmp_path: Path, root: Path) -> None:
    """The participant names (the browser's disable and manual are of the saved file's check) and the lists of the
    targets (connect) are of the saved file: the check of a text with other participants changes neither."""
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        status, reply = await c.post("api/connections", {"file": "connect.yaml", "network": "ETH_EXT"})
        assert status == 200, reply
        status, reply = await c.post("api/check", {"file": FILE, "text": unsaved(root, "# stand-in: rename")})
        assert status == 200, reply
        assert {p["name"] for p in reply["check"]["participants"]} >= {"FromText0", "FromText1"}
        checks = len(log.checks())
        # The names are still those of the saved file's check, and no check was needed to say so
        message = refused(*await c.post("api/check", {"file": FILE, "disable": ["FromText0"]}),
                          (400, "unknown-participant"))
        assert "FromText0" in message and len(log.checks()) == checks
        status, reply = await c.post("api/check", {"file": FILE, "disable": ["Dashboard"]})
        assert status == 200, reply
        # The list of ETH_EXT is still the one to pass a target on
        status, reply = await c.post("api/check", {"file": "connect.yaml", "text": unsaved(root)})
        assert status == 200, reply
        status, reply = await c.post("api/check", {"file": "connect.yaml", "connect": {"ETH_EXT": "fake0"}})
        assert status == 200 and reply["check"]["result"] == "passed", reply

    with server as gui:
        run_async(main, gui)


def test_a_text_is_checked_with_no_description_when_the_saved_file_cannot_give_them(tmp_path: Path,
                                                                                     root: Path) -> None:
    """A variant that only the text has: the check of the saved file is a usage error, so it leaves no descriptions,
    and the text is checked with none (the launcher: every participant not_described). Not asked again until the
    file changes. A saved file that can no longer be read leaves none either, also when an earlier check saved
    some."""
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        body = {"file": FILE, "variant": "only-in-text", "text": unsaved(root)}
        status, reply = await c.post("api/check", body)
        assert status == 200 and reply["check"]["result"] == "passed", reply
        describe, checked = log.checks()
        assert describe["request"] == {"vehicle_sil_run_request": 1, "variant": "only-in-text"}
        assert "--write-descriptions" in describe["argv"]
        assert (checked["request"]["variant"], checked["descriptions"]) == ("only-in-text", False)
        # A path where no file is, whatever the check wrote (a file that cannot be removed is not used either)
        assert option(checked["argv"], "--descriptions") != option(describe["argv"], "--write-descriptions")
        status, reply = await c.post("api/check", body)
        assert status == 200 and len(log.checks()) == 3, reply
        # The saved file can no longer be read: what an earlier check saved is not used
        body = {"file": FILE, "text": unsaved(root)}
        status, reply = await c.post("api/check", body)
        assert status == 200 and log.checks()[-1]["descriptions"] is True, reply
        with open(root / FILE, "a", encoding="utf-8") as f:
            f.write("# stand-in: unreadable\n")
        status, reply = await c.post("api/check", body)
        assert status == 200, reply
        describe, checked = log.checks()[-2:]
        assert "--write-descriptions" in describe["argv"] and checked["descriptions"] is False

    with server as gui:
        run_async(main, gui)


def test_a_text_takes_the_variant_and_strict_only(tmp_path: Path, root: Path) -> None:
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        text = unsaved(root)
        for field, value in (("scenario", "scenarios/pass.yaml"), ("disable", ["Dashboard"]), ("enable", ["X"]),
                             ("manual", []), ("connect", {"ETH_EXT": "none"}), ("params", {}),
                             ("time_policy", "free"), ("speed", 1), ("start_paused", False)):
            message = refused(*await c.post("api/check", {"file": FILE, "text": text, field: value}),
                              (400, "invalid-field"))
            assert f"api/check takes no {field!r} together with text" in message, message
        # An unknown field too: the answer lists only what a check of a text takes
        message = refused(*await c.post("api/check", {"file": FILE, "text": text, "varient": "a"}),
                          (400, "invalid-field"))
        assert message == ("api/check takes no 'varient' together with text: a check of unsaved text checks the "
                           "configuration only; the fields it takes with text are file, $schema, "
                           "vehicle_sil_run_request, variant, strict, text")
        for value in (5, None, ["a"]):
            refused(*await c.post("api/check", {"file": FILE, "text": value}), (400, "invalid-field"))
        message = refused(*await c.post("api/check", raw=b'{"file": "simulation.yaml", "text": "\\ud800"}'),
                          (400, "invalid-field"))
        assert "lone surrogate" in message
        refused(*await c.post("api/check", {"file": "../x.yaml", "text": text}), (400, "outside-root"))
        refused(*await c.post("api/check", {"file": "missing.yaml", "text": text}), (404, "file-not-found"))
        for name, data in ((".hidden/x.yaml", b"vehicle_sil_simulation: 2\n"), ("other.yaml", b"key: 1\n")):
            (root / name).parent.mkdir(exist_ok=True)
            (root / name).write_bytes(data)
            for path, body in (("api/check", {"file": name, "text": text}), ("api/describe", {"file": name})):
                refused(*await c.post(path, body), (400, "not-a-configuration"))
        assert log.checks() == []  # none of them reached the launcher
        status, reply = await c.post("api/check", {"file": "variants.yaml", "variant": "small", "strict": True,
                                                   "vehicle_sil_run_request": 1, "$schema": "x", "text": text})
        assert status == 200, reply
        describe, checked = log.checks()
        assert describe["request"] == {"vehicle_sil_run_request": 1, "variant": "small"}
        assert checked["request"] == {"vehicle_sil_run_request": 1, "variant": "small", "strict": True}

    with server as gui:
        run_async(main, gui)


def test_the_sizes_of_a_text(tmp_path: Path, root: Path) -> None:
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        # 1 MiB as UTF-8 is the most; the body may hold it in \\u escapes (6 bytes for 1 at the most)
        largest = "#" + "x" * (MAX_TEXT - 2) + "\n"
        status, reply = await c.post("api/check", {"file": FILE, "text": largest})
        assert status == 200 and log.checks()[-1]["stdin"] == largest, reply
        message = refused(*await c.post("api/check", {"file": FILE, "text": largest + "x"}),
                          (413, "text-too-large"))
        assert f"text is {MAX_TEXT + 1} bytes as UTF-8, more than the {MAX_TEXT} (1 MiB)" in message
        escaped = "あ" * (MAX_TEXT // 3 + 1)
        assert len(json.dumps({"file": FILE, "text": escaped})) > 2 * MAX_TEXT
        refused(*await c.post("api/check", {"file": FILE, "text": escaped}), (413, "text-too-large"))
        controls = "\u0001" * (MAX_TEXT + 1)
        assert len(json.dumps({"file": FILE, "text": controls})) > 6 * MAX_TEXT
        refused(*await c.post("api/check", {"file": FILE, "text": controls}), (413, "text-too-large"))
        # A body beyond that, and any other body beyond 64 KiB
        refused(*await c.post("api/check", {"file": FILE, "text": "x" * (7 * MAX_TEXT)}), (413, "too-large"))
        refused(*await c.post("api/run", {"file": FILE, "params": {"p": "x" * 70_000}}), (413, "too-large"))
        refused(*await c.post("api/describe", {"file": "x" * 70_000}), (413, "too-large"))

    with server as gui:
        run_async(main, gui)


def test_the_names_of_the_servers_files_are_hidden(tmp_path: Path, root: Path) -> None:
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        status, reply = await c.post("api/check", {"file": FILE, "text": unsaved(root, "# stand-in: name the "
                                                                                       "descriptions")})
        assert status == 200, reply
        saved = option(log.checks()[-1]["argv"], "--descriptions")
        note = reply["check"]["findings"][-1]
        assert (note["message"], note["file"], note["line"]) == ("cannot read the saved descriptions",
                                                                 "the saved descriptions", None)
        status, reply = await c.post("api/check", {"file": FILE, "text": unsaved(root, "# stand-in: fail")})
        assert refused(status, reply, (502, "launcher-output"))
        assert reply["error"]["detail"] == ["stand-in: cannot read the saved descriptions"]
        assert saved not in json.dumps(reply)

    with server as gui:
        run_async(main, gui)


def test_describe_again(tmp_path: Path, root: Path) -> None:
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        status, reply = await c.post("api/describe", {"file": "variants.yaml", "variant": "small"})
        assert status == 200, reply
        assert {k: v for k, v in reply.items() if k != "check"} == \
            {"file": "variants.yaml", "variant": "small", "exit_code": 0}
        assert reply["check"]["result"] == "passed"
        (describe,) = log.checks()
        assert "--write-descriptions" in describe["argv"] and describe["request"]["variant"] == "small"
        # The text of that variant is checked with them, without another check of the saved file
        status, reply = await c.post("api/check", {"file": "variants.yaml", "variant": "small",
                                                   "text": unsaved(root)})
        assert status == 200 and len(log.checks()) == 2 and log.checks()[1]["descriptions"] is True, reply
        # Asked again, it describes again (an executable may have been built again)
        status, reply = await c.post("api/describe", {"file": "variants.yaml", "variant": "small"})
        assert status == 200 and "--write-descriptions" in log.checks()[2]["argv"], reply
        status, reply = await c.post("api/describe", {"file": FILE})
        assert status == 200 and reply["variant"] is None, reply
        for body in ({"file": FILE, "scenario": "x"}, {"file": FILE, "variant": None}, {"file": FILE, "text": ""}):
            refused(*await c.post("api/describe", body), (400, "invalid-field"))
        refused(*await c.post("api/describe", {"file": "notes.txt"}), (400, "not-a-configuration"))

    with server as gui:
        run_async(main, gui)


def test_a_text_is_checked_with_no_description_when_describing_fails(tmp_path: Path, root: Path) -> None:
    """The check of the saved file that makes the descriptions does not end (an ECU's --describe hangs): the text is
    checked with none, and the saved file is not checked again for its texts until it changes or api/describe asks
    (which says what went wrong)."""
    with open(root / FILE, "a", encoding="utf-8") as f:
        f.write("# stand-in: describe hangs\n")
    server, log = serve(tmp_path, root, args=("--check-timeout", f"{scaled(0.4):g}"))

    async def main(c: Client) -> None:
        body = {"file": FILE, "text": unsaved(root)}
        for _ in range(2):
            status, reply = await c.post("api/check", body)
            assert status == 200 and reply["check"]["result"] == "passed", reply
        describe, checked, again = log.checks()
        assert "--write-descriptions" in describe["argv"]
        assert (checked["descriptions"], again["descriptions"]) == (False, False)
        assert "--write-descriptions" not in again["argv"]
        refused(*await c.post("api/describe", {"file": FILE}), (504, "check-timeout"))
        assert "--write-descriptions" in log.checks()[-1]["argv"]
        status, reply = await c.post("api/check", body)
        assert status == 200 and "--write-descriptions" not in log.checks()[-1]["argv"], reply
        touch(root / FILE)  # changed: described again first
        status, reply = await c.post("api/check", body)
        assert status == 200, reply
        assert ["--write-descriptions" in e["argv"] for e in log.checks()[-2:]] == [True, False]

    with server as gui:
        run_async(main, gui)


def test_the_run_page_s_check_and_a_run_write_no_descriptions(tmp_path: Path, root: Path) -> None:
    server, log = serve(tmp_path, root)

    async def main(c: Client) -> None:
        status, reply = await c.post("api/check", {"file": FILE, "disable": ["Dashboard"]})
        assert status == 200, reply
        status, reply = await c.post("api/run", {"file": FILE, "disable": ["Dashboard"], "stop_at_ms": 1000})
        assert status == 200, reply
        assert log.checks() and not any(o in e["argv"] for e in log.checks() for o in
                                        ("--write-descriptions", "--descriptions", "--text-from-stdin"))
        checks = len(log.checks())
        status, reply = await c.post("api/check", {"file": FILE, "text": unsaved(root)})
        assert status == 200, reply
        assert ["--write-descriptions" in e["argv"] for e in log.checks()[checks:]] == [True, False]

    with server as gui:
        run_async(main, gui)


def test_reading_the_text_of_a_configuration_file(tmp_path: Path, root: Path) -> None:
    server, _ = serve(tmp_path, root)
    files = {
        "crlf.yaml": "vehicle_sil_simulation: 2\r\n# 日本語\r\nparticipants: []\r\n".encode(),
        "bom.yaml": "\ufeffvehicle_sil_simulation: 1\nparticipants: []".encode(),  # no line end at the end
        "largest.yaml": b"vehicle_sil_simulation: 2\n" + b"#" * (MAX_TEXT - 26),
        "too-large.yaml": b"vehicle_sil_simulation: 2\n" + b"#" * (MAX_TEXT - 25),
        "nul.yaml": b"vehicle_sil_simulation: 2\n\0",
        "latin1.yaml": "vehicle_sil_simulation: 2\n# caf\u00e9\n".encode("latin-1"),
        # Not configuration files: none is read for the screen
        "utf16.yaml": "vehicle_sil_simulation: 2\n".encode("utf-16"),
        "other.yaml": b"oauth_token: x\n",
        "late.yaml": b"#" * 4096 + b"\nvehicle_sil_simulation: 1\n",
        ".hidden/x.yaml": b"vehicle_sil_simulation: 2\n",
        "sub/.x.yaml": b"vehicle_sil_simulation: 2\n",
    }
    for name, data in files.items():
        (root / name).parent.mkdir(exist_ok=True)
        (root / name).write_bytes(data)
    if POSIX:
        (root / "link.yaml").symlink_to(root / ".hidden" / "x.yaml")

    async def main(c: Client) -> None:
        for name in ("crlf.yaml", "bom.yaml", "largest.yaml"):
            status, reply = await c.get(f"api/config?file={name}")
            assert status == 200, reply
            assert reply == {"file": name, "text": files[name].decode("utf-8"),
                             "stamp": hashlib.sha256(files[name]).hexdigest()}
        message = refused(*await c.get("api/config?file=too-large.yaml"), (413, "config-too-large"))
        assert f"larger than {MAX_TEXT} bytes (1 MiB)" in message
        for name, why in (("nul.yaml", "it has a NUL byte at byte 26"), ("latin1.yaml", "byte 31 is not UTF-8")):
            message = refused(*await c.get(f"api/config?file={name}"), (422, "config-unreadable"))
            assert f"{name} is not UTF-8 text ({why}): save it as UTF-8" in message
        for name in ("utf16.yaml", "other.yaml", "late.yaml"):
            message = refused(*await c.get(f"api/config?file={name}"), (400, "not-a-configuration"))
            assert f"{name!r} is not a simulation configuration: it has no vehicle_sil_simulation in its first " \
                   "4096 bytes" in message
        for name in (".hidden/x.yaml", "sub/.x.yaml", *(["link.yaml"] if POSIX else [])):
            message = refused(*await c.get(f"api/config?file={name}"), (400, "not-a-configuration"))
            assert "has a part that starts with '.'" in message
        for query, expected in (("../x.yaml", (400, "outside-root")), ("notes.txt", (400, "not-a-configuration")),
                                ("missing.yaml", (404, "file-not-found")), ("", (400, "invalid-path"))):
            refused(*await c.get(f"api/config?file={query}"), expected)
        refused(*await c.get("api/config"), (400, "invalid-path"))

    with server as gui:
        run_async(main, gui)


@pytest.mark.skipif(not POSIX or (hasattr(os, "geteuid") and os.geteuid() == 0),
                    reason="the mode of a file that this user cannot read (POSIX, not root)")
def test_a_configuration_file_that_cannot_be_read(tmp_path: Path, root: Path) -> None:
    (root / "secret.yaml").write_text("vehicle_sil_simulation: 2\n", encoding="utf-8")
    (root / "secret.yaml").chmod(0)
    server, _ = serve(tmp_path, root)

    async def main(c: Client) -> None:
        message = refused(*await c.get("api/config?file=secret.yaml"), (422, "config-unreadable"))
        assert message.startswith("secret.yaml cannot be read (") and "check that this user can read it" in message

    try:
        with server as gui:
            run_async(main, gui)
    finally:
        (root / "secret.yaml").chmod(0o644)
