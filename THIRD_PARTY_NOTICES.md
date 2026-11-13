# Third-party notices

vehicle-sil is MIT licensed (`LICENSE`); the files of the ECU project template
(`template/`) alone are MIT-0 (`template/LICENSE`).

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
BoringSSL (Apache-2.0, with some older OpenSSL and ISC terms) and zlib (zlib license). They are not part of this
repository: they come from the system or are fetched at a pinned version by
CMake (see `docs/dependencies.md`). Distributions of binaries built with them
must carry their notices.

## The prebuilt tools archives

The tools archives of a release (`vehicle-sil-tools-<version>-<platform>`, made
by `scripts/package.py tools`) contain, besides vehicle-sil itself (MIT,
`LICENSE`), the following. The archive holds their license texts under
`licenses/`.

| Component | Where in the archive | License | License texts in the archive |
|---|---|---|---|
| Vector SIL Kit | linked statically into `bin/vehicle_sil_launch` | MIT | `licenses/sil-kit/LICENSE` |
| asio, fmt, spdlog, and rapidyaml with what is amalgamated into it (c4core, fast_float, debugbreak and others), compiled into SIL Kit and `vehicle_sil_launch` | in `bin/vehicle_sil_launch` | BSL-1.0, MIT, MIT; rapidyaml and the rest as listed | `licenses/sil-kit/ThirdParty/` (`LICENSES.rst` lists them; `rapidyaml-notices.txt` holds the notices of the amalgamated header) |
| gRPC and what it compiles in (Protocol Buffers, utf8_range, abseil, re2, c-ares, BoringSSL, zlib, address_sorting, xxhash), built from source at the pinned version and linked statically (Linux and Windows) | `bin/vehicle_sil_launch` | Apache-2.0 (with gRPC's `NOTICE.txt`), BSD-3-Clause, MIT, Apache-2.0, BSD-3-Clause, MIT, Apache-2.0 (with some older OpenSSL and ISC terms), zlib, BSD-3-Clause, BSD-2-Clause | `licenses/grpc/` |
| The GCC runtime (libstdc++, libgcc), linked statically | `bin/vehicle_sil_launch` | GPL-3.0 with the GCC Runtime Library Exception | Linux: none required; Windows: `licenses/msys2/gcc/` |
| Windows: winpthreads and the mingw-w64 C runtime and start-up code, linked statically (in every MinGW executable) | `bin/vehicle_sil_launch.exe` | winpthreads: MIT and BSD style; mingw-w64 runtime: as its COPYING files state | `licenses/msys2/` (`PACKAGES.txt` names the package of each part) |
| The browser GUI's screen: Angular, Angular CDK and Material, RxJS, tslib | `python/vehicle_sil_gui/web/` | MIT, Apache-2.0 (RxJS), 0BSD (tslib) | `licenses/web/3rdpartylicenses.txt` (written by the screen's build) |

Not in the archives: the Python packages of the GUI server and of the Control
API client (aiohttp and its dependencies, PyYAML, grpcio, protobuf).
`python/requirements.txt` pins them with hashes, and uv or pip installs them
from PyPI. The gRPC stubs in `python/vehicle_sil_control/_generated` are
generated from vehicle-sil's own `proto/` files.

## Distributing executables built with `VEHICLE_SIL_STATIC`

An executable linked with `VEHICLE_SIL_STATIC=ON` (README, "1 つの実行ファイルにする")
contains SIL Kit and the code SIL Kit compiles in, and the C++ runtime. Whoever
distributes it ships these notices with it:

- vehicle-sil: `LICENSE` (MIT). The vehicle-sil libraries are static, so this
  holds for every ECU executable, static or not.
- SIL Kit: `external/sil-kit/LICENSE`, and the notices of what it compiles in:
  `external/sil-kit/ThirdParty/LICENSES.rst`, the license files of fmt, spdlog
  and asio under `external/sil-kit/ThirdParty/`, and the notices in
  `external/sil-kit/ThirdParty/rapidyaml/rapidyaml.hpp`. The tools archive's
  `licenses/sil-kit/` holds this set, with those notices extracted into
  `rapidyaml-notices.txt`, ready to copy.
- Linux: libstdc++ and libgcc come under the GCC Runtime Library Exception,
  which asks no notice for programs compiled with GCC.
- Windows (MinGW): libstdc++ and libgcc as above; winpthreads and the
  mingw-w64 C runtime are linked in as well (the runtime and start-up code are
  in every MinGW executable, static or not): ship the files MSYS2 installs in
  `/ucrt64/share/licenses/winpthreads/` and `/ucrt64/share/licenses/crt/` (the
  tools archive has them in `licenses/msys2/`).
