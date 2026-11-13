include_guard(GLOBAL)

# Adds Vector SIL Kit from the pinned git submodule in external/. It is added
# with EXCLUDE_FROM_ALL, so only the targets that vehicle-sil (or a consumer)
# links against are built. It is built from a patched copy in the build tree
# (patches/sil-kit/).
#
# Targets provided (verified against the pinned sources, see docs/dependencies.md):
#   SilKit::SilKit                 SIL Kit shared library (SilKit/source/CMakeLists.txt)
#   sil-kit-registry               SIL Kit registry executable (Utilities/SilKitRegistry)

set(VEHICLE_SIL_EXTERNAL_DIR "${CMAKE_CURRENT_LIST_DIR}/../external")
cmake_path(NORMAL_PATH VEHICLE_SIL_EXTERNAL_DIR)
set(VEHICLE_SIL_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../patches")
cmake_path(NORMAL_PATH VEHICLE_SIL_PATCH_DIR)

function(_vehicle_sil_require_file path)
  if(NOT EXISTS "${VEHICLE_SIL_EXTERNAL_DIR}/${path}")
    message(
      FATAL_ERROR
        "vehicle-sil: missing ${VEHICLE_SIL_EXTERNAL_DIR}/${path}\n"
        "The git submodules are not checked out. Run:\n"
        "  git submodule update --init --recursive\n"
        "(FetchContent does this automatically unless GIT_SUBMODULES is set to an empty list.)")
  endif()
endfunction()

function(_vehicle_sil_add_silkit)
  _vehicle_sil_require_file("sil-kit/CMakeLists.txt")
  _vehicle_sil_require_file("sil-kit/ThirdParty/asio/asio/include/asio.hpp")
  _vehicle_sil_require_file("sil-kit/ThirdParty/fmt/CMakeLists.txt")
  _vehicle_sil_require_file("sil-kit/ThirdParty/spdlog/CMakeLists.txt")

  # SIL Kit's options are plain option() calls and its cmake_minimum_required
  # is >= 3.13, so (policy CMP0077) these normal variables take precedence
  # without touching the consumer's cache. They only live in this function.
  set(SILKIT_BUILD_DEMOS OFF)
  set(SILKIT_BUILD_TESTS OFF)
  set(SILKIT_BUILD_DOCS OFF)
  set(SILKIT_BUILD_DASHBOARD OFF)
  set(SILKIT_INSTALL_SOURCE OFF)
  set(SILKIT_WARNINGS_AS_ERRORS OFF)
  set(SILKIT_USE_SYSTEM_LIBRARIES OFF)
  # VEHICLE_SIL_STATIC: one executable per ECU, without libSilKit.so / SilKit.dll next to it.
  if(VEHICLE_SIL_STATIC)
    set(SILKIT_BUILD_STATIC ON)
  else()
    set(SILKIT_BUILD_STATIC OFF)
  endif()
  # Keeps the sil-kit-registry target available; EXCLUDE_FROM_ALL means it is
  # only built when something depends on it (the integration tests do).
  set(SILKIT_BUILD_UTILITIES ON)

  _vehicle_sil_patched_source(sil-kit silkit_source)
  add_subdirectory("${silkit_source}" "${CMAKE_CURRENT_BINARY_DIR}/external/sil-kit" EXCLUDE_FROM_ALL)
endfunction()

# Copies the pinned submodule external/<dependency> to the build tree and
# applies the patches in patches/<dependency>/ (in file name order) to the
# copy, so the submodule's working tree stays untouched. The copy is redone
# only when the content of the patches or of the submodule's files (not
# counting .git, also in nested submodules) differs from the last configure,
# so a submodule checkout, local edits in it and added or removed patches are
# all picked up; CMake reruns the configure step when any of those files
# changes. Sets <out_var> to the source directory to build: the submodule
# itself if there are no patches.
function(_vehicle_sil_patched_source dependency out_var)
  set(source "${VEHICLE_SIL_EXTERNAL_DIR}/${dependency}")
  file(GLOB patches LIST_DIRECTORIES false CONFIGURE_DEPENDS "${VEHICLE_SIL_PATCH_DIR}/${dependency}/*.patch")
  list(SORT patches)
  if(NOT patches)
    set(${out_var} "${source}" PARENT_SCOPE)
    return()
  endif()

  # The stamp lists every input file with its hash. It needs no git, so it also
  # works for a source tree that is not a git checkout.
  file(GLOB_RECURSE source_files LIST_DIRECTORIES false CONFIGURE_DEPENDS RELATIVE "${source}" "${source}/*")
  list(FILTER source_files EXCLUDE REGEX "(^|/)\\.git(/|$)")
  list(SORT source_files)
  set(stamp_content "")
  foreach(file IN LISTS source_files)
    file(SHA256 "${source}/${file}" hash)
    string(APPEND stamp_content "source ${file} ${hash}\n")
  endforeach()
  foreach(patch IN LISTS patches)
    file(SHA256 "${patch}" hash)
    cmake_path(GET patch FILENAME patch_name)
    string(APPEND stamp_content "patch ${patch_name} ${hash}\n")
  endforeach()
  # Rerun CMake when a patch or a submodule file changes (the globs above add
  # files that appear or disappear).
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${patches})
  list(TRANSFORM source_files PREPEND "${source}/" OUTPUT_VARIABLE source_paths)
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source_paths})

  set(patched "${CMAKE_CURRENT_BINARY_DIR}/external/${dependency}-patched")
  set(stamp_file "${CMAKE_CURRENT_BINARY_DIR}/external/${dependency}-patched.stamp")
  if(EXISTS "${stamp_file}")
    file(READ "${stamp_file}" previous_stamp)
    if(previous_stamp STREQUAL stamp_content AND IS_DIRECTORY "${patched}")
      set(${out_var} "${patched}" PARENT_SCOPE)
      return()
    endif()
  endif()

  find_package(Git REQUIRED)
  message(STATUS "vehicle-sil: applying ${VEHICLE_SIL_PATCH_DIR}/${dependency}/*.patch to a copy of ${dependency}")
  file(REMOVE "${stamp_file}")
  file(REMOVE_RECURSE "${patched}")
  file(COPY "${source}/" DESTINATION "${patched}" PATTERN ".git" EXCLUDE)
  # file(COPY) keeps the timestamps. Touch every file so that a file which a
  # previous patch set changed, and which is back to its older original now,
  # is still newer than its object file and gets rebuilt.
  file(GLOB_RECURSE copied LIST_DIRECTORIES false "${patched}/*")
  foreach(file IN LISTS copied)
    file(TOUCH_NOCREATE "${file}")
  endforeach()

  # git apply outside a repository works like patch(1):
  # - GIT_CEILING_DIRECTORIES keeps git from finding an enclosing repository
  #   (such as vehicle-sil itself when the build tree is inside it); there,
  #   git apply would resolve the paths against that repository and skip them.
  #   The ceiling is the directory above the copy in git's own form, as git
  #   prints it: MSYS2's git takes POSIX paths (/d/a/...) in a list separated
  #   by ':', so a Windows path (D:/a/...) would not stop it.
  # - GIT_DIR and friends, inherited for example from a git hook, would point
  #   it at another repository.
  set(git_env "${CMAKE_COMMAND}" -E env --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE
              --unset=GIT_COMMON_DIR)
  set(ceiling "${CMAKE_CURRENT_BINARY_DIR}/external")
  execute_process(
    COMMAND ${git_env} "${GIT_EXECUTABLE}" rev-parse --show-toplevel --show-prefix
    WORKING_DIRECTORY "${ceiling}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_QUIET)
  if(result EQUAL 0)
    string(REPLACE "\r" "" output "${output}")
    string(REGEX REPLACE "\n$" "" output "${output}")
    string(REPLACE "\n" ";" output "${output}")
    list(GET output 0 top)
    list(LENGTH output count)
    set(ceiling "${top}")
    if(count GREATER 1)
      list(GET output 1 prefix)
      string(REGEX REPLACE "/$" "" prefix "${prefix}")
      if(NOT prefix STREQUAL "")
        set(ceiling "${top}/${prefix}")
      endif()
    endif()
  endif()
  list(APPEND git_env "GIT_CEILING_DIRECTORIES=${ceiling}")

  foreach(patch IN LISTS patches)
    # --ignore-whitespace tolerates CRLF checkouts (core.autocrlf on Windows).
    execute_process(
      COMMAND ${git_env} "${GIT_EXECUTABLE}" apply --ignore-whitespace --verbose "${patch}"
      WORKING_DIRECTORY "${patched}"
      RESULT_VARIABLE result
      OUTPUT_VARIABLE output
      ERROR_VARIABLE output)
    if(NOT result EQUAL 0 OR output MATCHES "Skipped patch")
      _vehicle_sil_patch_failure_details("${patched}" "${patch}" "${ceiling}" "${git_env}" details)
      message(FATAL_ERROR "vehicle-sil: ${patch} does not apply to ${source}:\n${output}\n${details}")
    endif()
  endforeach()
  file(WRITE "${stamp_file}" "${stamp_content}")
  set(${out_var} "${patched}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to what explains a patch that did not apply to the copy in
# <patched>: the git used, the ceiling, the repository git finds around the
# copy (if any, git took the patch's paths as relative to it and skipped them)
# and the line ends of the files the patch changes.
function(_vehicle_sil_patch_failure_details patched patch ceiling git_env out_var)
  execute_process(
    COMMAND ${git_env} "${GIT_EXECUTABLE}" rev-parse --show-toplevel
    WORKING_DIRECTORY "${patched}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE enclosing
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT result EQUAL 0 OR enclosing STREQUAL "")
    set(enclosing "none (as intended)")
  else()
    string(APPEND enclosing " (git took the patch's paths as relative to this repository and skipped them: "
           "the ceiling did not hide it; build outside the repository, or with another git)")
  endif()
  string(CONCAT text "What git saw:\n  git: ${GIT_EXECUTABLE} (version ${GIT_VERSION_STRING})\n"
         "  GIT_CEILING_DIRECTORIES: ${ceiling}\n  repository around the copy: ${enclosing}\n"
         "  line ends of the files the patch changes, in the copy:")
  file(STRINGS "${patch}" targets REGEX "^diff --git a/")
  foreach(line IN LISTS targets)
    string(REGEX REPLACE "^diff --git a/([^ ]+) .*" "\\1" path "${line}")
    if(NOT EXISTS "${patched}/${path}")
      set(ends "missing")
    else()
      # As hex (file(READ) as text drops the CRs): byte pairs, so the match starts at an even offset.
      file(READ "${patched}/${path}" head LIMIT 4096 HEX)
      if(head MATCHES "^(..)*0d0a")
        set(ends "CRLF")
      elseif(head MATCHES "^(..)*0a")
        set(ends "LF")
      else()
        set(ends "no line end in the first 4096 bytes")
      endif()
    endif()
    string(APPEND text "\n    ${path}: ${ends}")
  endforeach()
  string(APPEND text "\nIf the files differ from what the patch expects, check that external/sil-kit is at the commit "
         "vehicle-sil pins (git submodule update --init --recursive).")
  set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# Marks the public include directories of a dependency built from source as
# system include directories, so that warnings from third-party headers do not
# show up in (or break, with -Werror) the targets that include them.
# (The SYSTEM target property would do the same but needs CMake 3.25.)
function(_vehicle_sil_mark_system_includes)
  foreach(target IN LISTS ARGN)
    get_target_property(real "${target}" ALIASED_TARGET)
    if(NOT real)
      set(real "${target}")
    endif()
    get_target_property(imported "${real}" IMPORTED)
    if(imported)
      continue()
    endif()
    get_target_property(includes "${real}" INTERFACE_INCLUDE_DIRECTORIES)
    if(includes)
      set_property(TARGET "${real}" APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${includes})
    endif()
  endforeach()
endfunction()

if(NOT TARGET SilKit::SilKit)
  _vehicle_sil_add_silkit()
  if(VEHICLE_SIL_STATIC)
    # Whatever links SIL Kit (the ECU executables, the tools) also links the compiler's runtime
    # statically, so that the executable needs no DLL or .so of the toolchain either. MinGW: -static
    # takes libstdc++, libgcc and winpthread (and static archives of other libraries where they
    # exist). Linux: libstdc++ and libgcc; glibc stays shared (a fully static glibc program cannot
    # load NSS modules and is not portable either).
    get_target_property(_vehicle_sil_silkit SilKit::SilKit ALIASED_TARGET)
    if(NOT _vehicle_sil_silkit)
      set(_vehicle_sil_silkit SilKit::SilKit)
    endif()
    if(MINGW)
      target_link_options(${_vehicle_sil_silkit} INTERFACE -static)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
      target_link_options(${_vehicle_sil_silkit} INTERFACE -static-libstdc++ -static-libgcc)
    endif()
    message(STATUS "vehicle-sil: VEHICLE_SIL_STATIC: SIL Kit static, the C++ runtime linked statically")
  endif()
  # The public headers are attached to SilKit::SilKitInterface (SilKit/include/CMakeLists.txt).
  _vehicle_sil_mark_system_includes(SilKit::SilKit SilKit::SilKitInterface)
  # SIL Kit's YAML/JSON parser, used by vehicle_sil::can_netsim for the network simulator
  # configuration file.
  if(TARGET rapidyaml)
    _vehicle_sil_mark_system_includes(rapidyaml)
  endif()
endif()
