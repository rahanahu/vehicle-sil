# Test development

[日本語](ja/code-map-testing.md)

Common test rules and documentation checks. Find area-specific tests from [code-map.md](code-map.md).

Related specifications and change areas: [build.md](build.md)

**On this page:** [Test names and how to run tests](#test-names-and-how-to-run-tests) / [Test labels](#test-labels) / [Documentation examples and tests](#documentation-examples-and-tests) / [Boundary tests](#boundary-tests)

## Test names and how to run tests

```sh
ctest --preset linux-gcc-debug -R '^launch\.'
scripts/verify.py linux-gcc-debug --tests '^launch_run\.'
```

### Naming and registration

A test is named `<prefix>.<name>`. The prefix is `PREFIX` in `tests/<area>/CMakeLists.txt`. Several areas can share a prefix: `launch.*` is in `tests/config`, `tests/scenario`, `tests/runner` and `tests/launch` (one test executable each; the first three also have the label of their directory, `config`, `scenario` or `runner`), so that the test of a layer links only that layer.

| Kind of test | Name | Registration |
|---|---|---|
| Test in a test executable (harness) | `<prefix>.<TEST_CASE name>` | Registered at configure time from `TEST_CASE(name)` in the source (`vehicle_sil_add_harness_tests()`, `cmake/VehicleSilTestHelpers.cmake`). |
| Test that starts processes (scenario) | `<prefix>.<scenario>` | Registered with `vehicle_sil_add_scenario_tests()`. |

To add a harness test:

- Adding a `TEST_CASE` to an existing source file needs no change to CMake.
- Add a new source file to `SOURCES` in the `CMakeLists.txt` of its directory.
- The macros (`TEST_CASE`, `CHECK`, `CHECK_EQ`) are in `tests/unit/test_harness.hpp`.

Where scenario tests live:

- `SCENARIOS` in `CMakeLists.txt` lists the scenarios.
- Their code is in `*launcher.cpp` in the same directory (`tests/integration/launcher.cpp`, `tests/launch/launch_run_launcher.cpp` and others).
- The shared code is in `tests/integration/launcher_support.hpp`.
- The `launch_run` scenarios are in `tests/launch/launch_run_<topic>.cpp`, one file for each topic. `main()` is in `launch_run_launcher.cpp`, and the code they share is in `launch_run_support.hpp`.

Rules for areas (the directories directly under `tests/`):

- Every directory with a `CMakeLists.txt` is added automatically, in name order (do not add a line to `tests/CMakeLists.txt`).
- An area with a condition (`control_server`, `fetchcontent`, `integration`, `virtual_time` and others) checks the condition at the top of its own `CMakeLists.txt` and calls `return()`.
- An area does not test for another area's targets with `if(TARGET ...)`. That would make it depend on the order in which areas are added.
- Write CMake lists (`add_subdirectory`, sources, scenarios) one entry per line, in name order. Where the order matters, a comment says so.

### Run a subset of tests

- To run one test, pass its name to the test executable (`<executable> <name>`). This is also how ctest runs it.
- To select one prefix only, write the pattern up to and including `\.`. `^j1939` also matches `j1939_parts.`, `j1939_codec.`, `j1939_silkit.` and `j1939_spy.`.
- To select or exclude by label or name, use `-L`, `-LE` or `-E` with `ctest` or `scripts/verify.py` (add `--no-build` to `scripts/verify.py` to skip configure and build; see [Test labels](#test-labels)).

### Choose the verification by risk

Run on your computer what the change can break; leave the full matrix of presets to CI. The page of the area you change lists its tests and the `ctest -R` expression for each (see [Find by change area](code-map.md#find-by-change-area)).

| Change | Run on your computer |
|---|---|
| Any change (the default) | One preset and the tests of what you touched: `scripts/verify.py linux-gcc-debug --tests '<regular expression>'` (add `--targets <target>` and `-L <label>` or `--no-build` to stay narrow) |
| Concurrency, threads, state transitions or time | Run the touched tests also with `linux-gcc-release` (`-O3`) and `linux-clang-tsan` using `--repeat`, and check the pull request's `linux-tsan` check before merging (`all checks` does not wait for it) |
| A public header or a class or function template that many targets include | Build the touched targets with `linux-clang-tsan`: `scripts/verify.py linux-clang-tsan --targets <target> --tests '<regex>'` |
| What the launcher prints or counts (findings, events, `run` output), or the documentation that copies it | `^launch`, `^multi_host\.`, `^schema\.` and `^template\.` (the ECU project template), which compare the documentation (see [Documentation examples and tests](#documentation-examples-and-tests)) |
| Launcher output that the GUI replays (`gui/server/tests/recordings/`), or the Control API | `scripts/verify.py linux-gcc-control --tests '^control'`, then `scripts/verify-gui.py --skip-web --launcher build/linux-gcc-control/tools/launch/vehicle_sil_launch` |
| The browser GUI only | During iteration, narrow with `scripts/verify-web.py --only format,lint`, `--only e2e --e2e-spec e2e/<file>.spec.ts`, or a direct single-file `vitest` / `ng test --watch=false --include` / `playwright test` run (when `scripts/busy_machine.py` reports `(none)`). Before a PR, run `scripts/verify-web.py --skip-install` (`--e2e-grep <pattern>` runs only matching e2e tests; `--summarize-junit build/logs/web-e2e.junit.xml` lists per-file times and the slowest tests) |
| The GUI server only | During iteration, `scripts/verify-gui.py --no-sync gui/server/tests/<file>.py` (or `-k <expr>`, both run server tests only) or direct in-process `uv run --no-sync pytest gui/server/tests/<file>.py`; before a PR, `scripts/verify-gui.py --skip-web` |
| Documentation only | `scripts/check-docs.py` |
| A script or Python-only code | `python3 tests/scripts/test_<name>.py` and `scripts/verify-python.py --only <step>` (for example `--only scripts` or `--only control`). When a change under `scripts/` branches on the OS (`os.name == "nt"` or platform helpers), add Linux-runnable unit tests that mock `os.name == "nt"` |

End the report of the work and the description of the pull request with two lines:

```text
Verified: <what you ran and what it covered>
Not run: <what you left to CI or did not run, and why>
```

Do not list under `Verified:` what was not run. If you ran nothing, say so on both lines.

### What CI runs

The job `all checks` in `.github/workflows/ci.yml` is the check that a merge into `main` needs: it passes when every other job passed or was left out, and fails when one failed or was cancelled. A pull request runs part of the matrix; a push to `main` runs all of it.

| Change | What runs |
|---|---|
| Documents only | The links and the schema and template checks (`python`). Also the `linux-gcc-debug` tests that compare the documents when a changed page is one they read (`docs/simulation-*.md`, `docs/scenario.md`, `docs/ecu-executable.md`, `docs/j1939-guide.md`, `docs/howto/*.md`), and `linux-gcc-control` when it is `docs/howto/first-ecu.md` or `docs/howto/operate-signals.md`. Also the format of the GUI's READMEs when a changed file is under `gui/`, the GUI server's tests and the timeline element's test when it is `docs/gui.md` or `gui/protocol/README.md` (or their Japanese pages), and the client library package check (`npm run pack:client`) when it is `gui/client/README.md` or `examples/gui_starter/README.md` (or their Japanese pages) |
| The browser GUI only (`gui/web`, `gui/client`, `gui/timeline`, `gui/protocol/examples`, `examples/gui_client`, `examples/gui_starter`) | The GUI jobs (`gui` and `gui-server`) and `python`. Also `linux-gcc-debug` when a changed file is one of the files whose limits or constants `tests/launch/test_shared_values.cpp` reads |
| The GUI server (`gui/server`, `gui/protocol/dashboard.schema.json`) | `gui`, `gui-server`, `linux-gcc-debug`, `linux-gcc-control` and `python` |
| `tools/knowledge` only | `python` |
| Code | `linux-gcc-debug`, `linux-clang-tsan`, `linux-gcc-control`, Windows, the GUI jobs (unless every file is outside the GUI and what its tests read) and `python` |
| Build files (CMake, presets, patches, `external/`, `.gitmodules`, `.github/`) | Also `linux-gcc-release`, `linux-gcc-static`, the oldest CMake and the test that builds the ECU project template with FetchContent |
| A push to `main` (or `workflow_dispatch`) | Everything above, and `linux-clang-asan` |

`all checks` does not wait for `linux-clang-tsan`: in a pull request its failure is shown on its check but does not hold the merge; on `main` it fails the run. `python` also checks the title of a pull request and, when it has one commit, that commit's message (`scripts/check-commits.py --message`), and that no file under `.local/` or `build/` and no `CLAUDE.local.md` is in the change (`--changed`). A re-run reads the old title: after you fix it, push a commit or close and reopen the pull request.

### Ports and locks

Tests do not listen on fixed ports. They can run on the same computer at the same time as the tests of a build in another work tree.

| What listens | How the port is chosen |
|---|---|
| registry | Port 0 (`RegistryProcess`, a configuration file without `registry_uri`) |
| Control Server | `--control 0` |
| GUI server | `--port 0`, or a free port that the e2e tests pick at each start |
| 18500 of `multi_host.two_hosts` | Inside a network namespace that the test creates |

A scenario that needs a fixed port shown in the documentation (such as 8500 for `manual`) takes a lock for the whole computer with `MACHINE_LOCK` of `vehicle_sil_add_scenario_tests()`, and such scenarios run one at a time.

- The lock is an `flock` on a file in `/tmp` (`cmake/vehicle_sil_machine_lock.sh`; there is no lock on Windows).
- While it waits, the test prints one line. The time it waits counts toward the test's CTest `TIMEOUT`.
- If the test cannot open the lock file (for example, it belongs to another user), it prints one line and runs without the lock.

### Timeouts and the scale factor

Wait for something that is expected to happen with `eventually(condition, limit)` (`test_harness.hpp`) or with a mechanism that signals that it happened. Do not sleep for a fixed time and assume that things have progressed.

Wait limits are multiplied by a scale factor.

| Topic | Details |
|---|---|
| How the factor is chosen | The environment variable `VEHICLE_SIL_TEST_TIMEOUT_SCALE` when the tests run; without it, the build's default. |
| Build default | The CMake cache variable `VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT`. The default is 1, and 4 for the TSan presets. |
| How to write the value | Digits and at most one decimal point, greater than 0 (`3`, `2.5`). |
| Other forms (spaces, exponents, hexadecimal) | The C++ tests, the Python tests under `tests/` (`tests/common/timeout_scale.py`, which also honors `VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT`), the GUI server tests and the e2e tests stop and print the variable's name and how to fix it. |
| CI | Sets 3. The TSan jobs do not set it and use the preset's default (`.github/workflows/ci.yml`). |

Whether a value is scaled depends on its kind.

| Kind of value | Scaled | Reason and caution |
|---|---|---|
| A limit for waiting for something that is expected to happen. The same applies to a time passed to the product as a limit (such as the value that a launcher passes to the product's `--timeout-ms`). | Yes | A passing test moves on as soon as what it waits for happens, so a larger factor does not slow it down. |
| A value that checks a timeout, where a child process or another thread must finish something before the timeout (for example, a child writes one line and is then cut off) | Yes | Build the numbers in the expected text from the same value. Such a test always takes longer by the factor. |
| A wait that checks that something does not happen, a period, or the value in a test that checks the timeout value itself (such as the 1500 ms of `receiver-timeout`) | No | Scaling it changes what the test checks. |

Rules for writing limits:

- When you pass a limit to a function, pass it through `scaled_timeout(20s)` (`tests/unit/test_timeout_scale.hpp`). The Python tests under `tests/` use `scaled()` from `tests/common/timeout_scale.py`, the GUI server tests use `scaled()` in `conftest.py`, and the e2e tests use `scaled()`.
- A function that takes a limit and scales it internally (`eventually`, the waits in `tests/process/spawn_sleeper.hpp`, `--timeout-ms` of a scenario launcher and others) says "scaled" in its documentation comment.
- Do not scale a value on the caller's side when you pass it to such a function.

The CTest `TIMEOUT`:

- The `TIMEOUT` of harness tests and scenario tests is extended by the factor at configure time (the environment variable, or without it the build's default, rounded up to an integer) (`cmake/VehicleSilTestHelpers.cmake`).
- If you raise the factor after configuring, configure again. Otherwise CTest kills a stuck test before the test can report its own failure.
- When that happens, the harness and the launcher print a line `warning: ... configure the build again ...`.

## Test labels

```sh
ctest --preset linux-gcc-release -L integration
# FetchContent test (slow: it rebuilds the dependencies from scratch)
cmake --preset linux-gcc-release -DVEHICLE_SIL_TEST_FETCHCONTENT=ON
ctest --preset linux-gcc-release -L fetchcontent
```

For every label:

- The log of each process stays under `build/<preset>/tests/`, in `integration/<scenario>/`, `virtual_time/<scenario>/`, `lin_cluster/<scenario>/` and `vector_xl/integration/<scenario>/`. When a test fails, the launcher prints all the logs.
- The `<label>.registration` test of each test executable detects test cases that are not registered (it compares the executable's `--list` with the registered names). Where one prefix is shared by executables of several directories, the test is `launch.registration` for `tests/launch` and `launch.registration.<target>` for the others.
- In a cross build, if you set `CMAKE_CROSSCOMPILING_EMULATOR` (for example `wine`), `ctest` runs the test executables with it (the `registration` tests included). Make the Windows DLLs (the MinGW runtime libraries) findable, for example by placing them next to the executables.
- The test presets run 8 tests in parallel. On a computer with fewer CPUs, `scripts/verify.py` runs as many as there are CPUs.
- Tests that measure wall-clock duration (`virtual_time.*`, `core.virtual_time_*`, the LIN speed tests) run alone.

### `unit`

Set on the tests of command-line parsing and of writing to stdout and stderr (`unit.*`).

| Also set on | Details |
|---|---|
| `core`, `lin`, `step`, `can`, `can_bridge`, `eth`, `j1939`, `peer`, `testkit`, `ecu`, `simulation`, `control`, `control_server`, `launch`, `config`, `scenario`, `runner`, `process`, `vector_xl` | Unit tests and boundary checks |
| `template.get_tools` | Tests the template's `get_tools.py`, and `template` and `version` of `scripts/package.py`, with locally built archives |
| `param_service.demo` | The only example run that has it |

It is not set on the scenario tests that start processes (`integration`), the example runs (`examples`), the Python client tests (`python`) or `fetchcontent`.

### `integration`

`integration.*` starts `sil-kit-registry`, a receiver and a sender (the real-time `Peer` and the J1939 stack) as separate processes, and judges the result by their exit codes.

| Scenario | What it checks |
|---|---|
| `address-claim-and-pgn` | Address Claim, and that the CAN ID and payload of the PGN match |
| `receiver-timeout` | Timeout handling |
| `receiver-detects-wrong-name`, `receiver-detects-wrong-payload` | The receiver reports a wrong NAME and a corrupted payload as failures |

The label is also set on the other scenario tests that start processes.

- `virtual_time.*`, `lin_cluster.*`, `launch_cli.*`, `launch_run.*`, `ecu_main.*`, `vector_xl.integration.*`, `control_server.launcher` (for what they cover, see the entry of each label)
- `template.check_and_run`: builds the template's ECUs with the main build and confirms that the launcher's check, run and scenario pass. It compares the check and run output with the guide "Create your first ECU".

## Documentation examples and tests

Among the examples in the documentation (configuration files, code, command output), a code block that directly follows a `<!-- ... -->` marker line is compared with the real thing by a test. The tests below fail when you change a marker or its block.

- The tests compare the English pages: the directory named by `VEHICLE_SIL_TESTED_DOCS` in `tests/CMakeLists.txt` is `docs/`. `scripts/check-docs.py` protects the Japanese pages in `docs/ja/` by comparing code blocks (the English and Japanese pages have the same number, order and language tags of blocks, and a block without Japanese is the same byte for byte).
- Tests that look at the pages themselves (`tests/schema/test_schemas.py`, the GUI server tests, `gui/client/pack.mjs`, `scripts/package.py version`) read the pages of both languages.

### Reference examples

| Document | Marker, block | Test that compares it |
|---|---|---|
| [simulation-config.md](simulation-config.md), [simulation-check.md](simulation-check.md), [simulation-run.md](simulation-run.md), [simulation-events.md](simulation-events.md) | `check` markers | `launch_cli.docs` |
| Same as above | `run` markers | `launch_run.docs` |
| [simulation-events.md](simulation-events.md) | The event examples of "Programmatic use" | `launch_run.events` |
| [simulation-config.md](simulation-config.md) | The examples of "[Starting a participant yourself (`manual`, `--manual`)](simulation-config.md#starting-a-participant-yourself-manual---manual)" | `launch_run.manual` |
| [ecu-executable.md](ecu-executable.md) | `<!-- vehicle_sil_engine_ecu --describe -->` | `ecu_main.describe` |
| [scenario.md](scenario.md) | `<!-- vehicle_sil_launch run simulation.yaml --scenario ... -->` | `launch_run.scenario` |
| [scenario.md](scenario.md) | `<!-- throttle_mistakes.yaml -->`, `<!-- throttle_response.yaml -->` | `launch_run.scenario` |
| YAML in the documentation, and the key tables | No marker | `schema.files_and_launcher` (`tests/schema/test_schemas.py`) |

- `launch_cli.docs` and `launch_run.docs` compare byte for byte, except for the vehicle-sil version.
- `launch_run.events` compares byte for byte after substitution.
- The examples with errors (`two-engines.yaml`, `engine-alone.yaml`, and the scenarios `throttle_mistakes.yaml` and `throttle_response.yaml`) come from the blocks in the documentation; the test writes them to its own working directory.
- The working directory is under `build/<preset>/tests/launch/` and holds copies of the examples and links to the ECUs. The examples with errors are not placed in the directory of the working examples (`build/<preset>/examples/ecu_main/`).

### Guide examples

| Guide | Marker | Test that compares it |
|---|---|---|
| [howto/first-ecu.md](howto/first-ecu.md) | `<!-- examples/first_ecu/... -->`, `<!-- first ECU: ... -->`, `<!-- cooler_ecu --describe: an excerpt -->` | `template.first_ecu`, `control_server.example_first_ecu` |
| Same as above | Output blocks of the template alone | `template.check_and_run` |
| [howto/scenario-tests.md](howto/scenario-tests.md) | `<!-- examples/scenario_tests/... -->`, `<!-- examples/ecu_main/scenarios/... -->`, `<!-- scenario guide: ... -->` | `launch_run.scenario_guide` |
| [howto/integrate-ecus.md](howto/integrate-ecus.md) | `<!-- examples/integrate_ecus/... -->`, `<!-- integrate ECUs: ... -->` | `launch_docs.integrate_ecus` |
| [howto/multi-pc.md](howto/multi-pc.md) | `<!-- examples/ecu_main/simulation.two-pcs.yaml -->`, `<!-- multi-pc guide: ... -->` | `multi_host.docs` |
| [howto/operate-signals.md](howto/operate-signals.md) | `<!-- examples/launch_control/... -->`, `<!-- vehicle_sil_control ... -->`, `<!-- standard input of run --start-paused --stdin-commands: ... -->`, `<!-- examples/ecu_main/dashboard/main.cpp: an excerpt -->` | `control_server.guide_operate_signals` |
| All guides in [howto/](howto/) | The same `check` and `run` markers as the reference | `launch_cli.docs`, `launch_run.docs` |

- The `check` and `run` output blocks in the guides are copies of the examples with the same markers in the documents directly under `docs/` (simulation-*.md and [scenario.md](scenario.md)). `launch_cli.docs` and `launch_run.docs` compare the guides' blocks with what they copy.
- One `<!-- vehicle_sil_launch run ... -->` in howto/scenario-tests.md is a copy of the same block in scenario.md, and `launch_run.scenario` also compares it.
- For the locations of the example files and what their tests cover, see the `examples/` entries in the relevant pages listed under [Find by change area](code-map.md#find-by-change-area).

### What the tests do not compare

No test compares the following. Keep them in sync by hand.

| Document | Not compared |
|---|---|
| The guides in [howto/](howto/) | Code quoted without a marker |
| "[Add J1939 communication](howto/first-ecu.md#add-j1939-communication)" in [howto/first-ecu.md](howto/first-ecu.md) | Places that refer to the template's code by line number |
| "[Run the ECU from a debugger](howto/first-ecu.md#run-the-ecu-from-a-debugger)" in [howto/first-ecu.md](howto/first-ecu.md) | The stderr lines and result table, the same as in the `launch_run.manual` example, shown with the name `Cooler` |
| [simulation-check.md](simulation-check.md) | The table of `code` values in "Checks" |
| [simulation-events.md](simulation-events.md) | The `features` table, and `kFeatures` in `events.hpp` |
| [ethernet-bridge.md](ethernet-bridge.md) | Copies of the bridge's output and error messages |

## Boundary tests

These tests read the sources' `#include` lines and similar, and check the boundaries between layers. They fail when you add an internal header or a SIL Kit header.

| Test | What it checks |
|---|---|
| `j1939_parts.boundary` | `libs/j1939/parts` uses only the engine's public API and the toolbox, and links only `vehicle_sil::j1939` and other protocol parts |
| `local.boundary`, `testkit.boundary`, `peer.boundary`, `ecu.boundary`, `simulation.boundary`, `j1939_codec.boundary`, `param_service.boundary`, `j1939_spy.boundary` | The sources of that directory do not use the engine's internals (`cmake/VehicleSilCheckJ1939Parts.cmake`) |
| `ecu.no_silkit_boundary`, `ecu.simulation_headers_boundary`, `lin.boundary`, `step.boundary`, `eth.boundary`, `signal.boundary` | `libs/ecu`, `libs/simulation/include`, `libs/lin`, `libs/step`, `libs/eth` and `libs/signal` include no SIL Kit header, directly or indirectly (`cmake/VehicleSilCheckNoSilKit.cmake`) |
| `peer.controllers_are_made_on_the_simulation_name` | In `libs/`, only `libs/peer/src/peer.cpp` creates SIL Kit bus controllers, and every call creates one on a network whose name went through `simulation_name()` (`cmake/VehicleSilCheckControllerNetworks.cmake`) |
| `core.silkit_configuration_defaults` | Every SIL Kit configuration that `libs/`, `tools/`, `bridges/` and `tests/` (except `tests/core/configuration_fixtures`) build from a string goes through the vehicle-sil defaults (`default_participant_configuration()` and others) (`cmake/VehicleSilCheckSilKitConfiguration.cmake`) |
| `launch.described_networks_are_read_in_one_place` | In `libs/config`, `libs/launch`, `libs/runner`, `libs/scenario` and `tools/launch`, only `libs/config/src/check_networks.cpp` (`seen_networks()`, which applies the map), `libs/launch/src/format/reported_networks.cpp` (the code that writes `reported` in the JSON) and `libs/config/src/describe_read.cpp` read the reported networks (`description.networks`, also through a pointer to member `::networks`) (`cmake/VehicleSilCheckDescribedNetworks.cmake`; `config.networks` of the configuration is not counted) |
| `launch.request_boundary`, `launch.probe_boundary`, `launch.format_boundary`, `launch.endpoints_boundary`, `launch.usecases_boundary` | The files of `libs/launch/src/request/`, `probe/`, `format/`, `endpoints/` or `usecases/` and the public headers that belong to the part include none of the headers that part must not reach (listed for each part in "Layers in `libs/launch`" of [architecture.md](architecture.md); SIL Kit for all but `endpoints/`, whose Control Server is the one exception; `usecases/` reaches no other part's limit but no SIL Kit), directly or through the headers they include; a failure names the chain (`DIR`, `FORBIDDEN_PREFIXES` and `ONLY_PREFIXES` of `cmake/VehicleSilCheckIncludes.cmake`; a new file in the directory is checked without a list) |
| `launch.parts_include_check_rejects_a_forbidden_include` | The check above fails on fixtures that reach a forbidden header through a header beside the file and through a private header of another part, and names the chain |
| `launch.config_files_do_not_reach_the_scenario` | `connections.hpp` of `libs/launch` includes no header under `vehicle_sil/scenario/`, directly or through the headers it includes; a failure names the chain (`FORBIDDEN_PREFIXES` of `cmake/VehicleSilCheckIncludes.cmake`; the request is guarded by `launch.request_boundary`) |
| `config.no_silkit_boundary` | `libs/config` includes no SIL Kit header, directly or indirectly, and of the core headers only those of `vehicle_sil::sim_base` (`cmake/VehicleSilCheckNoSilKit.cmake`) |
| `config.includes_no_runtime` | Every header and source of `libs/config` includes no header under `vehicle_sil/launch/`, `vehicle_sil/scenario/` or `vehicle_sil/runner/`, no `child_process.hpp` or `process.hpp`, directly or through the headers it includes; a failure names the chain (`DIR` and `FORBIDDEN_PREFIXES` of `cmake/VehicleSilCheckIncludes.cmake`; a new file in the directory is checked without a list) |
| `config.include_check_rejects_a_forbidden_include` | The check above fails on a directory of fixtures that reaches them, and names the chain |
| `scenario.no_silkit_boundary` | `libs/scenario` includes no SIL Kit header, directly or indirectly, and of the core headers only those of `vehicle_sil::sim_base` (`cmake/VehicleSilCheckNoSilKit.cmake`) |
| `scenario.includes_no_runtime` | Every header and source of `libs/scenario` includes no header under `vehicle_sil/launch/` or `vehicle_sil/runner/`, no `child_process.hpp` or `process.hpp`, directly or through the headers it includes; a failure names the chain (`DIR` and `FORBIDDEN_PREFIXES` of `cmake/VehicleSilCheckIncludes.cmake`) |
| `scenario.include_check_rejects_a_forbidden_include` | The check above fails on a directory of fixtures that reaches them, and names the chain |
| `runner.public_headers_need_no_silkit` | The headers under `libs/runner/include` (`vehicle_sil/runner/`, `detail/` among them) include no SIL Kit header and no header of core's runtime, directly or indirectly: the SIL Kit parts of a run are behind `RunnerImpl` (`cmake/VehicleSilCheckNoSilKit.cmake`; the runner links `core`, so the link does not show it) |
| `runner.includes_no_launcher` | Every header and source of `libs/runner` includes no header under `vehicle_sil/launch/`, directly or through the headers it includes (`DIR`, `FORBIDDEN_PREFIXES` and `ONLY_PREFIXES` of `cmake/VehicleSilCheckIncludes.cmake`: the runner itself uses the headers that the check forbids to the pure layers) |
| `runner.pure_files_include_no_runtime` | `command.hpp`, `traffic.hpp` and `control_rules.*` of `libs/runner` include no SIL Kit, process, run, events, time-command or stop-request header, directly or through the headers they include; a failure names the chain (`cmake/VehicleSilCheckIncludes.cmake`) |
| `runner.include_check_rejects_a_forbidden_include` | The check above fails on a directory of fixtures that reaches `vehicle_sil/launch/`, and names the chain |
| `j1939_codec.independent_of_the_engine` | The only vehicle-sil header that `codec.hpp` includes is `j1939/types.hpp` (the value types); it does not include `Network` or `Node` |
| `eth_link.boundary` | `libs/eth_link` includes no SIL Kit header, directly or indirectly (`cmake/VehicleSilCheckNoSilKit.cmake`) |
