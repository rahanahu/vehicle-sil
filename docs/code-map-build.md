# Build, packaging, dependencies and adding libraries

[日本語](ja/code-map-build.md)

<!-- Style: write each entry as a short block of the same shape (a subheading, a bullet list with one fact per line, a table whose cells are one sentence each). Do not pack several facts into one cell or one bullet: the line gets long and unrelated changes hit the same line. -->

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

See [code-map.md](code-map.md#dependency-direction) for the diagram legend and shared boundary tests.

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

- `project(... VERSION)` in the root `CMakeLists.txt`

[CHANGELOG.md](../CHANGELOG.md) lists the places to update with it:

- `pyproject.toml`, `gui/server/pyproject.toml`
- `template/get_tools.py`
- `GIT_TAG` in `template/CMakeLists.txt`
- The version in `template/README.md`
- The heading `## [<version>] - Unreleased` in `CHANGELOG.md`

At a release:

- In the commit that you tag, replace `Unreleased` in the heading with the release date (`YYYY-MM-DD`).
- `.github/workflows/release.yml` cuts the release notes from this version's section of `CHANGELOG.md`.

Tests:

- `scripts/package.py version` (and `template.get_tools`) finds mismatches.
- `--expect v<version>` (the release workflow) also rejects a heading without a date.

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
