include_guard(GLOBAL)

# Warning flags for vehicle-sil's own targets (not for the third-party code).
function(vehicle_sil_set_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
    # Always an error, also without VEHICLE_SIL_WARNINGS_AS_ERRORS: a switch over an enumeration
    # without a default must handle every enumerator (a new network kind or use is found where it
    # is missing: vehicle_sil/ecu/describe.hpp). Only vehicle-sil's own targets, not a user's.
    target_compile_options(${target} PRIVATE -Werror=switch)
    if(VEHICLE_SIL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Adds -fsanitize=<VEHICLE_SIL_SANITIZERS> to every target defined after this
# call in the calling directory and below, including the dependencies.
function(vehicle_sil_enable_sanitizers)
  if("${VEHICLE_SIL_SANITIZERS}" STREQUAL "")
    return()
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "VEHICLE_SIL_SANITIZERS requires GCC or Clang")
  endif()
  list(JOIN VEHICLE_SIL_SANITIZERS "," sanitizers)
  message(STATUS "vehicle-sil: building with -fsanitize=${sanitizers}")
  add_compile_options(-fsanitize=${sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${sanitizers})
endfunction()

# Settings every vehicle-sil target uses: C++20 without compiler extensions and the
# warning flags above. Libraries require C++20 of their users too.
function(vehicle_sil_target_defaults target)
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "EXECUTABLE")
    target_compile_features(${target} PRIVATE cxx_std_20)
  else()
    target_compile_features(${target} PUBLIC cxx_std_20)
  endif()
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  vehicle_sil_set_warnings(${target})
endfunction()

# Queries `${compiler} [<driver-flags>...] -print-file-name=<name>` and sets <out_var> to the
# normalized absolute existing file path, or "" if the driver could not locate it.
function(_vehicle_sil_compiler_print_file compiler name out_var)
  set(${out_var} "" PARENT_SCOPE)
  if("${compiler}" STREQUAL "" OR "${name}" STREQUAL "")
    return()
  endif()
  execute_process(
    COMMAND "${compiler}" ${ARGN} "-print-file-name=${name}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _rc EQUAL 0 OR "${_out}" STREQUAL "")
    return()
  endif()
  string(REPLACE "\r" "" _out "${_out}")
  string(REPLACE "\n" "" _out "${_out}")
  cmake_path(SET _out NORMALIZE "${_out}")
  if(_out STREQUAL "${name}")
    return()
  endif()
  cmake_path(IS_ABSOLUTE _out _is_abs)
  if(NOT _is_abs)
    return()
  endif()
  if(EXISTS "${_out}" AND NOT IS_DIRECTORY "${_out}")
    set(${out_var} "${_out}" PARENT_SCOPE)
  endif()
endfunction()

# Appends normalized existing directory <dir> to <list_var> in caller scope if not already present.
function(_vehicle_sil_append_existing_dir list_var dir)
  if("${dir}" STREQUAL "")
    return()
  endif()
  cmake_path(SET _norm NORMALIZE "${dir}")
  if(IS_DIRECTORY "${_norm}")
    set(_items "${${list_var}}")
    if(NOT "${_norm}" IN_LIST _items)
      list(APPEND _items "${_norm}")
      set(${list_var} "${_items}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

# Resolves MinGW compiler runtime DLLs matching `compiler_id` (and for Clang, whether `stdc++` is
# in `implicit_link_libs`, e.g. MSYS2 UCRT64 clang++ vs CLANG64 libc++) via `-print-file-name=<dll>`
# with optional `compiler_target` (ARGV5) / `sysroot` (ARGV6) forwarding plus sysroot,
# static-archive `../bin`, implicit link directories, and compiler REALPATH fallbacks.
function(_vehicle_sil_find_mingw_runtime_dlls compiler compiler_id implicit_link_dirs implicit_link_libs out_var)
  set(_compiler_target "")
  if(ARGC GREATER 5)
    set(_compiler_target "${ARGV5}")
  endif()
  set(_explicit_sysroot "")
  if(ARGC GREATER 6)
    set(_explicit_sysroot "${ARGV6}")
  endif()
  set(${out_var} "" PARENT_SCOPE)
  set(_use_libstdcxx FALSE)
  if(compiler_id STREQUAL "GNU")
    set(_use_libstdcxx TRUE)
  elseif(compiler_id STREQUAL "Clang" AND "stdc++" IN_LIST implicit_link_libs)
    set(_use_libstdcxx TRUE)
  endif()

  if(_use_libstdcxx)
    set(_candidates
        libstdc++-6.dll
        libgcc_s_seh-1.dll
        libgcc_s_dw2-1.dll
        libgcc_s_sjlj-1.dll
        libwinpthread-1.dll)
    set(_probe_archives
        libstdc++.dll.a
        libstdc++.a
        libgcc_s.a
        libgcc.a
        libwinpthread.dll.a
        libwinpthread.a)
  elseif(compiler_id STREQUAL "Clang")
    set(_candidates
        libc++.dll
        libunwind.dll
        libwinpthread-1.dll)
    set(_probe_archives
        libc++.dll.a
        libc++.a
        libunwind.dll.a
        libunwind.a
        libwinpthread.dll.a
        libwinpthread.a)
  else()
    return()
  endif()

  set(_probe_args "")
  if(compiler_id STREQUAL "Clang" AND NOT "${_compiler_target}" STREQUAL "")
    list(APPEND _probe_args "--target=${_compiler_target}")
  endif()
  if(NOT "${_explicit_sysroot}" STREQUAL "")
    list(APPEND _probe_args "--sysroot=${_explicit_sysroot}")
  endif()

  set(_fallback_dirs "")
  # 1. Explicit CMAKE_SYSROOT and compiler `-print-sysroot` bin directories.
  if(NOT "${_explicit_sysroot}" STREQUAL "")
    _vehicle_sil_append_existing_dir(_fallback_dirs "${_explicit_sysroot}/bin")
    _vehicle_sil_append_existing_dir(_fallback_dirs "${_explicit_sysroot}/mingw/bin")
  endif()
  if(NOT "${compiler}" STREQUAL "")
    execute_process(
      COMMAND "${compiler}" ${_probe_args} -print-sysroot
      RESULT_VARIABLE _sysroot_rc
      OUTPUT_VARIABLE _sysroot
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_sysroot_rc EQUAL 0 AND NOT "${_sysroot}" STREQUAL "")
      string(REPLACE "\r" "" _sysroot "${_sysroot}")
      string(REPLACE "\n" "" _sysroot "${_sysroot}")
      _vehicle_sil_append_existing_dir(_fallback_dirs "${_sysroot}/bin")
      _vehicle_sil_append_existing_dir(_fallback_dirs "${_sysroot}/mingw/bin")
    endif()
  endif()
  # 2. Static/import archive sibling `../bin` (and archive directory itself).
  foreach(_archive IN LISTS _probe_archives)
    _vehicle_sil_compiler_print_file("${compiler}" "${_archive}" _archive_path ${_probe_args})
    if(NOT "${_archive_path}" STREQUAL "")
      cmake_path(GET _archive_path PARENT_PATH _archive_dir)
      _vehicle_sil_append_existing_dir(_fallback_dirs "${_archive_dir}/../bin")
      _vehicle_sil_append_existing_dir(_fallback_dirs "${_archive_dir}")
    endif()
  endforeach()
  # 3. Implicit link directories (`../bin` and the directory itself).
  foreach(_link_dir IN LISTS implicit_link_dirs)
    _vehicle_sil_append_existing_dir(_fallback_dirs "${_link_dir}/../bin")
    _vehicle_sil_append_existing_dir(_fallback_dirs "${_link_dir}")
  endforeach()
  # 4. Compiler REALPATH parent directory (and `../bin`).
  if(NOT "${compiler}" STREQUAL "" AND EXISTS "${compiler}")
    file(REAL_PATH "${compiler}" _compiler_real)
    cmake_path(GET _compiler_real PARENT_PATH _compiler_dir)
    _vehicle_sil_append_existing_dir(_fallback_dirs "${_compiler_dir}")
    _vehicle_sil_append_existing_dir(_fallback_dirs "${_compiler_dir}/../bin")
  endif()

  set(_resolved "")
  foreach(_dll IN LISTS _candidates)
    _vehicle_sil_compiler_print_file("${compiler}" "${_dll}" _found ${_probe_args})
    if("${_found}" STREQUAL "")
      foreach(_dir IN LISTS _fallback_dirs)
        if(EXISTS "${_dir}/${_dll}" AND NOT IS_DIRECTORY "${_dir}/${_dll}")
          cmake_path(SET _found NORMALIZE "${_dir}/${_dll}")
          break()
        endif()
      endforeach()
    endif()
    if(NOT "${_found}" STREQUAL "" AND NOT "${_found}" IN_LIST _resolved)
      list(APPEND _resolved "${_found}")
    endif()
  endforeach()

  set(${out_var} "${_resolved}" PARENT_SCOPE)
endfunction()

# Cached per-configure wrapper around _vehicle_sil_find_mingw_runtime_dlls().
function(_vehicle_sil_resolve_mingw_runtime_dlls out_var)
  set(${out_var} "" PARENT_SCOPE)
  if(NOT (WIN32 AND MINGW AND NOT VEHICLE_SIL_STATIC))
    return()
  endif()
  get_property(_cached GLOBAL PROPERTY _VEHICLE_SIL_MINGW_RUNTIME_DLLS_CACHED)
  if(NOT _cached)
    _vehicle_sil_find_mingw_runtime_dlls(
      "${CMAKE_CXX_COMPILER}"
      "${CMAKE_CXX_COMPILER_ID}"
      "${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}"
      "${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES}"
      _dlls
      "${CMAKE_CXX_COMPILER_TARGET}"
      "${CMAKE_SYSROOT}")
    set_property(GLOBAL PROPERTY _VEHICLE_SIL_MINGW_RUNTIME_DLLS "${_dlls}")
    set_property(GLOBAL PROPERTY _VEHICLE_SIL_MINGW_RUNTIME_DLLS_CACHED TRUE)
  else()
    get_property(_dlls GLOBAL PROPERTY _VEHICLE_SIL_MINGW_RUNTIME_DLLS)
  endif()
  set(${out_var} "${_dlls}" PARENT_SCOPE)
endfunction()

# On Windows, copies the DLLs a target depends on (SilKit.dll and, for non-static MinGW builds,
# the toolchain's C++/GCC/Clang/winpthread runtime DLLs) next to it after each build so that it
# can be started outside an MSYS2 shell without changing PATH. No-op elsewhere; when
# VEHICLE_SIL_STATIC=ON, adds no compiler runtime DLLs.
function(vehicle_sil_copy_runtime_dlls target)
  if(WIN32)
    set(_mingw_dlls "")
    if(MINGW AND NOT VEHICLE_SIL_STATIC)
      _vehicle_sil_resolve_mingw_runtime_dlls(_mingw_dlls)
    endif()
    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND}
              "-DDEST_DIR=$<TARGET_FILE_DIR:${target}>"
              "-DLOCK_DIR=${CMAKE_BINARY_DIR}/CMakeFiles"
              -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VehicleSilCopyDlls.cmake"
              -- $<TARGET_RUNTIME_DLLS:${target}> ${_mingw_dlls}
      COMMAND_EXPAND_LISTS
      VERBATIM)
  endif()
endfunction()
