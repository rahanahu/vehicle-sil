"""The saved descriptions of the configuration files: what each executable's --describe wrote, which the launcher
saves (check --write-descriptions) so that a check of an unsaved text (check --text-from-stdin --descriptions)
starts no process.

One file per (configuration file, variant), in one temporary directory of this server, with the configuration
file's stamp when its check wrote it. The browser never names these files, and their paths are not in any answer.
The directory is removed when the server ends.
"""

from __future__ import annotations

import logging
import secrets
import shutil
import tempfile
from pathlib import Path

log = logging.getLogger("vehicle_sil_gui")

KEPT = 256  # the (file, variant) whose descriptions are kept; the oldest goes first

Key = tuple[str, "str | None"]  # (root-relative configuration file, variant)


class Descriptions:
    """path_of and remove_all make and remove the directory: call them off the event loop."""

    def __init__(self) -> None:
        self.directory: Path | None = None
        self._files: dict[Key, Path] = {}
        self._stamps: dict[Key, tuple] = {}  # the stamp of the file when its descriptions were last made

    def path_of(self, key: Key) -> Path:
        """Where the descriptions of (file, variant) are written and read. Makes the directory on first use; the file
        is the launcher's to write (it may not be there)."""
        if self.directory is None:
            self.directory = Path(tempfile.mkdtemp(prefix="vehicle-sil-gui-descriptions-"))
        path = self._files.pop(key, None) or self._new_path()
        self._files[key] = path  # the newest
        while len(self._files) > KEPT:
            old_key = next(iter(self._files))
            self._stamps.pop(old_key, None)
            _remove(self._files.pop(old_key))
        return path

    def made_from(self, key: Key) -> tuple | None:
        """The stamp of the file when its descriptions were last made (None: never, or they were dropped)."""
        return self._stamps.get(key) if key in self._files else None

    def made(self, key: Key, stamp: tuple, usable: bool) -> None:
        """The check that writes the descriptions has ended (stamp: of the file before it). usable: it read the
        file; otherwise the key gets a new path where no file is, so that the text is checked with no description
        even when what an earlier check wrote cannot be removed (Windows: another program has it open; it goes with
        the directory at the end)."""
        self._stamps[key] = stamp
        if not usable and key in self._files:
            _remove(self._files[key])
            self._files[key] = self._new_path()

    def forget(self, file: str) -> None:
        """The file was saved (POST api/config): its descriptions, of every variant, are made again before the next
        check of a text, also where its stamp would not change (a coarse modification time, the same size)."""
        for key in [k for k in self._stamps if k[0] == file]:
            del self._stamps[key]

    def _new_path(self) -> Path:
        assert self.directory is not None
        return self.directory / f"{secrets.token_hex(8)}.json"

    def remove_all(self) -> None:
        self._files.clear()
        self._stamps.clear()
        if self.directory is not None:
            shutil.rmtree(self.directory, ignore_errors=True)
            if self.directory.exists():
                log.warning("the saved descriptions in %s could not all be removed: remove the directory by hand",
                            self.directory)
            self.directory = None


def _remove(path: Path) -> None:
    try:
        path.unlink(missing_ok=True)
    except OSError:  # held open by another program (Windows): it goes with the directory at the end
        pass
