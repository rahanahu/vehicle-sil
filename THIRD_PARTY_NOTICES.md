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
