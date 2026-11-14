"""Compare the screen's messages (the extraction of gui/web) with its translation files, and update the files.

The extraction is Angular's "simple JSON" ({"locale": "ja", "translations": {"<id>": "<source>"}}), written by
`npm run i18n:extract` to build/i18n/messages.json. A translation file is gui/web/src/locale/messages.<lang>.json:

    {"locale": "en", "note": "...", "translations": {"<id>": {"source": "<source>", "target": "<text>" | null}}}

`target: null` marks a message as not translated yet (the screen shows the source text). The check fails on a
message missing from a file, a message not translated yet, a source that differs from the extraction, an empty
target, and a target whose placeholders or ICU cases do not fit the source; it lists obsolete entries (ids no
longer extracted) without failing. Messages not translated yet pass only with allow_untranslated
(verify-web.py --allow-untranslated, for work in progress on one's own machine; CI does not pass it).
scripts/verify-web.py runs it (step i18n) and updates the files (--i18n-update).
"""

from __future__ import annotations

import json
import re
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

PLACEHOLDER = re.compile(r"\{\$[A-Za-z0-9_]+\}")
# A placeholder inside an ICU case ({INTERPOLATION}, {START_BOLD_TEXT}); the ICU header is {VAR_..., kind,
ICU_PLACEHOLDER = re.compile(r"\{([A-Z][A-Z0-9_]*)\}")
ICU_HEADER = re.compile(r"^\{(VAR_[A-Z0-9_]+), (plural|select),")
PLURAL_KEYS = re.compile(r"^(=\d+|zero|one|two|few|many|other)$")


@dataclass
class Report:
    language: str
    total: int = 0
    translated: int = 0
    untranslated: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    obsolete: list[str] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)  # wrong source, empty or broken target

    def passes(self, allow_untranslated: bool = False) -> bool:
        return not self.missing and not self.problems and (allow_untranslated or not self.untranslated)

    @property
    def ok(self) -> bool:
        return self.passes()

    def summary(self) -> str:
        parts = []
        if self.missing:
            parts.append(f"{len(self.missing)} missing")
        if self.problems:
            parts.append(f"{len(self.problems)} wrong")
        parts.append(f"{self.translated} of {self.total} translated")
        if self.untranslated:
            parts.append(f"{len(self.untranslated)} not translated yet")
        if self.obsolete:
            parts.append(f"{len(self.obsolete)} obsolete")
        return f"{self.language}: " + ", ".join(parts)


def short(text: str, limit: int = 60) -> str:
    text = " ".join(text.split())
    return text if len(text) <= limit else text[: limit - 1] + "…"


