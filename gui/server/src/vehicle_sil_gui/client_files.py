"""The client library and the parts for a screen of your own: the two files that `npm run build:client` writes
(gui/web), served under vsil/ so that a screen without a build step needs no copy of them.

The names are the files' own names: the bundle of the parts imports ./vehicle-sil-gui-client.mjs, so a page that
loads both from vsil/ gets one module (and one connection) for the library.
"""

from __future__ import annotations

from pathlib import Path

PATH = "vsil/"  # where the files are served (relative to the server's URL)
JS = "text/javascript; charset=utf-8"
# The only files served under vsil/, by the name they are asked for (the same as the file's name).
FILES = {
    "vehicle-sil-gui-client.mjs": JS,
    "vehicle-sil-gui-elements.mjs": JS,
}
BUILD_COMMAND = "npm ci && npm run build:client"  # in gui/web


def candidates() -> tuple[Path, Path]:
    """Where the files are looked for: packaged with this module (vehicle_sil_gui/client), else build/gui-client of
    the repository, when this module runs from the repository's source tree."""
    here = Path(__file__).resolve().parent
    return here / "client", here.parents[3] / "build" / "gui-client"


def find_dir() -> Path | None:
    """The directory of candidates() that has both files, or None."""
    for candidate in candidates():
        if all((candidate / name).is_file() for name in FILES):
            return candidate
    return None


def missing_message(*, where: bool = False) -> str:
    """What to do when find_dir() is None. `where` adds the two directories that were searched: for the server's own
    log only, never for an answer over HTTP (vsil/ needs no token, and the paths name this computer's directories)."""
    files = f"the client library and the parts ({' and '.join(FILES)})"
    found = f"are in neither {' nor '.join(map(str, candidates()))}" if where else "are not in this GUI server"
    return (f"{files} {found}: in the repository, run `{BUILD_COMMAND}` in gui/web; in the tools archive, unpack "
            "it again")
