# Code map

[日本語](ja/code-map.md)

<!-- Style: write each entry as a short block of the same shape (a subheading, a bullet list with one fact per line, a table whose cells are one sentence each). Do not pack several facts into one cell or one bullet: the line gets long and unrelated changes hit the same line. -->

Use this map when you develop vehicle-sil itself: it shows where to make a change, which tests cover it, how to run only those tests and what else to update with it. It does not describe what each directory and target contains; see "[Directories and targets](architecture.md#directories-and-targets)" in [architecture.md](architecture.md).

## Find by change area

Start with the page for the area you change. Follow its specifications and read neighboring maps only when the change crosses their boundaries. Common test rules are in the testing page. CMake and CTest are authoritative for actual registrations and links; the diagrams and change checklists record design constraints.

| Change area | Map |
|---|---|
| Test development | [code-map-testing.md](code-map-testing.md) |
| Core, step, signal and SIL Kit defaults | [code-map-core.md](code-map-core.md) |
| CAN, Ethernet and LIN buses | [code-map-buses.md](code-map-buses.md) |
| J1939 engine, parts, codec and spy | [code-map-j1939.md](code-map-j1939.md) |
| ECU, Peer, local execution and Simulation | [code-map-execution.md](code-map-execution.md) |
| Launcher, process, configuration and run | [code-map-launcher.md](code-map-launcher.md) |
| Scenarios, timelines and batch runs | [code-map-scenario.md](code-map-scenario.md) |
| Control API, gRPC and launcher Control Server | [code-map-control.md](code-map-control.md) |
| Vector XL and Ethernet bridges | [code-map-bridges.md](code-map-bridges.md) |
| Browser GUI, protocol, client and screen examples | [code-map-gui.md](code-map-gui.md) |
| Build, packaging, dependencies and adding libraries | [code-map-build.md](code-map-build.md) |

## Find by kind of information

- Test naming, registration, execution and timeouts: [common test rules](code-map-testing.md#test-names-and-how-to-run-tests)
- Test labels and `ctest -R`: use "Test labels" and "Code and tests by directory" in the relevant area. See [common labels](code-map-testing.md#test-labels); LIN is in buses and virtual time is in execution. For the GUI, use the verification scripts named on its page.
- Dependencies: [layers and diagram legend](code-map-core.md#dependency-direction). Detailed diagrams are in [J1939](code-map-j1939.md#dependency-direction), [ECU and execution](code-map-execution.md#dependency-direction), [Control API and launcher](code-map-control.md#dependency-direction), [bridges](code-map-bridges.md#dependency-direction), and [build and other executables](code-map-build.md#dependency-direction).
- [Boundary tests](code-map-testing.md#boundary-tests)
- [Documentation examples and tests](code-map-testing.md#documentation-examples-and-tests)
- Change checklists: see "What to update together" in the relevant area. For a cross-area change, follow links from the specific entry.

## Area page format

Each entry gives, in this order, a table of the main classes and functions (headers as paths under `include/`), the test directory and the regular expression to pass to `ctest -R`.

Each entry lists, in this order, where to change, the documentation to update with it, the code to update with it, the rules to keep and the tests (headings that do not apply are left out). Items marked "keep in sync by hand" are not compared by any test.

When adding an entry to an area page, keep its change guidance and relevant references there. Do not register every heading in this index or add a reverse link for every link.

## See also

| Document | Details |
|---|---|
| [AGENTS.md](../AGENTS.md) | Documentation rules, commits, verification scripts (read it first) |
| [architecture.md](architecture.md) | The overall structure, and what each directory and each target contains |
| The documents in [internals/](architecture.md#internals) | The internals of the places you change |
