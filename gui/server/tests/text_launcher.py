#!/usr/bin/env python3
"""A stand-in for vehicle_sil_launch for the checks of an unsaved text (POST api/check with `text`, POST
api/describe): it plays the recording of the same check of the saved file (replay_launcher.py), and does with the
options of saved descriptions what the GUI server's tests look at.

    text_launcher.py <the arguments the GUI server gives vehicle_sil_launch>

- check --write-descriptions <file>: writes <file> (a stand-in content) when the check could read the
  configuration (its result is not usage or unreadable), as the real launcher does.
- check --text-from-stdin --descriptions <file>: reads the text from standard input to its end; the check object is
  the one of the saved file.
- Every other command line is replay_launcher.py's.

TEXT_LAUNCHER_LOG=<file>: each start adds one JSON line: {"argv", "stdin" (the text read, or null), "request" (the
request file's keys, or null), "descriptions" (with --descriptions: whether that file was there)}.

What the tests ask for:
- the variant "only-in-text": the check of the saved file is a usage error (it has no such variant), as the real
  launcher's; the check of a text plays the saved file's check without a variant (as if the text had that variant);
- a saved file with the line "# stand-in: unreadable": its check says that it cannot be read; with the line
  "# stand-in: describe hangs", check --write-descriptions does not end (a --describe that hangs). The check of a
  text of such a file has no participant (no recording is of that file);
- a text with the line "# stand-in: rename": its participants are named FromText (a text with other participants);
- a text with the line "# stand-in: name the descriptions": a note that names the --descriptions file (the GUI server
  must hide the name);
- a text with the line "# stand-in: fail": no check object, and a line on standard error that names that file.
- `strict` of the request is taken out before the recording is chosen (none is recorded with --strict).

The check of a text that POST api/config would save (check --text-from-stdin with --compare-commands, or of a file
that is not there yet) plays no recording (the saved file changes in those tests): it is a check object made up from
the text. Its result is passed, or unreadable with the line "# stand-in: unreadable text". With --compare-commands its
`commands` is "changed" when a `command:` line of a participant differs from the file's, or with the line "# stand-in:
commands changed"; "not_compared" ("# stand-in: commands not compared"); null ("# stand-in: commands null"); else
"same". The launcher's notes commands-changed and commands-not-compared come with them. Without the option, null.
With the line "# stand-in: the file changes during the check", it adds the line "# changed by another program" to the
file of --compare-commands before it answers (another program saved the file while the launcher checked).
"""

from __future__ import annotations

import json
import os
import runpy
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPLAY = Path(__file__).resolve().parent / "stub_launcher.py"
OPTIONS = ("--descriptions", "--write-descriptions", "--compare-commands")
COMMANDS = {"# stand-in: commands changed": "changed", "# stand-in: commands not compared": "not_compared",
            "# stand-in: commands null": None}


def log(entry: dict) -> None:
    path = os.environ.get("TEXT_LAUNCHER_LOG")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")


def fabricated(result: str, participants: list | None = None) -> str:
    """A check object that no recording has (the keys that the GUI server and its tests read)."""
    errors = 0 if result == "passed" else 1
    findings = [] if not errors else [{"severity": "error", "code": result, "message": f"stand-in: {result}",
                                       "file": None, "line": None, "participant": None, "key": None}]
    elig = (
        {"eligible": True, "reason": None, "message": None}
        if result == "passed"
        else {"eligible": False, "reason": "usage" if result == "usage" else "config-unreadable", "message": f"stand-in: {result}"}
    )
    return json.dumps({"vehicle_sil_check": 1, "result": result, "errors": errors, "warnings": 0, "notes": 0,
                       "participants": participants or [], "networks": [], "findings": findings,
                       "file_sha256": "0" * 64 if result != "unreadable" else None,
                       "hot_eligibility": elig}, indent=2)


def command_lines(text: str) -> dict[str, str]:
    """participant name -> its `command:` line, of a configuration written as the tests' configurations are."""
    out, name = {}, ""
    for line in text.splitlines():
        if line.strip().startswith("- name:"):
            name = line.split(":", 1)[1].strip()
        elif line.strip().startswith("command:"):
            out[name] = line.strip()
    return out


