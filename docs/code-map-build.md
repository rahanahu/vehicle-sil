# Build, packaging, dependencies and adding libraries

[日本語](ja/code-map-build.md)

This is the map for changes in this area. Start at [code-map.md](code-map.md); common test rules are in [code-map-testing.md](code-map-testing.md).

Related specifications and change areas: [build.md](build.md) / [dependencies.md](dependencies.md) / [Test development](code-map-testing.md) / [ECU, Peer, local execution and Simulation](code-map-execution.md) / [Core, step, signal and SIL Kit defaults](code-map-core.md)

## Test labels

### `fetchcontent`

Disabled by default.

- Pulls in a commit of this repository with FetchContent, then builds and runs a usage example.
- `fetchcontent.template`: copies the template to another directory, pulls in the current work tree with FetchContent, builds it and confirms that the check, run and scenario pass.

## Code and tests by directory

### `examples/fetchcontent_consumer` (use through FetchContent)

- Tests: `tests/fetchcontent` (only when `VEHICLE_SIL_TEST_FETCHCONTENT=ON`)
- `ctest -R`: `^fetchcontent\.`

### `template` (ECU project template), `scripts/package.py` (archives of the prebuilt tools and the template)

| Name | Details |
|---|---|
| `template/get_tools.py` | Downloads the tools |

| Test | Details |
|---|---|
| `tests/template` | Adds the template to the main build, then checks and runs it |
| Same as above | Tests `get_tools.py`, and `template` and `version` of `package.py`, with local archives |
| `fetchcontent.template` | Copies the template and pulls vehicle-sil in with FetchContent (opt-in) |
| `scripts/package.py verify` | Tests the tools archive by following the user's steps |

- `ctest -R`: `^template\.`

### `scripts/`

