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
BUILD_COMMAND = "npm run build:client"  # in gui/web


def find_dir() -> Path | None:
    """The files packaged with this module (vehicle_sil_gui/client), else build/gui-client of the repository, when
    this module runs from the repository's source tree."""
    here = Path(__file__).resolve().parent
    for candidate in (here / "client", here.parents[3] / "build" / "gui-client"):
        if all((candidate / name).is_file() for name in FILES):
            return candidate
    return None


def missing_message() -> str:
    """What to do when find_dir() is None."""
    return (f"the client library and the parts ({' and '.join(FILES)}) are not in this installation of the GUI "
            f"server: build them with `{BUILD_COMMAND}` in gui/web of the repository and start the server again, "
            "or install the GUI from the tools archive")
