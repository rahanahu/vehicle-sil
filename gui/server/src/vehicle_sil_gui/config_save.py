"""Saving the text of a configuration file for the screen (POST api/config): what the body may hold, which files may be
written, whether the launcher's check of the text lets the server save it, and the replacement of the file.

The rule of what may change is the launcher's: `check --compare-commands` says whether the text runs the same commands
as the saved file (`commands` of its JSON). This module only reads that answer; it does not read YAML. By default only
`"same"` is saved, and no new file is made; `--allow-command-changes` lets the screen save any text that the launcher
can read as a configuration, and make new files. Nothing here knows HTTP: server.py relays.
"""

from __future__ import annotations

import errno
import hashlib
import json
import os
import re
import secrets
import stat
from pathlib import Path

from .dashboard import SUFFIX as DASHBOARD_SUFFIX
from .launcher import InvalidRequest
from .paths import MARKER, MARKER_BYTES, MAX_DEPTH, SUFFIXES, ConfigFile, ConfigRoot, PathRejected
from .scenarios import DIRECTORY as SCENARIO_DIRECTORY

SAVE_FIELDS = ("file", "text", "stamp")
STAMP = re.compile(r"[0-9a-f]{64}")
# The file that is written before it replaces the configuration file, in the same directory: its name starts with '.'
# and does not end with .yaml or .yml, so the listing of the configuration files never shows it.
TEMP_PREFIX = ".vehicle-sil-gui-save-"
TEMP_SUFFIX = ".tmp"


class SaveRefused(Exception):
    """A save that is refused: an HTTP status, a code of the protocol, a sentence (and a detail, or None)."""

    def __init__(self, status: int, code: str, message: str, detail: object = None):
        super().__init__(message)
        self.status, self.code, self.detail = status, code, detail


def stamp_of(data: bytes) -> str:
    """The stamp of a configuration file's content: the SHA-256 of its bytes (as GET api/config gives it)."""
    return hashlib.sha256(data).hexdigest()


def save_request(body: dict, max_text: int) -> tuple[object, bytes, str | None]:
    """(file, the text as UTF-8 bytes, stamp) of a body of POST api/config; stamp None: a new file. The file is not
    checked here (ConfigRoot does it). Raises InvalidRequest (invalid-field) or SaveRefused (413 text-too-large)."""
    unknown = sorted(k for k in body if k not in SAVE_FIELDS)
    if unknown:
        raise InvalidRequest("invalid-field", f"api/config takes no {', '.join(repr(k) for k in unknown)}: the fields "
                                              f"it takes are {', '.join(SAVE_FIELDS)}")
    text = body.get("text")
    if not isinstance(text, str):
        raise InvalidRequest("invalid-field", "text must be the text to save (a string): the whole content of the "
                                              "configuration file")
    if "stamp" not in body:
        raise InvalidRequest("invalid-field", "stamp is missing: send the stamp that GET api/config gave for the "
                                              "file, or null to create a new file")
    stamp = body["stamp"]
    if stamp is not None and not (isinstance(stamp, str) and STAMP.fullmatch(stamp)):
        raise InvalidRequest("invalid-field", "stamp must be the stamp that GET api/config gave for the file (64 "
                                              "lowercase hexadecimal digits), or null to create a new file")
    try:
        data = text.encode("utf-8")
    except UnicodeEncodeError:
        raise InvalidRequest("invalid-field", "text is not valid Unicode (it has a lone surrogate such as "
                                              "\\ud800): send the text as UTF-8") from None
    if len(data) > max_text:
        raise SaveRefused(413, "text-too-large",
                          f"text is {len(data)} bytes as UTF-8, more than the {max_text} (1 MiB) that the GUI server "
                          "saves: edit the file in a text editor")
    return body.get("file"), data, stamp


def new_file(root: ConfigRoot, relative: object) -> ConfigFile:
    """Where a new configuration file would be written: a .yaml/.yml file that the listing of the configuration files
    would show (no part that starts with '.', not deeper than it looks), whose directory exists under the root also
    after following links, that is not a dashboard file or in the scenario directory next to a configuration, and that
    is not there yet. The absolute path is in the real directory. Raises PathRejected, or SaveRefused (409
    config-changed) when the file is there. Touches the file system (the directory, whether the file is there)."""
    parts = root.parts(relative)
    name = "/".join(parts)
    if not name.lower().endswith(SUFFIXES):
        raise PathRejected("not-a-configuration", f"{name!r} is not a .yaml or .yml file: a configuration file "
                                                  "is one of them")
    if name.lower().endswith(DASHBOARD_SUFFIX):
        raise PathRejected("not-a-configuration", f"{name!r} is the name of a dashboard file (*{DASHBOARD_SUFFIX}), "
                                                  "not of a configuration file: choose another name")
    try:
        directory = (root.root / Path(*parts[:-1])).resolve(strict=True)
    except (OSError, RuntimeError, ValueError):
        directory = None
    if directory is None or not directory.is_dir():
        where = "/".join(parts[:-1])
        raise PathRejected("file-not-found", f"the directory {where!r} of {name!r} does not exist under the root "
                                             f"({root.root}): make it first, or save the file in one that exists")
    if not directory.is_relative_to(root.root):
        raise PathRejected("outside-root", f"{name!r} leads outside the root ({root.root}) through a symbolic link: "
                                           "only files under the root can be written")
    inside = directory.relative_to(root.root).parts
    if any(p.startswith(".") for p in (*parts, *inside)):
        raise PathRejected("not-a-configuration", f"{name!r} has a part that starts with '.', which GET api/configs "
                                                  "leaves out: choose a path that it would list")
    if max(len(parts) - 1, len(inside)) > MAX_DEPTH:
        raise PathRejected("not-a-configuration", f"{name!r} is more than {MAX_DEPTH} directories below the root, "
                                                  "where GET api/configs does not look: choose a shallower path")
    if SCENARIO_DIRECTORY in (parts[-2].lower() if len(parts) > 1 else "", directory.name.lower()) \
            and directory != root.root:
        raise PathRejected("not-a-configuration", f"{name!r} is in a directory named {SCENARIO_DIRECTORY}, where the "
                                                  "scenario files of the configuration next to it are: choose "
                                                  "another directory")
    absolute = directory / parts[-1]
    if os.path.lexists(absolute):
        raise _exists(name)
    return ConfigFile(name, absolute)


