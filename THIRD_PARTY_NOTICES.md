# Third-party notices

vehicle-sil builds on the following project, included as a git submodule. Its
license text is kept unmodified in the submodule.

| Component | Location | License | License file |
|---|---|---|---|
| Vector SIL Kit | `external/sil-kit` | MIT | `external/sil-kit/LICENSE` |

vehicle-sil builds SIL Kit with the patches in `patches/sil-kit/` applied (to a
copy in the build tree); the patches are offered to the SIL Kit project under
its MIT license.

SIL Kit in turn bundles third-party code (asio, fmt, spdlog, rapidyaml,
googletest); see `external/sil-kit/ThirdParty/LICENSES.rst`.

With `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`, the control server and the control
agent's client link gRPC (Apache-2.0) and its dependencies: Protocol Buffers
(BSD-3-Clause), abseil (Apache-2.0), re2 (BSD-3-Clause), c-ares (MIT),
BoringSSL (OpenSSL/ISC style) and zlib (zlib license). They are not part of this
repository: they come from the system or are fetched at a pinned version by
CMake (see `docs/dependencies.md`). Distributions of binaries built with them
must carry their notices.
