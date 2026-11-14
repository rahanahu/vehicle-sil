# Patches to upstream dependencies

vehicle-sil does not modify the working tree of `external/sil-kit`. A fix or feature that vehicle-sil needs from an
upstream library is kept here as a `git format-patch` file named
`<dependency>/NNNN-<topic>.patch`, applied in file name order.

| Dependency | Patches | Why |
|---|---|---|
| SIL Kit | `sil-kit/0001` | LIN: several dynamic-response slaves on one identifier (`LinSlave`); see `docs/dependencies.md`, "SIL Kit へのパッチ" |
| SIL Kit | `sil-kit/0002` | CAN: a thread-safe switch to the detailed simulation (the simple CAN network simulator); see `docs/dependencies.md`, "SIL Kit へのパッチ" |
| SIL Kit | `sil-kit/0003` | Registry: a peer refused for its name does not remove the entry of the participant that has the name (relayed connections keep working); see `docs/dependencies.md`, "SIL Kit へのパッチ" |
| SIL Kit | `sil-kit/0004` | Network simulation: the announcement of a simulated network is not lost when the simulator's subscriptions complete at the same moment (the simple CAN network simulator); see `docs/dependencies.md`, "SIL Kit へのパッチ" |
| SIL Kit | `sil-kit/0005` | Registry: a participant whose connection is already closed is not advertised to a joining participant (the join no longer times out); see `docs/dependencies.md`, "SIL Kit へのパッチ" |

`cmake/VehicleSilDependencies.cmake` copies a submodule that has patches into
the build tree, applies the patches to the copy and builds the copy.

## Rules

Every patch

- is one upstream commit of a series on the pinned submodule commit, in the
  upstream code style, with the upstream tests that reproduce the problem or
  cover the feature. The series applies in order: a patch may depend on
  earlier ones (code or test context), so submitting one upstream means
  rebasing it on upstream's main;
- comes with a test in this repository that fails without it, when vehicle-sil
  relies on its behaviour;
- has an entry in `docs/dependencies.md` that says why vehicle-sil needs it and
  whether it was reported upstream.

Remove a patch when the submodule is updated to an upstream commit that
contains it.

## Creating or updating the patches

The pinned commit is the tag `v5.0.8`, the upstream style is its
`.clang-format`, and the upstream tests are built with
`-DSILKIT_BUILD_TESTS=ON` (`SilKitUnitTests`, `SilKitIntegrationTests`).

Work in a separate clone, not in the submodule:

```sh
git clone external/sil-kit <work>
cd <work>
git checkout -b vehicle-sil-patches v5.0.8
git submodule update --init                       # SIL Kit's third-party submodules
git am <vehicle-sil>/patches/sil-kit/*.patch      # the current series
# edit, commit (one commit per patch), run the upstream tests:
#   cmake -B build -DSILKIT_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
# format like upstream: clang-format (.clang-format)
git format-patch --zero-commit --no-signature v5.0.8..vehicle-sil-patches \
  -o <vehicle-sil>/patches/sil-kit
```

Delete the old patch files first when the series changes, so that no stale
patch stays behind.
