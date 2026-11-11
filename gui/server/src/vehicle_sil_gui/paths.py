"""The configuration files the screen may choose: the .yaml and .yml files under the root, and nothing else."""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

SUFFIXES = (".yaml", ".yml")
MAX_DEPTH = 6  # directories below the root that the listing enters
MAX_FILES = 1000  # files the listing returns
MAX_ENTRIES = 50_000  # directory entries the listing looks at, in all
MAX_PATH_CHARS = 1024
MARKER = b"vehicle_sil_simulation"  # a simulation configuration has this key near its top
MARKER_BYTES = 4096


class PathRejected(ValueError):
    """The path is not one of the configuration files under the root. str() says why and what to do."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class ConfigFile:
    relative: str  # with "/", relative to the root: what the screen sees and what the launcher is given
    absolute: Path  # resolved


class ConfigRoot:
    def __init__(self, root: Path):
        self.root = root.resolve(strict=True)
        if not self.root.is_dir():
            raise NotADirectoryError(f"{root} is not a directory")

    def resolve(self, relative: object) -> ConfigFile:
        """Checks a path from the screen. Raises PathRejected unless it names a .yaml/.yml file under the root,
        also after following symbolic links."""
        if not isinstance(relative, str) or not relative:
            raise PathRejected("invalid-path", "file must be a non-empty string: a path relative to the root, "
                                               "as GET api/configs lists it")
        if len(relative) > MAX_PATH_CHARS or "\x00" in relative or "\\" in relative:
            raise PathRejected("invalid-path", "file must be a path relative to the root, with / between the "
                                               "parts (no backslash, no NUL)")
        if relative.startswith("/") or re.match(r"^[A-Za-z]:", relative):
            raise PathRejected("outside-root", f"{relative!r} is absolute: write it relative to the root "
                                               f"({self.root})")
        parts = relative.split("/")
        if any(p in ("", ".", "..") for p in parts):
            raise PathRejected("outside-root", f"{relative!r} has an empty, '.' or '..' part: write the path "
                                               "as GET api/configs lists it")
        if not relative.lower().endswith(SUFFIXES):
            raise PathRejected("not-a-configuration", f"{relative!r} is not a .yaml or .yml file")
        try:
            absolute = (self.root / Path(*parts)).resolve(strict=True)
        except (OSError, RuntimeError):
            raise PathRejected("file-not-found", f"{relative!r} does not exist under the root ({self.root}): "
                                                 "choose one that GET api/configs lists") from None
        if not absolute.is_relative_to(self.root):
            raise PathRejected("outside-root", f"{relative!r} leads outside the root ({self.root}) through a "
                                               "symbolic link: only files under the root can be used")
        if not absolute.name.lower().endswith(SUFFIXES):
            raise PathRejected("not-a-configuration", f"{relative!r} is a link to {absolute.name}, which is not "
                                                      "a .yaml or .yml file")
        if not absolute.is_file():
            raise PathRejected("not-a-configuration", f"{relative!r} is not a regular file")
        return ConfigFile("/".join(parts), absolute)

    def relative(self, path: Path) -> str:
        """The root-relative name of a file under the root (raises PathRejected otherwise)."""
        absolute = path.resolve(strict=True)
        if not absolute.is_relative_to(self.root):
            raise PathRejected("outside-root", f"{path} is not under the root ({self.root})")
        return self.resolve(absolute.relative_to(self.root).as_posix()).relative

    def list(self) -> tuple[list[dict], bool]:
        """([{path, modified, size, simulation}], truncated). Skips directories whose name starts with '.',
        and does not follow links to directories."""
        files: list[dict] = []
        seen = 0
        truncated = False
        stack: list[tuple[Path, int]] = [(self.root, 0)]
        while stack:
            directory, depth = stack.pop()
            try:
                entries = sorted(os.scandir(directory), key=lambda e: e.name)
            except OSError:
                continue
            for entry in entries:
                seen += 1
                if seen > MAX_ENTRIES or len(files) >= MAX_FILES:
                    truncated = True
                    break
                if entry.name.startswith("."):
                    continue
                try:
                    if entry.is_dir(follow_symlinks=False):
                        if depth < MAX_DEPTH:
                            stack.append((Path(entry.path), depth + 1))
                        else:
                            truncated = True
                        continue
                    if not entry.name.lower().endswith(SUFFIXES):
                        continue
                    found = self.resolve(Path(entry.path).relative_to(self.root).as_posix())
                    stat = found.absolute.stat()
                    with open(found.absolute, "rb") as f:
                        head = f.read(MARKER_BYTES)
                except (OSError, PathRejected):
                    continue
                files.append({
                    "path": found.relative,
                    "modified": datetime.fromtimestamp(stat.st_mtime, UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
                    "size": stat.st_size,
                    "simulation": MARKER in head,
                })
            if truncated:
                break
        files.sort(key=lambda f: f["path"])
        return files, truncated
