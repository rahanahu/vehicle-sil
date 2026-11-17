# Local knowledge management

How the developers and AI agents of this repository keep working knowledge that does not belong in Git: design deliberation, plans, review records, the state of the work in progress. The settled specification stays in Git (see `AGENTS.md`). This directory holds the rules, the tool that writes shared files safely, and the templates. The knowledge itself is never committed.

Everything lives in `.local/` at the repository root. `.git/info/exclude` excludes it, and a working tree has no copy of it: agents in a working tree use the `.local/` of the main checkout by its absolute path.

## Layout of `.local/`

| Path | Content |
|---|---|
| `CONSTRAINTS.md` | What agents must not do (documents not to open, what must not reach Git, assets not to delete). Only an explicit decision of the user changes it. **Shared file.** |
| `plans/STATE.md` | What spans streams: the list of streams, waiting items for the user, notes about this computer. Short. **Shared file.** |
| `plans/streams/<stream>.md` | One file per stream of work: next steps, waiting items, plans in progress, open items. |
| `plans/RULES.md` | Rules of work: pull requests, verification, models, how to word requests. **Shared file.** |
| `decisions/ACTIVE.md` | Register of the design decisions that are valid now. **Shared file.** |
| `decisions/`, `research/`, `reviews/`, `experiments/`, `plans/` | Deliberation, research, review records, experiment logs, plans: dated documents, kept as records. |
| `changelog/pending.md` | User-visible changes not yet in `CHANGELOG.md`. **Shared file.** |
| `vendor/`, `work/`, `vm/`, `backup/`, `tools/` | Downloads, scratch checkouts, VM images, backups, local scripts. Not knowledge: do not search them. Never open `vendor/` unless a task needs a named file. |

## Rules

**Classify.** A document is a record (what was thought, found or decided at a date) or a current instruction. Only `CONSTRAINTS.md`, the decisions listed in `ACTIVE.md`, and `RULES.md` are current instructions. Everything else is a record: useful, but check it against newer documents and Git before relying on it.

**Find.** Read in this order, and stop when you have what you need: your stream file, then the sections of `RULES.md` that apply, then `ACTIVE.md` for your area, then a record that `ACTIVE.md` or a stream file points to. Search by naming the directories; never pass `.local` as one path (a search without a path finds nothing in it):

```sh
rg -n --glob '*.md' 'PATTERN' .local/decisions .local/plans .local/reviews .local/changelog .local/research
rg -n --glob '*.md' --max-depth 2 'PATTERN' .local/experiments
```

Zero hits do not prove that no decision exists. Try the title words and other directories before you conclude.

**Update.**
- Only the coordinator of a stream writes its stream file and the shared files. A sub-agent reports to its coordinator and writes none of them.
- Write a shared file only with `tools/knowledge/shared_edit.py`: it takes one lock for all shared files, checks that the text to replace is there exactly once, and replaces the file atomically. Read the part you change first. `no-match` means someone changed it after you read it: read it again and rebuild the edit; do not repeat the same one. A conflict between two decisions goes to the user. If the tool cannot lock twice in a row, tell the user; never fall back to writing the file directly.
- Change one item (a line or a section) per call. `python3 tools/knowledge/shared_edit.py --help-full` lists the outcomes and the exit codes (0 changed, 1 not changed, 2 usage error).
- Before a large rewrite of a shared file, copy it to `plans/archive/`. Undo a change with the inverse edit; restore a whole file from a copy only while no other writer is running.
- Do not keep a shared file open in an editor and save it later: the save discards the tool's changes.
- Record external facts by pointer, not by copy: pull requests, CI, counts, commits, and the list of working trees come from `gh` and `git`.

**Expire.**
- The first word of the line `State:` (or the Japanese equivalent) near the top of a record says whether it still applies: superseded, rejected, handed over, done and cancelled end it. Adopted, decided, implemented, in progress, proposal and under review do not decide anything; a record without a state line is unjudged.
- A stream file has `Last updated`. If it is older than 48 hours, check its next steps and waiting items against GitHub and the working trees before you use them.
- An entry of `ACTIVE.md` ends when Git carries the same constraint (then remove the entry) or when a newer decision replaces it (then change the entry).

**Hand over.** `.local/plans/STATE.md` and your stream file are the handover. Before you stop, or before a context clear, update your stream file (next steps, waiting items, where the unfinished work is). Do not append: rewrite the part that changed. Never delete or discard a working tree with uncommitted changes; record where it is and who owns it.

**Stream files.** Make one when a single coordinator works on something across several pull requests or more than two days. A small piece of work that ends in one pull request is not a stream. A finished stream keeps its file with `State: finished` and a one-line result. Copy `templates/stream.md`.

**What goes into Git.** The rules, the tool, the templates and this description, only. Never the contents of `.local/`, the real names of its documents, who works on what, absolute paths, credentials or personal settings. Published documents must stand without the local material.

## Set up

In a new clone, or in an existing one (it never overwrites a file):

```sh
python3 tools/knowledge/init_local.py            # shows what it would create
python3 tools/knowledge/init_local.py --apply    # creates the directories and the missing files from templates/
```

Then fill in the placeholders (`<...>`) of the files it created, and add what your project needs to `CONSTRAINTS.md`.

## Use from Claude Code and Copilot

Agents read `.local/CONSTRAINTS.md` through instructions that are not committed.

- **Claude Code.** `CLAUDE.local.md` (excluded from Git; `templates/CLAUDE.local.md` is a start) imports `@.local/CONSTRAINTS.md` and names the files to read first. In a working tree the import may not load, so the file also names the absolute path to read. Sub-agent definitions in `.claude/agents/` should say to read `CONSTRAINTS.md` by its absolute path; a request to a built-in sub-agent should say so too.
- **Copilot.** It does not read `CLAUDE.local.md`. Put `templates/copilot-instructions.md` (with the absolute path of your clone filled in) into the user-level instructions file of your Copilot product; it is conditional on the repository, so it does not affect other repositories. Whether a given Copilot product reads that file has to be checked in a new chat.
- **Deny rules (Claude Code).** `templates/claude-settings-deny.json` shows `permissions.deny` entries for `.local/vendor/` in `.claude/settings.local.json`, once with a relative and once with a `//`-absolute path. They stop the Read tool and `cat`, not `grep -r` or a script that opens the file. They back up the written constraints; they do not replace them.

## Tests

```sh
python3 -m unittest discover -s tools/knowledge     # also run by scripts/verify-python.py
python3 tools/knowledge/mutations.py                # the tests must catch every mutation of shared_edit.py
```

`shared_edit.py` needs a POSIX system (it locks with `fcntl`). Its lock file is `.local/.shared-edit.lock`; it is never deleted. A leftover `.<name>.<pid>.<random>.tmp` file in a shared file's directory is harmless and may be deleted by hand while no tool is running.
