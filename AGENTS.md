# AGENTS.md

Instructions for developers and AI agents who work on vehicle-sil.

## Documentation

Keep the settled specification in Git and working material locally only. The authoritative specification is the code, tests and documentation tracked in Git.

### What goes into Git

- `README.md`, `AGENTS.md`
- These documents in `docs/`:
  - Build and development procedures
  - API references and user manuals
  - The settled architecture and the specifications of externally exposed interfaces
  - The design contracts and constraints that the implementation must keep

Write the settled specification concisely. Do not copy the deliberation into it.

### What stays local

Put the material below in `.local/` at the repository root and do not commit it. `.git/info/exclude` excludes `.local/`.

| Directory | Contents |
|---|---|
| `.local/research/` | Technical research |
| `.local/decisions/` | Design proposals, comparisons, the discussion that led to a decision, rejected proposals |
| `.local/reviews/` | Code review records |
| `.local/experiments/` | Experiment logs, working notes of performance measurements |
| `.local/plans/` | Temporary implementation plans, working notes of AI agents |

In a new clone, or in an existing one, set up the exclusion, the directories and the missing local files with:

```sh
python3 tools/knowledge/init_local.py --apply    # without --apply it only shows what it would do
```

It never overwrites an existing file. [tools/knowledge/README.md](tools/knowledge/README.md) describes how to classify, find, update, expire and hand over this local material, and the tool and the templates for it.

### Design decisions

Record the deliberation and the settled decisions separately.

- Put the deliberation in `.local/decisions/<topic>.md`, including the alternatives, the measurements and the reasons for rejecting proposals.
- Put the settled decisions in `docs/`: the chosen approach, public API contracts, invariants, safety and thread-safety constraints, important limitations, and what future changes must watch out for.
- If a settled decision contradicts the existing specification, update the specification documents too.
- Do not justify a design only by referring to local material. A reader of the documents tracked in Git alone must be able to understand the current specification.

### Instructions for AI agents

- Save research results and design deliberation in `.local/`. When you make a new design decision, record how you reached it in `.local/decisions/` where useful.
- When the implementation is done and the specification is settled, put only the necessary information into `docs/`.
- Never:
  - Save research notes as they are in `docs/`.
  - Commit the full text of a discussion with an AI.
  - Write a temporary design proposal as the settled specification.
  - Commit large amounts of unorganized deliberation material.
  - Treat local material as the only specification.

### Commit messages

Write the title in the [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) format (for example, `feat: add LIN signal encoding`, `fix(j1939): keep the frame order within a step`).

- Main types: `feat` (new feature), `fix` (bug fix), `refactor`, `test`, `docs`, `build` (CMake, dependencies, patches), `ci`, `chore`.
- The scope (in parentheses) is optional. Use `core`, `j1939`, `lin`, `vector-xl`, `deps` and similar.
- In the body, write why you made the change, not what you changed.
- Commit messages of the upstream patches in `patches/` follow the upstream conventions, not this format.

### Before you commit

1. No file in `.local/` is staged.
2. No temporary research or review material is included.
3. If you changed a public API or the settled specification, you updated the related manuals.
4. You put important design constraints into the documentation.
5. The documentation matches the actual code.

Ignore settings alone do not untrack a tracked file: untrack it in a commit (`git rm --cached`).

### What to protect, and delegation

Ordinary changes to the product go through a pull request and its review and need no separate approval: adding, changing, deleting, moving, renaming, splitting or merging tracked files, refactoring included. Git keeps them, the pull request shows them, and they can be undone.

Ask the owner first, or do not do it, when a step would:

- discard or overwrite someone else's work: uncommitted changes, another worktree, a branch that is not merged;
- delete or overwrite data that Git does not hold, such as local records, machine images and backups;
- be irreversible or reach outside: rewriting pushed history, force-pushing a branch that others use, pushing to `main` without a pull request, publishing a tag or a release, opening issues or pull requests upstream.

An agent that the owner gave a task may hand parts of it to other agents. The task description is the delegation: within it, the other agent has the same authority, and no more. An agent stops and reports to whoever gave it the task when a step falls outside the task or would do one of the things above.

## Agent skills

Skills for AI agents live in `.agents/skills/<name>/SKILL.md`, a layout that several agents read (Codex, Gemini CLI and others). An agent that does not read that directory can open the `SKILL.md` file directly. Claude Code reads `.claude/skills/`; link it to the same directory (`ln -s ../../.agents/skills/<name> .claude/skills/<name>`) instead of keeping a copy.

- `agent-prompt-design`: a checklist for writing instructions, briefs and handoffs that another agent will act on.

## Before you change code

These steps are a trial. They are advice: no CI check enforces them, and their effect on the quality of the work is not established. If a step gets in the way, say so in your report instead of working around it.

- **Build small.** Choose the smallest design that meets the request and the constraints in `docs/` (safety, thread safety, the standards the code implements, compatibility); those constraints are part of what the work must satisfy.
  - A new abstraction, dependency, setting or extension point needs a reason that comes from the request or from such a constraint.
  - Reuse what exists before you add something.
  - Stop when the request and the constraints are met. Put further improvements in a separate task.
  - When you review, check for unnecessary complexity as well as for what is missing.
