# SPDX-License-Identifier: MIT
"""Tests of scripts/check-docs.py on made-up pages: the pairs of an English page and its Japanese page (both exist,
the same heading levels, the same code blocks, links to the pages and pictures of their own language and to the same
files), and a Japanese page whose English page is not written yet (counted, a problem with --require-english).

    python tests/scripts/test_check_docs.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECK_DOCS = ROOT / "scripts" / "check-docs.py"
failures: list[str] = []

ENGLISH = """# Guide

See [the other page](other.md#other), [the picture](images/shot.png) and the [notes](../README.md).

## Run it

```sh
vehicle_sil_launch check simulation.yaml
```

```yaml
# the engine
rate_hz: 100
```
"""

JAPANESE = """# 手引き

[ほかのページ](other.md#ほか)、[写真](images/shot.png)、[メモ](../../README.ja.md) を見ます。

## 動かす

```sh
vehicle_sil_launch check simulation.yaml
```

```yaml
# エンジン
rate_hz: 100
```
"""


def check(condition: bool, what: str) -> None:
    if not condition:
        failures.append(what)
        print(f"FAILED: {what}")


def write(root: Path, files: dict[str, str]) -> None:
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if name.endswith(".png"):
            path.write_bytes(b"\x89PNG")
        else:
            path.write_text(text, encoding="utf-8", newline="\n")


def run(root: Path, *args: str) -> subprocess.CompletedProcess:
    files = sorted(str(p) for p in root.rglob("*.md"))
    env = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
    return subprocess.run([sys.executable, str(CHECK_DOCS), "--root", str(root), *args, *files], env=env,
                          capture_output=True, text=True, encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL)


def tree(root: Path, english: str | None = ENGLISH, japanese: str | None = JAPANESE, **more: str) -> None:
    files = {
        "README.md": "# Notes\n", "README.ja.md": "# メモ\n",
        "docs/other.md": "# Other\n", "docs/ja/other.md": "# ほか\n",
        "docs/images/shot.png": "", "docs/ja/images/shot.png": "",
    }
    if english is not None:
        files["docs/guide.md"] = english
    if japanese is not None:
        files["docs/ja/guide.md"] = japanese
    files.update(more)
    write(root, files)


def case(name: str, expect_exit: int, expect: str, *args: str, **kwargs) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        tree(root, **kwargs)
        p = run(root, *args)
        out = p.stdout + p.stderr
        check(p.returncode == expect_exit, f"{name}: exit {expect_exit} (got {p.returncode}): {out}")
        check(expect in out, f"{name}: the output says {expect!r}: {out}")


def main() -> int:
    case("a pair that agrees", 0, "check-docs: ok")
    case("a Japanese page without its English page", 0, "1 Japanese pages have no English page yet",
         english=None)
    case("--require-english", 1, "docs/ja/guide.md: no English page docs/guide.md", "--require-english",
         english=None)
    case("an English page without its Japanese page", 1, "docs/guide.md: no Japanese page docs/ja/guide.md",
         japanese=None)
    case("a heading of another level", 1, "heading 2 is of level 3, heading 2 of docs/ja/guide.md",
         english=ENGLISH.replace("## Run it", "### Run it"))
    case("a heading too many", 1, "docs/guide.md: 3 headings, docs/ja/guide.md has 2",
         english=ENGLISH + "\n## More\n")
    case("an ASCII block that differs", 1, "code block 1 differs from the block of docs/ja/guide.md:7",
         english=ENGLISH.replace("check simulation", "run simulation"))
    case("a translated block of another length", 1, "code block 2 has 3 lines",
         english=ENGLISH.replace("# the engine\n", "# the engine\n# more\n"))
    case("a block with another tag", 1, "code block 1 is tagged 'console'",
         english=ENGLISH.replace("```sh", "```console"))
    case("a block too many", 1, "docs/guide.md: 3 code blocks, docs/ja/guide.md has 2",
         english=ENGLISH + "\n```sh\nls\n```\n")
    case("a Japanese page that links an English page", 1,
         "links the English page docs/other.md: link its Japanese page docs/ja/other.md",
         japanese=JAPANESE.replace("(other.md#ほか)", "(../other.md#other)"))
    case("an English page that links a Japanese page", 1,
         "links the Japanese page README.ja.md: link its English page README.md",
         english=ENGLISH.replace("(../README.md)", "(../README.ja.md)"))
    case("a file that only one page links", 1, "links docs/images/shot.png, which docs/guide.md does not",
         english=ENGLISH.replace(", [the picture](images/shot.png)", ""))
    case("an English page that links the Japanese picture", 1,
         "links the Japanese file docs/ja/images/shot.png: link the English one, docs/images/shot.png",
         english=ENGLISH.replace("(images/shot.png)", "(ja/images/shot.png)"))
    case("a Japanese page that links the English picture", 1,
         "links the English file docs/images/shot.png: link the Japanese one, docs/ja/images/shot.png",
         japanese=JAPANESE.replace("(images/shot.png)", "(../images/shot.png)"))
    case("a picture that only the Japanese edition has", 0, "check-docs: ok",
         english=ENGLISH.replace("(images/shot.png)", "(ja/images/only.png)"),
         japanese=JAPANESE.replace("(images/shot.png)", "(images/only.png)"),
         **{"docs/ja/images/only.png": ""})
    case("a README of a directory", 1, "tool/README.md: no Japanese page tool/README.ja.md",
         **{"tool/README.md": "# Tool\n"})
    # An English-only page (the change log) that links an English page not written yet: counted, not a problem.
    case("a link to an English page not written yet", 0, "1 links to them not followed",
         english=None, **{"CHANGELOG.md": "# Changes\n\n- See [the guide](docs/guide.md#run-it).\n"})
    case("the same link with --require-english", 1, "CHANGELOG.md:3: missing file docs/guide.md#run-it",
         "--require-english", english=None,
         **{"CHANGELOG.md": "# Changes\n\n- See [the guide](docs/guide.md#run-it).\n"})
    print(f"test_check_docs: {'FAILED' if failures else 'ok'} - {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
