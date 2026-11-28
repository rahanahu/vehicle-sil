"""The timelines of scenario runs: the files the launcher writes for this server (run --scenario --timeline), and
the page that draws them (gui/timeline).

Each scenario run started with a launcher that has the feature `timeline` writes its file into one temporary
directory of this server. The files of the last KEPT runs that wrote one are kept (the oldest is removed when
another run's file is ready); the directory is removed when the server ends.
"""

from __future__ import annotations

import logging
import os
import re
import secrets
import shutil
import tempfile
import threading
from collections import OrderedDict
from pathlib import Path

KEPT = 4  # the timeline files kept: of the last runs that wrote one
PAGE_PATH = "timeline/"  # where the page is served (relative to the server's URL)
# The page's files, by the name they are served under (the page itself also as the directory).
# vehicle-sil-timeline.js is the element <vehicle-sil-timeline> in one file: screens of this server (--web, custom
# panels) load it from timeline/vehicle-sil-timeline.js too.
PAGE_FILES = {
    "": ("timeline.html", "text/html; charset=utf-8"),
    "timeline.html": ("timeline.html", "text/html; charset=utf-8"),
    "vehicle-sil-timeline.js": ("vehicle-sil-timeline.js", "text/javascript; charset=utf-8"),
    "page.js": ("page.js", "text/javascript; charset=utf-8"),
}
MAX_PAGE_FILE_BYTES = 4 * 1024 * 1024

log = logging.getLogger("vehicle_sil_gui")


def api_path(run_id: str) -> str:
    """Where the file of a run is served (relative to the server's URL; needs the token)."""
    return f"api/timeline/{run_id}"


def written_whole(result: object) -> bool:
    """Whether the launcher's run result says that it wrote the whole timeline file: there is a result, and it has
    no timeline-write finding (a file written only in part)."""
    if not isinstance(result, dict):
        return False
    findings = result.get("findings")
    return not (isinstance(findings, list) and any(
        isinstance(f, dict) and f.get("code") == "timeline-write" for f in findings))


def find_page_dir() -> Path | None:
    """The page packaged with this module (vehicle_sil_gui/timeline), else gui/timeline of the repository, when this
    module runs from the repository's source tree."""
    here = Path(__file__).resolve().parent
    for candidate in (here / "timeline", here.parents[2] / "timeline"):
        if all((candidate / name).is_file() for name, _ in PAGE_FILES.values()):
            return candidate
    return None


def read_page_file(directory: Path, name: str) -> bytes | None:
    """A file of the page (a name of PAGE_FILES), or None when it cannot be read."""
    try:
        with open(directory / name, "rb") as f:
            body = f.read(MAX_PAGE_FILE_BYTES + 1)
    except OSError:
        return None
    return body if len(body) <= MAX_PAGE_FILE_BYTES else None


class Timelines:
    """The temporary directory and the files in it. Its methods touch the disk: call them off the event loop."""

    def __init__(self, kept: int = KEPT) -> None:
        if kept < 1:
            raise ValueError("kept must be 1 or more")
        self._lock = threading.Lock()
        self._closed = False
        self.kept = kept
        self.directory: Path | None = None
        self.ready: OrderedDict[str, tuple[Path, str]] = OrderedDict()  # run_id -> (file, name), oldest first
        self._failed: set[str] = set()

    def new_file(self) -> Path:
        """A name for the next run's file (the launcher creates the file). Makes the directory on first use."""
        with self._lock:
            if self._closed:
                raise RuntimeError("timelines are closed")
            if self.directory is None:
                self.directory = Path(tempfile.mkdtemp(prefix="vehicle-sil-gui-timelines-"))
            return self.directory / f"{secrets.token_hex(8)}.timeline.json"

    def finish(self, run_id: str, path: Path, usable: bool, scenario: str, *, failed: bool = False) -> bool:
        """The run has ended: keeps its file when the launcher wrote a whole one (usable, and the file is not
        empty), and removes older files beyond `kept` (evicting passing runs before failed ones); otherwise removes
        it. Whether it is kept. scenario: the scenario file, whose name the file is given when it is served
        (<name>.timeline.json)."""
        with self._lock:
            try:
                kept = usable and path.stat().st_size > 0
            except OSError:
                kept = False
            if not kept:
                _remove(path)
                return False
            stem = re.sub(r"[^A-Za-z0-9._-]", "_", Path(scenario.replace("\\", "/")).stem) or "scenario"
            key = str(run_id)
            prev = self.ready.get(key)
            if prev is not None and prev[0] != path:
                _remove(prev[0])
            self.ready[key] = (path, f"{stem}.timeline.json")
            self.ready.move_to_end(key)
            if failed:
                self._failed.add(key)
            else:
                self._failed.discard(key)
            while len(self.ready) > self.kept:
                # Prefer keeping failed runs when evicting; never evict the run that just finished.
                victim = next((rid for rid in self.ready if rid != key and rid not in self._failed), None)
                if victim is None:
                    victim = next(iter(self.ready))
                old, _ = self.ready.pop(victim)
                self._failed.discard(victim)
                _remove(old)
            return True

    def file_of(self, run_id: str) -> tuple[Path, str] | None:
        """(the file, the name it is served under) of a run whose file is kept."""
        with self._lock:
            found = self.ready.get(str(run_id))
            return found if found is not None and found[0].is_file() else None

    def remove_all(self) -> None:
        with self._lock:
            self._closed = True
            self.ready.clear()
            self._failed.clear()
            if self.directory is not None:
                shutil.rmtree(self.directory, ignore_errors=True)
                if self.directory.exists():
                    log.warning("the timeline files in %s could not all be removed: remove the directory by hand",
                                self.directory)
                self.directory = None


def _remove(path: Path) -> None:
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    except OSError as e:  # held open by another program (Windows): it goes with the directory at the end
        log.warning("the timeline file %s could not be removed now (%s): it is removed when the server ends",
                    path, e.strerror)
