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
  set(SILKIT_BUILD_STATIC OFF)
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

  foreach(patch IN LISTS patches)
    # git apply outside a repository works like patch(1):
    # - GIT_CEILING_DIRECTORIES keeps git from finding an enclosing repository
    #   (such as vehicle-sil itself when the build tree is inside it); there,
    #   git apply would resolve the paths against that repository and skip them.
    # - GIT_DIR and friends, inherited for example from a git hook, would point
    #   it at another repository.
    # --ignore-whitespace tolerates CRLF checkouts (core.autocrlf on Windows).
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE
              --unset=GIT_COMMON_DIR "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}/external"
              "${GIT_EXECUTABLE}" apply --ignore-whitespace --verbose "${patch}"
      WORKING_DIRECTORY "${patched}"
      RESULT_VARIABLE result
      OUTPUT_VARIABLE output
      ERROR_VARIABLE output)
    if(NOT result EQUAL 0 OR output MATCHES "Skipped patch")
      message(FATAL_ERROR "vehicle-sil: ${patch} does not apply to ${source}:\n${output}")
    endif()
  endforeach()
  file(WRITE "${stamp_file}" "${stamp_content}")
  set(${out_var} "${patched}" PARENT_SCOPE)
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
  # The public headers are attached to SilKit::SilKitInterface (SilKit/include/CMakeLists.txt).
  _vehicle_sil_mark_system_includes(SilKit::SilKit SilKit::SilKitInterface)
  # SIL Kit's YAML/JSON parser, used by vehicle_sil::can_netsim for the network simulator
  # configuration file.
  if(TARGET rapidyaml)
    _vehicle_sil_mark_system_includes(rapidyaml)
  endif()
endif()
