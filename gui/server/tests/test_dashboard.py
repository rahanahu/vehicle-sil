"""The dashboard file (<configuration>.dashboard.yaml): its reading, its shape errors with their lines, and the
panel modules the server serves (only with the panel key, only from a panels directory under the root)."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

import pytest
from yarl import URL
from conftest import CONFIGS, REPOSITORY, Client, gui_server, run_async
from vehicle_sil_gui import dashboard
from vehicle_sil_gui.paths import ConfigRoot, PathRejected

EXAMPLE = REPOSITORY / "examples" / "ecu_main"
SYMLINKS = sys.platform != "win32"


def read(root: Path, text: str | bytes, config: str = "simulation.yaml") -> dashboard.Result:
    """Writes the dashboard file of `config` (which must exist under root) and reads it."""
    path = root / dashboard.dashboard_path(ConfigRoot(root).resolve(config))
    path.write_bytes(text if isinstance(text, bytes) else text.encode())
    r = ConfigRoot(root)
    return dashboard.read_dashboard(r, r.resolve(config), "panels/KEY/")


@pytest.fixture
def root(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    root.mkdir()
    shutil.copy(CONFIGS / "simulation.yaml", root / "simulation.yaml")
    shutil.copytree(EXAMPLE / "panels", root / "panels")
    return root


def errors(result: dashboard.Result) -> list[tuple[int, str]]:
    return [(e.line, e.message) for e in result.errors]


def test_the_example_is_read(root: Path) -> None:
    result = read(root, (EXAMPLE / "simulation.dashboard.yaml").read_text())
    assert result.found and not result.errors, errors(result)
    d = result.dashboard
    assert d["title"] == "Engine and dashboard"
    assert d["panels"] == [{"module": "panels/rpm-bar.js", "url": "panels/KEY/panels/rpm-bar.js"}]
    engine = d["sections"][0]["items"]
    assert engine[0] == {"variable": {"peer": "Engine", "name": "engine.throttle"}, "widget": "slider",
                         "label": "Throttle", "options": {"step": 5}}
    assert engine[2]["panel"] == "rpm-bar" and engine[2]["variables"] == [{"peer": "Engine", "name": "engine.speed"}]
    assert engine[3] == {"auto": "Engine"}


def test_no_file_is_not_an_error(root: Path) -> None:
    r = ConfigRoot(root)
    result = dashboard.read_dashboard(r, r.resolve("simulation.yaml"), "panels/KEY/")
    assert (result.found, result.errors, result.dashboard, result.path) == (False, [], None,
                                                                            "simulation.dashboard.yaml")


def test_the_url_of_a_module_is_encoded(root: Path) -> None:
    (root / "a#b?c%d").mkdir()
    shutil.copy(root / "simulation.yaml", root / "a#b?c%d" / "sim.yaml")
    shutil.copytree(root / "panels", root / "a#b?c%d" / "panels")
    result = read(root, "vehicle_sil_dashboard: 1\npanels: [panels/rpm-bar.js]\nsections: []\n", "a#b?c%d/sim.yaml")
    assert result.dashboard["panels"][0]["url"] == "panels/KEY/a%23b%3Fc%25d/panels/rpm-bar.js"


def test_a_configuration_in_a_directory(root: Path) -> None:
    (root / "sub").mkdir()
    shutil.copy(root / "simulation.yaml", root / "sub" / "bench.yml")
    shutil.copytree(root / "panels", root / "sub" / "panels")
    result = read(root, "vehicle_sil_dashboard: 1\npanels: [panels/rpm-bar.js]\nsections: []\n", "sub/bench.yml")
    assert result.path == "sub/bench.dashboard.yaml" and not result.errors
    assert result.dashboard["panels"][0]["url"] == "panels/KEY/sub/panels/rpm-bar.js"


@pytest.mark.parametrize("text, line, words", [
    ("sections: []\n", 1, "has no vehicle_sil_dashboard"),
    ("vehicle_sil_dashboard: 2\nsections: []\n", 1, "reads format 1"),
    ("vehicle_sil_dashboard: 1\nsections: []\ncolour: red\n", 3, "unknown key 'colour'"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - variable: speed\n", 5,
     "is not <participant>.<variable>"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - variable: E.x\n        auto: E\n", 5,
     "exactly one of variable"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - variable: E.x\n        widjet: graph\n",
     6, "unknown key 'widjet'"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    collapsed: yes please\n    items: []\n", 4,
     "true or false"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    title: B\n    items: []\n", 4, "given twice"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: 12\n    items: []\n", 3, "put it in quotes"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - panel: gauge\n", 5,
     "not a custom element name"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - panel: my-gauge\n", 5,
     "list the module that defines it under panels:"),
    ("vehicle_sil_dashboard: 1\npanels: [panels/missing.js]\nsections: []\n", 2, "does not exist"),
    ("vehicle_sil_dashboard: 1\npanels: [../panels/rpm-bar.js]\nsections: []\n", 2, "is not a path in panels/"),
    ("vehicle_sil_dashboard: 1\npanels: [panels/../simulation.yaml]\nsections: []\n", 2, "is not a path in panels/"),
    ("vehicle_sil_dashboard: 1\npanels: ['panels/c:x.js']\nsections: []\n", 2, "is not a path in panels/"),
    ("vehicle_sil_dashboard: 1\npanels: [panels/readme.txt]\nsections: []\n", 2, "not a .js or .mjs file"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - variable: E.x\n        options:\n"
     "          step: .inf\n", 7, "not a finite number"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - variable: E.x\n        options:\n"
     "          when: 2026-01-01\n", 7, "put it in quotes"),
    ("vehicle_sil_dashboard: 1\nsections: [\n", 3, "not YAML"),
    ("vehicle_sil_dashboard: 1\nsections: []\n---\nmore: 1\n", 3, "a second document"),
    ("vehicle_sil_dashboard: 1\ntitle: !!python/object:os.system x\nsections: []\n", 2, "write a plain value"),
    ("", 1, "the file is empty"),
    ("vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - variable: E.x\n        widget: Graph!\n",
     6, "is not the name of a widget"),
    # an integer of thousands of digits (Python refuses to convert it) and any long value: an error, not a 500
    ("vehicle_sil_dashboard: " + "9" * 5000 + "\nsections: []\n", 1, "longer than 1024 characters"),
    ("vehicle_sil_dashboard: 1\ntitle: " + "x" * 1100 + "\nsections: []\n", 2, "longer than 1024 characters"),
    ("- a\n- b\n", 1, "must be a mapping"),
])
def test_mistakes_say_the_line_and_what_to_do(root: Path, text: str, line: int, words: str) -> None:
    (root / "panels" / "readme.txt").write_text("x")
    result = read(root, text)
    assert result.found and result.dashboard is None
    assert any(e.line == line and words in e.message for e in result.errors), errors(result)


def test_every_mistake_is_listed(root: Path) -> None:
    result = read(root, "vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n      - variable: x\n"
                        "      - variable: E.y\n        colour: red\n  - items: []\n")
    assert [line for line, _ in errors(result)] == [5, 7, 8]
    assert result.errors[0].at == "sections[0].items[0].variable"


def test_the_reading_is_bounded(root: Path) -> None:
    # Aliases (a few lines can expand to billions of values) are refused, not expanded.
    bomb = "a: &a [x, x, x, x, x, x, x, x, x]\n" + "".join(
        f"{chr(98 + i)}: &{chr(98 + i)} [*{chr(97 + i)}, *{chr(97 + i)}, *{chr(97 + i)}, *{chr(97 + i)}]\n"
        for i in range(10))
    assert "aliases are not used" in read(root, "vehicle_sil_dashboard: 1\n" + bomb).errors[0].message
    deep = "vehicle_sil_dashboard: 1\nsections: " + "[" * 5000 + "]" * 5000 + "\n"
    assert "nested more than" in read(root, deep).errors[0].message
    many = "vehicle_sil_dashboard: 1\nsections: [" + "1," * 60_000 + "1]\n"
    assert "more than 50000 values" in read(root, many).errors[0].message
    large = "vehicle_sil_dashboard: 1\n" + "# " + "x" * dashboard.MAX_FILE_BYTES + "\nsections: []\n"
    assert "larger than 256 KiB" in read(root, large).errors[0].message
    assert "not UTF-8" in read(root, b"vehicle_sil_dashboard: 1\ntitle: \xff\nsections: []\n").errors[0].message
    items = "".join(f"      - variable: E.v{i}\n" for i in range(dashboard.MAX_ITEMS + 1))
    result = read(root, f"vehicle_sil_dashboard: 1\nsections:\n  - title: A\n    items:\n{items}")
    assert "at most 2000" in result.errors[0].message


@pytest.mark.skipif(not SYMLINKS, reason="symbolic links")
def test_links_out_of_the_root_are_not_read(tmp_path: Path, root: Path) -> None:
    secret = tmp_path / "secret.yaml"
    secret.write_text("vehicle_sil_dashboard: 1\nsections: []\ntitle: secret\n")
    os.symlink(secret, root / "simulation.dashboard.yaml")
    r = ConfigRoot(root)
    result = dashboard.read_dashboard(r, r.resolve("simulation.yaml"), "panels/KEY/")
    assert result.found and result.dashboard is None and "outside the root" in result.errors[0].message


def test_module_paths(tmp_path: Path, root: Path) -> None:
    r = ConfigRoot(root)
    assert dashboard.module_path(r, "panels/rpm-bar.js") == (root / "panels" / "rpm-bar.js").resolve()
    (root / "panels" / "lib").mkdir()
    (root / "panels" / "lib" / "util.mjs").write_text("export const x = 1;\n")
    assert dashboard.module_path(r, "panels/lib/util.mjs").name == "util.mjs"
    (root / "panels" / ".hidden.js").write_text("x")
    (root / "panels" / "notes.txt").write_text("x")
    (root / "loose.js").write_text("x")
    (root / "panels" / "big.js").write_bytes(b"x" * (dashboard.MAX_MODULE_BYTES + 1))
    refused = ["../secret.js", "panels/../loose.js", "loose.js", "panels/.hidden.js", "panels/notes.txt",
               "panels/big.js", "panels/missing.js", "/etc/passwd.js", "C:/x/panels/a.js", "panels/a:b.js",
               "panels\\rpm-bar.js", "panels/rpm-bar.js\x00.txt", "panels/" + "x" * 2000 + ".js", "", None]
    if SYMLINKS:
        outside = tmp_path / "outside.js"
        outside.write_text("x")
        os.symlink(outside, root / "panels" / "out.js")
        os.symlink(root / "loose.js", root / "panels" / "up.js")  # in the root, but out of panels/
        os.symlink(tmp_path, root / "other")
        (tmp_path / "panels").mkdir(exist_ok=True)
        (tmp_path / "panels" / "a.js").write_text("x")
        os.symlink(root / "panels" / "notes.txt", root / "panels" / "notes.js")
        os.symlink(root / "panels" / "rpm-bar.js", root / "panels" / "code.txt")  # the name asked for counts too
        refused += ["panels/out.js", "panels/up.js", "other/panels/a.js", "panels/notes.js", "panels/code.txt"]
        # a panels directory that is a link, even to a directory in the root
        (root / "real").mkdir()
        (root / "real" / "x.js").write_text("x")
        (root / "sub").mkdir()
        os.symlink(root / "real", root / "sub" / "panels")
        os.symlink(root / "sub", root / "linked")
        (root / "sub" / "own").mkdir()
        (root / "sub" / "own" / "panels").mkdir()
        (root / "sub" / "own" / "panels" / "y.js").write_text("y")
        refused += ["sub/panels/x.js", "linked/own/panels/y.js"]
        assert dashboard.module_path(r, "sub/own/panels/y.js").name == "y.js"
    for path in refused:
        with pytest.raises(PathRejected):
            dashboard.module_path(r, path)


def test_the_server_serves_the_file_and_its_panels(tmp_path: Path, root: Path) -> None:
    shutil.copy(EXAMPLE / "simulation.dashboard.yaml", root / "simulation.dashboard.yaml")
    (root / "panels" / "notes.txt").write_text("not served")
    with gui_server(tmp_path, root=root) as gui:
        async def main(c: Client) -> None:
            status, body = await c.get("api/dashboard?file=simulation.yaml")
            assert status == 200 and body["found"] and not body["errors"], body
            url = body["dashboard"]["panels"][0]["url"]
            key = url.split("/")[1]
            assert url == f"panels/{key}/panels/rpm-bar.js" and key not in gui.token and gui.token not in url
            # The module: JavaScript, never sniffed, the same CSP; no token needed (an import has no header)
            async with c.http.get(f"{gui.base}/{url}") as r:
                assert r.status == 200
                assert r.headers["Content-Type"] == "text/javascript; charset=utf-8"
                assert r.headers["X-Content-Type-Options"] == "nosniff"
                assert r.headers["Cross-Origin-Resource-Policy"] == "same-origin"  # no <script src> from elsewhere
                assert r.headers["Cache-Control"] == "no-store"
                assert "script-src 'self'" in r.headers["Content-Security-Policy"]
                assert await r.read() == (root / "panels" / "rpm-bar.js").read_bytes()
            # Not without the key, not with a wrong one, not other files, not out of panels/
            for path in (f"panels/{key}x/panels/rpm-bar.js", "panels/wrong/panels/rpm-bar.js",
                         "panels/panels/rpm-bar.js", f"panels/{key}/panels/notes.txt",
                         f"panels/{key}/simulation.yaml", f"panels/{key}/panels/%2e%2e/simulation.yaml",
                         f"panels/{key}/..%2fsimulation.yaml", f"panels/{key}/panels/rpm-bar.js%00.txt", "panels"):
                async with c.http.get(URL(f"{gui.base}/{path}", encoded=True)) as r:
                    assert r.status == 404, path
                    assert "customElements" not in await r.text()
            # Other pages and other hosts: refused before the key is looked at
            async with c.http.get(f"{gui.base}/{url}", headers={"Origin": "http://evil.example"}) as r:
                assert r.status == 403
            async with c.http.get(f"{gui.base}/{url}", headers={"Host": "evil.example"}) as r:
                assert r.status == 403
            # The file of a configuration: the token, and the same path checks as api/check
            async with c.http.get(f"{gui.base}/api/dashboard?file=simulation.yaml") as r:
                assert r.status == 401
            for file, code in (("../x.yaml", "outside-root"), ("missing.yaml", "file-not-found"),
                               ("panels/rpm-bar.js", "not-a-configuration"), ("", "invalid-path")):
                status, body = await c.get(f"api/dashboard?file={file}")
                assert body["error"]["code"] == code, (file, body)
            status, body = await c.get("api/dashboard")
            assert body["error"]["code"] == "invalid-path"
        run_async(main, gui)


def test_a_wrong_url_under_panels_is_not_the_screen(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        async def main(c: Client) -> None:
            async with c.http.get(f"{gui.base}/panels/x") as r:
                assert r.status == 404
        run_async(main, gui)


def test_the_panel_key_changes_with_each_start(tmp_path: Path, root: Path) -> None:
    shutil.copy(EXAMPLE / "simulation.dashboard.yaml", root / "simulation.dashboard.yaml")
    keys = []
    for _ in range(2):
        with gui_server(tmp_path, root=root) as gui:
            async def main(c: Client) -> str:
                status, body = await c.get("api/dashboard?file=simulation.yaml")
                return body["dashboard"]["panels"][0]["url"].split("/")[1]
            keys.append(run_async(main, gui))
    assert keys[0] != keys[1]