- **Placement note.** For a `feat` or `refactor`, write this line in the plan and in the pull request description. If the type changes while you work (a `fix` turns into a `feat`), write it then. When the change reads or interprets data that another place already reads, you can add a second line that names those readers and the way you looked (search terms, code-map page).

  ```text
  Placement: <new responsibility or state> -> <file or class>. Why: <relation to what is already there>
  ```

  The optional second line has the form `Same data elsewhere: <existing readers of the same input or data, and how you looked>`.

- **hotspots (optional).** `scripts/hotspots.py <paths you plan to touch>` shows which of those files are large, busy or touched by other work; see its `--help`. "attention" is a hint, not a request for another step or for a split.
- **Verification.** Choose it by risk with "[Choose the verification by risk](docs/code-map-testing.md#choose-the-verification-by-risk)", and end the report and the pull request description with `Verified:` and `Not run:` lines.

## Verification and scripts

`scripts/` contains the scripts used in development and CI. They are written with the Python 3.11+ standard library only. Every script prints only a summary (the details are in logs under `build/logs/`, and the summary names the location) and exits with 0 on success, 1 on failure and 2 on a usage error. Each script's `--help` gives its arguments and the details of its behavior. In CI (GitHub Actions), the scripts that CI calls also write the same summary to the job's Summary (`scripts/job_summary.py`; a failure names the artifact that holds the logs).

| Script | What it does |
|---|---|
| `scripts/doctor.py` | Checks whether this computer has what the build and the tests need, and prints one line per requirement (read-only; for anything missing, prints how to install it) |
| `scripts/verify.py` | Configures, builds and tests each preset, and summarizes the result (failed tests with the start of their output and the lines further down that say FAIL or Error, and the log locations) |
| `scripts/check-commits.py` | Checks the format of commit messages, and that no file that must not be committed is included |
| `scripts/check-docs.py` | Checks the relative links in the tracked Markdown files, and the section names quoted after a link in the form `の「…」`. Also checks that each English page (`docs/`, `README.md`) and its Japanese page (`docs/ja/`, `README.ja.md`) agree in the sequence of heading levels, the code blocks and the link targets (a page without its English edition is only counted; with `--require-english` it is an error) |
| `scripts/mutate.py` | Applies the mutations written in a TOML file one at a time and checks that the tests fail |
| `scripts/hotspots.py` | Optional information before you change code: which of the files you plan to touch are large, busy or touched by other work (it judges nothing, exits with 0 whatever it finds and with 2 on a usage error; see "Before you change code") |
| `scripts/verify-web.py` | Formatting, lint, unit tests, build and e2e tests of the screens (`gui/web`) and the client library (`gui/client`) |
| `scripts/verify-gui.py` | The whole GUI: the tests of the GUI server (`gui/server`), the client examples, then `verify-web.py` (`--skip-web` for the server only, `--launcher` to also use the real launcher) |
| `scripts/verify-python.py` | Creates environments with uv and checks the Python code that needs no C++ build (on 3.11, on the version in `.python-version`, and with the lower bounds of the dependencies) |
| `scripts/package.py` | Creates the prebuilt tools archive (`tools`), checks it by following the steps a user takes (`verify`), creates the template archive (`template`), and checks that the version numbers agree (`version`) |
| `scripts/run-gui.py` | Starts the browser GUI from the repository (for anything missing, it names the command that builds it; it builds nothing itself) |
| `scripts/trace-report.py` | Summarizes a profiling trace of one run: the critical path of each virtual-time step across the processes, and where each scenario write's time goes ([docs/internals/trace.md](docs/internals/trace.md)) |

- Run the GUI tests through `verify-web.py` and `verify-gui.py`; do not call pytest, ng or Playwright directly. Before their timing-sensitive tests, these 2 scripts wait while a build, a test run or a Windows VM is running on this computer, because tests that run at the same time fail their timing conditions (`--wait-limit`).
- On a slow or loaded computer, extend how long the tests wait with the environment variable `VEHICLE_SIL_TEST_TIMEOUT_SCALE` (for example, `3`). The CTest `TIMEOUT` takes the value at configure time, so reconfigure after you change the value. The rules are in "Timeouts and the scale factor" under "Test names and how to run tests" in [docs/code-map-testing.md](docs/code-map-testing.md#timeouts-and-the-scale-factor).

```sh
scripts/doctor.py                                    # first: is everything in place?
scripts/verify.py --linux                            # all Linux presets
scripts/verify.py linux-gcc-debug --tests '^launch'  # some tests on one preset
scripts/verify.py --summarize build/logs/verify-linux-gcc-debug-test.log
scripts/check-commits.py                             # the staged changes (--range origin/main..HEAD: each commit in the range)
scripts/check-docs.py                                # after you change docs
scripts/mutate.py mutations.toml --check             # only checks that the places to mutate are found
scripts/hotspots.py libs/launch tests/launch         # optional: large, busy or shared files among the paths you plan to touch
scripts/verify-python.py                             # Python code
scripts/verify-gui.py                                # GUI (--skip-web: the server only)
scripts/run-gui.py                                   # starts the GUI from the repository
scripts/package.py tools && scripts/package.py verify build/dist/vehicle-sil-tools-*-linux-x86_64.tar.gz
```