def _exists(relative: str) -> SaveRefused:
    return SaveRefused(409, "config-changed", f"{relative} exists already: read it (GET api/config) and save it with "
                                              "its stamp, or choose another name")


def check_unchanged(config: ConfigFile, stamp: str | None, max_bytes: int) -> None:
    """The file is as the screen read it: its bytes have the stamp (only the first max_bytes + 1 are read: a larger
    file is not one that GET api/config gave), or, for a new file (stamp None), it is not there. Raises SaveRefused
    (409 config-changed); an OSError of reading the file is the caller's."""
    if stamp is None:
        if os.path.lexists(config.absolute):
            raise _exists(config.relative)
        return
    with open(config.absolute, "rb") as f:
        current = f.read(max_bytes + 1)
    if stamp_of(current) != stamp:
        raise SaveRefused(409, "config-changed",
                          f"{config.relative} has changed since the screen read it (its content no longer has the "
                          "stamp that was sent): read it again (GET api/config), make the edit on that text and save "
                          "it with the new stamp")


def check_marker(relative: str, data: bytes) -> None:
    """The text must stay a configuration file as the listing tells one (MARKER in its first MARKER_BYTES), so that the
    server writes no other kind of file and the screen can read it again. Raises SaveRefused (422
    config-unreadable)."""
    if MARKER not in data[:MARKER_BYTES]:
        raise SaveRefused(422, "config-unreadable",
                          f"the text for {relative} has no {MARKER.decode()} in its first {MARKER_BYTES} bytes: the "
                          "GUI server saves only a simulation configuration, which has it near its top")


def decide(relative: str, check: dict, command_changes_allowed: bool) -> None:
    """Whether the text that the launcher checked (check --json --text-from-stdin --compare-commands) may be saved.
    Refused: a text the launcher cannot read as a configuration (result unreadable or usage), whatever the options;
    then, unless command changes are allowed, a text whose commands are not "same" ("changed", "not_compared", null:
    what cannot be compared is not saved). The messages are the launcher's findings. Raises SaveRefused."""
    result, commands = check.get("result"), check.get("commands")
    findings = [f for f in check.get("findings", []) if isinstance(f, dict)] \
        if isinstance(check.get("findings"), list) else []
    detail = {"result": result, "commands": commands, "findings": findings}
    if result in ("unreadable", "usage"):
        said = next((f["message"] for f in findings if f.get("severity") == "error"
                     and isinstance(f.get("message"), str)), f"the result is {result!r}")
        raise SaveRefused(422, "config-unreadable",
                          f"the launcher cannot read the text for {relative} as a configuration file ({said}): fix "
                          "the text (detail.findings lists the findings) and save again", detail)
    if commands == "same" or command_changes_allowed:
        return
    said = next((f["message"] for f in findings if f.get("code") in ("commands-changed", "commands-not-compared")
                 and isinstance(f.get("message"), str)), None)
    if said is None:
        said = f"the launcher did not compare the commands that the text runs with {relative} (commands is " \
               f"{json.dumps(commands)})"
    raise SaveRefused(403, "commands-changed",
                      f"the GUI server saves only an edit that keeps the commands that run ({said}): make this edit in "
                      "a text editor, or start the GUI with --allow-command-changes", detail)


def replace_file(target: Path, data: bytes) -> None:
    """Writes data to a file next to target (TEMP_PREFIX, TEMP_SUFFIX) and puts it in target's place, so that a reader
    sees the old content or the new one, never a part. target is a real path (its links followed). An existing
    target keeps its mode, and one that this user may not write is not replaced (as on Windows, where a read-only file
    cannot be); a new one gets the mode that a new file of this user gets. The bytes are written as they are (no line
    end is changed). The file that was written is removed when anything fails. Raises OSError."""
    try:
        mode: int | None = stat.S_IMODE(os.stat(target).st_mode)
    except FileNotFoundError:
        mode = None
    if mode is not None and not os.access(target, os.W_OK):
        raise PermissionError(errno.EACCES, "the file is read-only for this user", str(target))
    temp = target.parent / f"{TEMP_PREFIX}{secrets.token_hex(8)}{TEMP_SUFFIX}"
    # 0o600 while the content is written over an existing file (which may be more private than this user's default);
    # 0o666 of a new file, less the umask, as any new file of this user
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0),
                 0o600 if mode is not None else 0o666)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        if mode is not None:
            os.chmod(temp, mode)
        os.replace(temp, target)
    except BaseException:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass
        raise