def read_extraction(path: Path) -> dict[str, str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    messages = data.get("translations") if isinstance(data, dict) else None
    if not isinstance(messages, dict) or not all(isinstance(v, str) for v in messages.values()):
        raise ValueError(f"{path}: not Angular's simple JSON extraction")
    return messages


def read_translations(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("translations"), dict):
        raise ValueError(f'{path}: expected {{"locale": ..., "translations": {{...}}}}')
    return data


def translation_files(locale_dir: Path) -> list[Path]:
    return sorted(locale_dir.glob("messages.*.json"))


def language_of(path: Path) -> str:
    return path.name.removeprefix("messages.").removesuffix(".json")


# --- ICU ------------------------------------------------------------------------------------------


def split_icu(text: str) -> tuple[str, str, dict[str, str]] | None:
    """{VAR, kind, key {message} key {message}} -> (VAR, kind, {key: message}); None if malformed."""
    m = ICU_HEADER.match(text)
    if not m or not text.endswith("}"):
        return None
    body, cases, i = text[m.end() : -1], {}, 0
    while True:
        while i < len(body) and body[i].isspace():
            i += 1
        if i >= len(body):
            break
        start = body.find("{", i)
        if start < 0:
            return None
        key = body[i:start].strip()
        depth, j = 0, start
        while j < len(body):
            depth += {"{": 1, "}": -1}.get(body[j], 0)
            if depth == 0:
                break
            j += 1
        if depth != 0 or not key or key in cases:
            return None
        cases[key] = body[start + 1 : j]
        i = j + 1
    return (m.group(1), m.group(2), cases) if cases else None


def balanced(text: str) -> bool:
    depth = 0
    for ch in text:
        depth += {"{": 1, "}": -1}.get(ch, 0)
        if depth < 0:
            return False
    return depth == 0


def target_problem(source: str, target: str) -> str | None:
    """Why a target does not fit its source (None when it fits)."""
    if not target.strip():
        return "empty target (use null for not translated yet)"
    if not balanced(target):
        return "unbalanced braces"
    if Counter(PLACEHOLDER.findall(source)) != Counter(PLACEHOLDER.findall(target)):
        return (f"placeholders differ: source {sorted(set(PLACEHOLDER.findall(source)))}, "
                f"target {sorted(set(PLACEHOLDER.findall(target)))}")
    src_icu = split_icu(source)
    if src_icu is None:
        return None
    tgt_icu = split_icu(target)
    if tgt_icu is None:
        return "the source is an ICU expression ({VAR_..., plural|select, ...}); the target is not"
    (src_var, src_kind, src_cases), (tgt_var, tgt_kind, tgt_cases) = src_icu, tgt_icu
    if (src_var, src_kind) != (tgt_var, tgt_kind):
        return f"ICU header differs: {{{src_var}, {src_kind}}} in the source"
    if "other" not in tgt_cases:
        return "ICU without an 'other' case"
    if src_kind == "plural":
        bad = [k for k in tgt_cases if not PLURAL_KEYS.match(k)]
        if bad:
            return f"unknown plural cases {bad} (use =N, zero, one, two, few, many, other)"
    elif set(tgt_cases) != set(src_cases):
        return f"select cases differ: source {sorted(src_cases)}, target {sorted(tgt_cases)}"
    known = {name for case in src_cases.values() for name in ICU_PLACEHOLDER.findall(case)}
    unknown = {name for case in tgt_cases.values() for name in ICU_PLACEHOLDER.findall(case)} - known
    if unknown:
        return f"placeholders not in the source: {sorted(unknown)}"
    return None


# --- check and update -----------------------------------------------------------------------------


def compare(extracted: dict[str, str], data: dict, language: str) -> Report:
    report = Report(language, total=len(extracted))
    entries = data["translations"]
    for msg_id, source in extracted.items():
        entry = entries.get(msg_id)
        if entry is None:
            report.missing.append(f"{msg_id} {short(source)!r}")
            continue
        if not isinstance(entry, dict) or "target" not in entry:
            report.problems.append(f'{msg_id}: expected {{"source": ..., "target": ...}}')
            continue
        if entry.get("source") != source:
            report.problems.append(f"{msg_id}: the source differs from the extraction (run --i18n-update)")
            continue
        target = entry["target"]
        if target is None:
            report.untranslated.append(f"{msg_id} {short(source)!r}")
            continue
        problem = target_problem(source, target) if isinstance(target, str) else "target is not a string or null"
        if problem:
            report.problems.append(f"{msg_id} {short(source)!r}: {problem}")
        else:
            report.translated += 1
    for msg_id, entry in entries.items():
        if msg_id not in extracted:
            source = entry.get("source", "") if isinstance(entry, dict) else ""
            report.obsolete.append(f"{msg_id} {short(str(source))!r}")
    return report


def check(extraction: Path, locale_dir: Path) -> list[Report]:
    extracted = read_extraction(extraction)
    return [compare(extracted, read_translations(path), language_of(path)) for path in translation_files(locale_dir)]


def update(extraction: Path, locale_dir: Path) -> list[tuple[str, int, list[str]]]:
    """Rewrite each translation file in the extraction's order: keep the targets of known ids, add new ids as
    not translated yet (target null), drop obsolete ids. Returns (language, added, dropped) per file; dropped lists
    the obsolete entries with their targets, to reuse when a source text was only reworded."""
    extracted = read_extraction(extraction)
    results = []
    for path in translation_files(locale_dir):
        data = read_translations(path)
        old = data["translations"]
        new, added = {}, 0
        for msg_id, source in extracted.items():
            entry = old.get(msg_id)
            target = entry.get("target") if isinstance(entry, dict) else None
            if entry is None:
                added += 1
            new[msg_id] = {"source": source, "target": target}
        dropped = [f"{k} {short(str(v.get('source', '')))!r} -> {v.get('target')!r}" for k, v in old.items()
                   if k not in extracted and isinstance(v, dict)]
        data["translations"] = new
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        results.append((language_of(path), added, dropped))
    return results