def save_check(text: str, compare: str | None) -> int:
    """The check of a text that would be saved (see the module's text)."""
    lines = text.splitlines()
    result = "unreadable" if "# stand-in: unreadable text" in lines else "passed"
    check = json.loads(fabricated(result))
    commands = None
    if compare is not None and "# stand-in: the file changes during the check" in lines:
        with open(compare, "a", encoding="utf-8", newline="") as f:
            f.write("# changed by another program\n")
    if compare is not None:
        ours, theirs = command_lines(text), command_lines(Path(compare).read_text(encoding="utf-8"))
        differ = [n for n in ours if theirs.get(n, ours[n]) != ours[n]]
        commands = next((COMMANDS[line] for line in lines if line in COMMANDS), "changed" if differ else "same")
        if commands == "changed":
            message = (f"the commands that run differ from {compare}: participant '{(differ or ['Engine'])[0]}': "
                       "command differs")
        else:
            message = f"the commands that run are not compared with {compare}: stand-in"
        if commands in ("changed", "not_compared"):
            check["findings"].append({"severity": "note", "code": f"commands-{commands.replace('_', '-')}",
                                      "message": message, "file": None, "line": None, "participant": None,
                                      "key": None})
            check["notes"] = 1
    check["commands"] = commands
    print(json.dumps(check, indent=2))
    return 0 if result == "passed" else 3


def main(argv: list[str]) -> int:
    end = argv.index("--") if "--" in argv else len(argv)
    options = {o: argv[argv.index(o) + 1] for o in OPTIONS if o in argv[:end]}
    from_stdin = "--text-from-stdin" in argv[:end]
    rest = [a for i, a in enumerate(argv) if a != "--text-from-stdin" and a not in OPTIONS
            and not (i > 0 and argv[i - 1] in OPTIONS)]
    text = sys.stdin.buffer.read().decode("utf-8") if from_stdin else None
    request = None
    if "--request" in rest:
        at = rest.index("--request") + 1
        request = json.loads(Path(rest[at]).read_text(encoding="utf-8"))
    saved = options.get("--descriptions")
    log({"argv": argv, "stdin": text, "request": request,
         "descriptions": None if saved is None else Path(saved).is_file()})
    if argv[:1] != ["check"]:
        sys.argv = [str(REPLAY), *argv]
        runpy.run_path(str(REPLAY), run_name="__main__")
        return 0
    if (request or {}).get("variant") == "only-in-text" and not from_stdin:
        print(fabricated("usage"))
        return 4
    config = Path(argv[end + 1]) if end + 1 < len(argv) else None
    marks = set(config.read_text(encoding="utf-8").splitlines()) if config is not None and config.is_file() else set()
    if from_stdin and ("--compare-commands" in options or config is None or not config.is_file()):
        return save_check(text or "", options.get("--compare-commands"))
    if "# stand-in: describe hangs" in marks and "--write-descriptions" in options:
        time.sleep(600)
    if marks & {"# stand-in: unreadable", "# stand-in: describe hangs"}:
        print(fabricated("passed" if from_stdin else "unreadable"))
        return 0 if from_stdin else 1
    if "# stand-in: fail" in (text or "").splitlines():
        sys.stderr.write(f"stand-in: cannot read {saved}\n")
        return 70
    with tempfile.TemporaryDirectory() as scratch:
        if request is not None and ("strict" in request or request.get("variant") == "only-in-text"):
            plain = Path(scratch) / "request.json"
            plain.write_text(json.dumps({k: v for k, v in request.items() if k not in ("strict", "variant")
                                         or k == "variant" and v != "only-in-text"}), encoding="utf-8")
            rest[rest.index("--request") + 1] = str(plain)
        p = subprocess.run([sys.executable, str(REPLAY), *rest], stdin=subprocess.DEVNULL, capture_output=True)
    out, err = p.stdout.decode("utf-8"), p.stderr.decode("utf-8")
    try:
        check = json.loads(out)
    except ValueError:
        check = None
    if isinstance(check, dict):
        if "--write-descriptions" in options and check.get("result") not in ("usage", "unreadable"):
            Path(options["--write-descriptions"]).write_text(json.dumps({"stand_in": rest[-1]}), encoding="utf-8")
        lines = (text or "").splitlines()
        if "# stand-in: rename" in lines:
            for n, participant in enumerate(check.get("participants", [])):
                participant["name"] = f"FromText{n}"
        if "# stand-in: name the descriptions" in lines:
            check.setdefault("findings", []).append(
                {"severity": "note", "code": "descriptions-not-used", "message": f"cannot read {saved}",
                 "file": saved, "line": 1, "participant": None, "key": None})
        out = json.dumps(check, indent=2, ensure_ascii=False) + "\n"
    sys.stdout.write(out)
    sys.stderr.write(err)
    return p.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