Scripts for development and CI. For the list, see "Verification and scripts" in [AGENTS.md](../AGENTS.md#verification-and-scripts).

| Test | Covers |
|---|---|
| `scripts/verify-python.py` | `--help` of each script |
| `tests/scripts/test_run_gui.py` | `scripts/run-gui.py`: how the launcher is chosen, the message for each missing item |
| `tests/scripts/test_job_summary.py` | `scripts/job_summary.py`: what it writes to the Summary of a GitHub Actions job, and when it writes nothing |
| `tests/scripts/test_check_docs.py` | `scripts/check-docs.py`: pairs of English and Japanese pages (headings, code blocks, links), pages without an English edition yet |
| `tests/scripts/test_verify.py` | `scripts/verify.py --summarize`: the lines of a failing test's output in the summary (the first lines, then the lines further down that say FAIL or Error) |
| `tests/scripts/test_hotspots.py` | `scripts/hotspots.py`: the signals (size, churn, stack, parallel), what counts as a lower layer and as independent work, what is left out (merged, closed, stale), what it says it could not observe, a path that is not in HEAD, and a failing, hanging or odd `gh` (the test is not run on Windows) |
| `tests/scripts/test_trace_report.py` | `scripts/trace-report.py`: on made-up traces, the participant that completes each step last, the synchronization time, the launcher's hold, each write's round trip, and the exit codes |
| `tests/scripts/test_package.py` | `scripts/package.py`: MSYS2 license staging and Windows `--build-only` export |
| `tests/template/test_get_tools.py` | `package.py template`, `version` |

- `ctest -R`: `^template\.get_tools`, `^scripts\.`

### `patches/sil-kit`

Patches applied to SIL Kit. At configure time, they are applied to the copy in the build directory (`cmake/VehicleSilDependencies.cmake`). For how to write them, see [patches/README.md](../patches/README.md).

- Tests: configure succeeds

### `cmake/`

| What | Location |
|---|---|
| Pulling in dependencies | `VehicleSilDependencies.cmake`, `VehicleSilGrpc.cmake` |
| Test registration | `VehicleSilTestHelpers.cmake` |
| Boundary checks | `VehicleSilCheck*.cmake` |

## Dependency direction

See the [diagram legend and layers](code-map-core.md#dependency-direction) and [boundary tests](code-map-testing.md#boundary-tests).

### Other executables

Each one links `vehicle_sil_cli` (`tools/common`, header-only) privately.

| Executable | Also links |
|---|---|
| `vehicle_sil_sim_controller` | `core` |
| `vehicle_sil_can_bus_simulator` | `can_netsim` |
| `vehicle_sil_launch` | `launch` and `process` |
| `vehicle_sil_launch` (Windows) | `shell32` too |
| `vehicle_sil_launch` (`VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`) | `control_grpc` and `core` too |

## What to update together

### Version numbers

Where to change:

- `project(... VERSION)` in the root `CMakeLists.txt` (numbers only), and `VEHICLE_SIL_VERSION_PRERELEASE` next to it: `rc.<n>`, `beta.<n>` or `alpha.<n>` for a pre-release, `""` for a release.
- Together they make the full version (`0.2.0-rc.1`, the SemVer form), `VEHICLE_SIL_VERSION_FULL`. The executables report it, and every place below names it. The tag is `v<full version>`; a tag with a pre-release part makes a pre-release on GitHub.

[CHANGELOG.md](../CHANGELOG.md) lists the places to update with it:

- `pyproject.toml`, `gui/server/pyproject.toml` (`uv lock` then writes the PEP 440 form, `0.2.0rc1`, in `uv.lock`)
- `__version__` in `python/vehicle_sil_control/__init__.py` and in `gui/server/src/vehicle_sil_gui/__init__.py`
- `template/get_tools.py`
- `GIT_TAG` in `template/CMakeLists.txt`
- The version in `template/README.md`
- The heading `## [<version>] - Unreleased` in `CHANGELOG.md`

At a release:

- In the commit that you tag, replace `Unreleased` in the heading with the release date (`YYYY-MM-DD`).
- `.github/workflows/release.yml` cuts the release notes from this version's section of `CHANGELOG.md`.
- Publishing the draft release puts the client library on npm (below).

Tests:

- `scripts/package.py version` (and `template.get_tools`) finds mismatches.
- `--expect v<version>` (the release workflow) also rejects a heading without a date.

### Publish the client library to npm

`.github/workflows/publish-npm.yml` puts `@vehicle-sil/gui-client` on the npm registry when a person publishes a release (the event "release published"). Publishing the release is the approval to publish to npm; the tag push, the draft and the rehearsal of `release.yml` publish nothing to npm. To retry (when that run failed), run the workflow by hand from `main` (Actions > publish-npm > Run workflow) with the tag of the published release; it refuses a draft and another branch.

- It publishes the release's `vehicle-sil-gui-client-<version>.tgz` as it is, without building it again. First it checks that the tag is `v<version>`, the release is published and its pre-release mark agrees with the version, `release.yml` uploaded the file and `SHA256SUMS` (`github-actions[bot]`), the file's SHA-256 is the one in `SHA256SUMS`, and the `package.json` in it has the name, the version and this repository as `repository.url` (`gui/client/pack.mjs` writes it; npm compares it with the repository that publishes).
- The dist-tag is `latest` for a version without a pre-release part and `next` for one with it (`0.2.0-rc.1`). `next` stays on the last pre-release, so after the release it is older than `latest`. A release of an older line after a newer one (0.1.x after 0.2.0) would move `latest` back: move it with `npm dist-tag add` by hand.
- It uses npm's trusted publishing (OIDC) and keeps no npm token. Only the `publish` job has `id-token: write`, in the GitHub environment `npm`. npm attaches provenance. It needs a GitHub-hosted runner, npm 11.5.1 or later and Node.js 22.14.0 or later; the job checks the versions.
- Run again, or with the version already on npm: the same file (`dist.integrity`) succeeds without publishing, another file fails. npm never takes a version twice, even after it is unpublished.

npm's trusted publishing checks only the repository, the workflow's file name and the environment, not the branch or tag, and a workflow file can be changed on any branch. The repository settings below keep the approval with the person who publishes the release. Set them once, in this order, before the workflow first runs (a job that names the environment `npm` creates it without protection if it does not exist):

1. In the repository's Settings > Environments, create `npm` and limit its deployment branches and tags to `main` and the tags `v*` (the event runs on the tag, a run by hand on `main`).
2. In Settings > Rules > Rulesets, add a tag ruleset for `refs/tags/v*` that restricts creations, updates and deletions, with only the repository's administrators in the bypass list. With it, only they can make a ref that the environment accepts outside `main`, so the environment needs no reviewers.
3. On npmjs.com, in the package's Settings > Trusted publishing (GitHub Actions), add:

   | Field | Value |
   |---|---|
   | Organization or user | `rahanahu` |
   | Repository | `vehicle-sil` |
   | Workflow filename | `publish-npm.yml` |
   | Environment name | `npm` |
   | Allowed actions | `npm publish` |

4. After the first publish with it, set the package's Settings > Publishing access to require two-factor authentication and disallow tokens; this does not stop trusted publishing.

Change these together with the workflow's file name, the repository or the environment: npm accepts the OIDC token only when they match, and a trusted publisher cannot be edited (delete it and add it again).

### `VEHICLE_SIL_STATIC`

Where to change:

- How SIL Kit is pulled in, and the static link settings on `SilKit::SilKit`, in `cmake/VehicleSilDependencies.cmake`

Documentation to update:

- [build.md](build.md): the `VEHICLE_SIL_STATIC` section (it also has the license notices to include when you distribute)
- The table in "[How dependencies are pulled in (`cmake/VehicleSilDependencies.cmake`)](dependencies.md#how-dependencies-are-pulled-in-cmakevehiclesildependenciescmake)" in [dependencies.md](dependencies.md)

Related:

- The presets `linux-gcc-static` and `windows-mingw-static` (CI builds only `linux-gcc-static`)
- The launcher in the tools archive is built this way too.

### Contents of the tools archive

Where to change:

- `scripts/package.py tools`

Documentation and files to update:

- [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md): the part that describes the same contents (what is bundled, and where the license texts are)
- "[Prebuilt tools](build.md#prebuilt-tools)" in [build.md](build.md)
- `.github/workflows/release.yml`

If you change the gRPC version, update `GRPC_LICENSES` in `package.py` and the license names in the table of THIRD_PARTY_NOTICES.md, because the licenses of what gRPC pulls in change with the version.

### Assumptions about SIL Kit APIs and upstream behavior

Where to change:

- The SIL Kit APIs in use
- Assumptions about upstream behavior

Documentation to update:

- "[APIs used](dependencies.md#apis-used)" and "[Upstream behavior the implementation relies on](dependencies.md#upstream-behavior-the-implementation-relies-on)" in [dependencies.md](dependencies.md)

### Add a library to `libs/`

Where to add:

- `add_subdirectory` in the root `CMakeLists.txt`

Documentation to update:

- "[Directories and targets](architecture.md#directories-and-targets)" in [architecture.md](architecture.md)
