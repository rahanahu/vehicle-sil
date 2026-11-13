"""The scenario files a configuration may be run with: the .yaml and .yml files in the directory `scenarios` next to
it, and nothing else (docs/scenario.md is the launcher's side).

Only what the listing returns may be given to the launcher (`run --scenario`, `check --scenario`): a request names
one of them by its root-relative path, and it is looked up in a new listing. When that listing is made, the
directory and every directory above it (up to the root) must not be a symbolic link, and neither may the file, so a
link cannot take the name out of the directory. What is not prevented: a file replaced by a link between the listing
and the launcher's reading of it (whoever can write in the root can write the scenario itself as well).
"""

from __future__ import annotations

import os
import stat
from pathlib import Path

from .paths import SUFFIXES, ConfigFile, ConfigRoot, PathRejected

DIRECTORY = "scenarios"
MAX_SCENARIOS = 200  # files the listing returns
MAX_ENTRIES = 5000  # directory entries it looks at
MAX_NAME_CHARS = 255


def directory_of(config: ConfigFile) -> str:
    """The root-relative name of a configuration's scenario directory: a/simulation.yaml -> a/scenarios."""
    parent = config.relative.rpartition("/")[0]
    return f"{parent}/{DIRECTORY}" if parent else DIRECTORY


def _real_directory(root: ConfigRoot, relative: str) -> Path | None:
    """The directory when it exists and neither it nor a directory above it (under the root) is a link."""
    named = root.root / Path(*relative.split("/"))
    try:
        if named.resolve(strict=True) != named or not named.is_dir():
            return None
    except (OSError, RuntimeError, ValueError):
        return None
    return named


def list_scenarios(root: ConfigRoot, config: ConfigFile) -> tuple[str, bool, list[dict], bool]:
    """(the directory's root-relative name, whether it is there, [{path, name, size}], truncated). `name` is the
    file name; `path` is root-relative, as a request gives it back."""
    relative = directory_of(config)
    directory = _real_directory(root, relative)
    if directory is None:
        return relative, False, [], False
    files: list[dict] = []
    truncated = False
    try:
        entries = sorted(os.scandir(directory), key=lambda e: e.name)
    except OSError:
        return relative, True, [], False
    for seen, entry in enumerate(entries, start=1):
        if seen > MAX_ENTRIES or len(files) >= MAX_SCENARIOS:
            truncated = True
            break
        name = entry.name
        if name.startswith(".") or not name.lower().endswith(SUFFIXES) or len(name) > MAX_NAME_CHARS:
            continue
        try:
            name.encode("utf-8")
            info = entry.stat(follow_symlinks=False)
        except (OSError, UnicodeEncodeError):
            continue
        if not stat.S_ISREG(info.st_mode):  # a link, a directory, a device
            continue
        path = f"{relative}/{name}"
        try:
            root.parts(path)  # the same checks as any path from the screen (no ':', no '\\', length)
        except PathRejected:
            continue
        files.append({"path": path, "name": name, "size": info.st_size})
    return relative, True, files, truncated


def resolve_scenario(root: ConfigRoot, config: ConfigFile, wanted: object) -> str | None:
    """The scenario to give the launcher: None for none (null or left out), else `wanted` when the listing of the
    configuration's scenario directory has it. Raises PathRejected with what to send instead."""
    if wanted is None:
        return None
    try:  # the shape: a non-empty root-relative path, no '..', not absolute, no NUL, not too long
        root.parts(wanted)
    except PathRejected as e:
        raise PathRejected(e.code, f"scenario must be null or a path as GET api/scenarios?file={config.relative} "
                                   f"lists it ({e})") from None
    relative, found, files, _ = list_scenarios(root, config)
    if any(f["path"] == wanted for f in files):
        return wanted
    if not found:
        raise PathRejected("scenario-not-listed", f"{config.relative} has no directory {relative} (or it is a "
                                                  "symbolic link): put the scenario files in a directory "
                                                  f"{relative} of its own, next to the configuration")
    raise PathRejected("scenario-not-listed", f"{wanted!r} is not a scenario of {config.relative}: choose one "
                                              f"that GET api/scenarios?file={config.relative} lists (the .yaml "
                                              f"and .yml files in {relative}, not links)")
