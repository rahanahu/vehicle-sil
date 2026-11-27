# SPDX-License-Identifier: MIT
#
# Host-runnable tests for cmake/VehicleSilCopyDlls.cmake and the MinGW runtime DLL resolver in
# cmake/VehicleSilHelpers.cmake. Invoked by CTest with cmake -P:
#   -DROOT=<repo root>
#   -DWORK=<scratch directory>
#   [-DPYTHON=<python executable for parallel contention check>]
cmake_minimum_required(VERSION 3.21)

foreach(_var ROOT WORK)
  if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
    message(FATAL_ERROR "test_copy_dlls.cmake: ${_var} is required")
  endif()
endforeach()

set(_script "${ROOT}/cmake/VehicleSilCopyDlls.cmake")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# --- 1. Empty DLL list after `--` is a clean no-op (VEHICLE_SIL_STATIC=ON behavior) ---
set(_empty_dest "${WORK}/empty_dest")
set(_empty_lock "${WORK}/empty_lock")
execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DDEST_DIR=${_empty_dest}" "-DLOCK_DIR=${_empty_lock}" -P "${_script}" --
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "empty DLL list failed (${_rc}): ${_out}\n${_err}")
endif()
if(EXISTS "${_empty_dest}" OR EXISTS "${_empty_lock}")
  message(FATAL_ERROR "empty DLL list must not create DEST_DIR or LOCK_DIR")
endif()

# --- 2. Multiple DLLs including spaces, deduplication (first wins), lock in LOCK_DIR only ---
set(_src_a "${WORK}/src a")
set(_src_b "${WORK}/src b")
set(_dest "${WORK}/dest dir")
set(_lock "${WORK}/lock dir")
file(MAKE_DIRECTORY "${_src_a}" "${_src_b}")
file(WRITE "${_src_a}/alpha.dll" "ALPHA-V1")
file(WRITE "${_src_a}/beta with space.dll" "BETA-V1")
file(WRITE "${_src_a}/dup.dll" "FIRST-DUP")
file(WRITE "${_src_b}/DUP.DLL" "SECOND-DUP")

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DDEST_DIR=${_dest}" "-DLOCK_DIR=${_lock}" -P "${_script}" --
          "${_src_a}/alpha.dll" "${_src_a}/beta with space.dll" "${_src_a}/dup.dll" "${_src_b}/DUP.DLL"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "basic copy failed (${_rc}): ${_out}\n${_err}")
endif()

file(READ "${_dest}/alpha.dll" _got_a)
file(READ "${_dest}/beta with space.dll" _got_b)
file(READ "${_dest}/dup.dll" _got_dup)
if(NOT _got_a STREQUAL "ALPHA-V1" OR NOT _got_b STREQUAL "BETA-V1" OR NOT _got_dup STREQUAL "FIRST-DUP")
  message(FATAL_ERROR "copied contents mismatch: a='${_got_a}' b='${_got_b}' dup='${_got_dup}'")
endif()
file(GLOB _dest_locks "${_dest}/*.lock")
if(_dest_locks)
  message(FATAL_ERROR "lock file must not be written inside DEST_DIR: ${_dest_locks}")
endif()
file(GLOB _lock_files "${_lock}/vehicle_sil_dll_*.lock")
list(LENGTH _lock_files _lock_count)
if(NOT _lock_count EQUAL 1)
  message(FATAL_ERROR "expected 1 lock file in LOCK_DIR, found ${_lock_count}: ${_lock_files}")
endif()

# --- 3. Update when source changes (ONLY_IF_DIFFERENT) and same-file no-op ---
file(WRITE "${_src_a}/alpha.dll" "ALPHA-V2")
execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DDEST_DIR=${_dest}" "-DLOCK_DIR=${_lock}" -P "${_script}" --
          "${_src_a}/alpha.dll" "${_dest}/beta with space.dll"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "update/same-file copy failed (${_rc}): ${_out}\n${_err}")
endif()
file(READ "${_dest}/alpha.dll" _got_a2)
file(READ "${_dest}/beta with space.dll" _got_b2)
if(NOT _got_a2 STREQUAL "ALPHA-V2" OR NOT _got_b2 STREQUAL "BETA-V1")
  message(FATAL_ERROR "update/same-file mismatch: a='${_got_a2}' b='${_got_b2}'")
endif()

# --- 4. Error handling: missing source DLL and missing DEST_DIR ---
execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DDEST_DIR=${_dest}" "-DLOCK_DIR=${_lock}" -P "${_script}" -- "${_src_a}/missing.dll"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT "${_out}\n${_err}" MATCHES "runtime DLL not found")
  message(FATAL_ERROR "expected missing DLL error, got rc=${_rc}: ${_out}\n${_err}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -P "${_script}" -- "${_src_a}/alpha.dll"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT "${_out}\n${_err}" MATCHES "-DDEST_DIR=<dir> is required")
  message(FATAL_ERROR "expected missing DEST_DIR error, got rc=${_rc}: ${_out}\n${_err}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DDEST_DIR=${_dest}" "-DLOCK_DIR=${_lock}" -P "${_script}" "${_src_a}/alpha.dll"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT "${_out}\n${_err}" MATCHES "unexpected argument")
  message(FATAL_ERROR "expected missing '--' separator error, got rc=${_rc}: ${_out}\n${_err}")
endif()

if(UNIX)
  set(_colon_src "${WORK}/src:colon")
  set(_colon_dest "${WORK}/dest:colon")
  file(MAKE_DIRECTORY "${_colon_src}")
  file(WRITE "${_colon_src}/gamma.dll" "GAMMA-COLON")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DDEST_DIR=${_colon_dest}" "-DLOCK_DIR=${_lock}" -P "${_script}" --
            "${_colon_src}/gamma.dll"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "colon-path copy failed (${_rc}): ${_out}\n${_err}")
  endif()
  file(READ "${_colon_dest}/gamma.dll" _got_gamma)
  if(NOT _got_gamma STREQUAL "GAMMA-COLON")
    message(FATAL_ERROR "colon-path mismatch: got '${_got_gamma}'")
  endif()
endif()

# --- 5. Parallel lock contention (8 concurrent workers targeting the same DEST_DIR) ---
if(DEFINED PYTHON AND NOT "${PYTHON}" STREQUAL "")
  set(_par_dest "${WORK}/parallel_dest")
  set(_par_lock "${WORK}/parallel_lock")
  set(_par_py
    "import concurrent.futures, pathlib, subprocess, sys\n"
    "cmake, script, dest, lock = sys.argv[1:5]\n"
    "dlls = sys.argv[5:]\n"
    "def run(_):\n"
    "    p = subprocess.run([cmake, f'-DDEST_DIR={dest}', f'-DLOCK_DIR={lock}', '-P', script, '--', *dlls], capture_output=True, text=True)\n"
    "    if p.returncode != 0:\n"
    "        raise RuntimeError(f'rc={p.returncode} out={p.stdout} err={p.stderr}')\n"
    "with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:\n"
    "    list(ex.map(run, range(16)))\n"
    "for d in dlls:\n"
    "    src = pathlib.Path(d)\n"
    "    dst = pathlib.Path(dest) / src.name\n"
    "    assert dst.read_bytes() == src.read_bytes(), f'mismatch for {src.name}'\n"
    "assert not list(pathlib.Path(dest).glob('*.lock'))\n")
  string(CONCAT _par_code ${_par_py})
  execute_process(
    COMMAND "${PYTHON}" -c "${_par_code}" "${CMAKE_COMMAND}" "${_script}" "${_par_dest}" "${_par_lock}"
            "${_src_a}/alpha.dll" "${_src_a}/beta with space.dll" "${_src_a}/dup.dll"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "parallel lock contention test failed (${_rc}): ${_out}\n${_err}")
  endif()
endif()

# --- 6. MinGW runtime DLL resolution fallbacks (_vehicle_sil_find_mingw_runtime_dlls) ---
if(UNIX)
  include("${ROOT}/cmake/VehicleSilHelpers.cmake")

  function(_write_fake_compiler path content)
    file(WRITE "${path}" "#!/bin/sh\n${content}\n")
    file(CHMOD "${path}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  endfunction()

  # 6a. GNU direct -print-file-name=<dll>
  set(_c1 "${WORK}/case_direct")
  file(MAKE_DIRECTORY "${_c1}/dlls")
  file(WRITE "${_c1}/dlls/libstdc++-6.dll" "s")
  file(WRITE "${_c1}/dlls/libgcc_s_seh-1.dll" "g")
  file(WRITE "${_c1}/dlls/libwinpthread-1.dll" "p")
  _write_fake_compiler("${_c1}/fake-g++"
"case \"\$1\" in
  -print-file-name=libstdc++-6.dll) echo '${_c1}/dlls/libstdc++-6.dll' ;;
  -print-file-name=libgcc_s_seh-1.dll) echo '${_c1}/dlls/libgcc_s_seh-1.dll' ;;
  -print-file-name=libwinpthread-1.dll) echo '${_c1}/dlls/libwinpthread-1.dll' ;;
  -print-file-name=*) echo \"\${1#-print-file-name=}\" ;;
  -print-sysroot) echo '' ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls("${_c1}/fake-g++" "GNU" "" "" _got)
  set(_want "${_c1}/dlls/libstdc++-6.dll;${_c1}/dlls/libgcc_s_seh-1.dll;${_c1}/dlls/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6a mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()

  # 6b. GNU sysroot/bin fallback
  set(_c2 "${WORK}/case_sysroot")
  file(MAKE_DIRECTORY "${_c2}/sysroot/bin")
  file(WRITE "${_c2}/sysroot/bin/libstdc++-6.dll" "s")
  file(WRITE "${_c2}/sysroot/bin/libgcc_s_dw2-1.dll" "g")
  file(WRITE "${_c2}/sysroot/bin/libwinpthread-1.dll" "p")
  _write_fake_compiler("${_c2}/fake-g++"
"case \"\$1\" in
  -print-sysroot) echo '${_c2}/sysroot' ;;
  -print-file-name=*) echo \"\${1#-print-file-name=}\" ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls("${_c2}/fake-g++" "GNU" "" "" _got)
  set(_want "${_c2}/sysroot/bin/libstdc++-6.dll;${_c2}/sysroot/bin/libgcc_s_dw2-1.dll;${_c2}/sysroot/bin/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6b mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()

  # 6c. GNU static-archive ../bin fallback
  set(_c3 "${WORK}/case_archive")
  file(MAKE_DIRECTORY "${_c3}/tc/lib" "${_c3}/tc/bin")
  file(WRITE "${_c3}/tc/lib/libstdc++.a" "a")
  file(WRITE "${_c3}/tc/bin/libstdc++-6.dll" "s")
  file(WRITE "${_c3}/tc/bin/libgcc_s_sjlj-1.dll" "g")
  file(WRITE "${_c3}/tc/bin/libwinpthread-1.dll" "p")
  _write_fake_compiler("${_c3}/fake-g++"
"case \"\$1\" in
  -print-file-name=libstdc++.a) echo '${_c3}/tc/lib/libstdc++.a' ;;
  -print-file-name=*) echo \"\${1#-print-file-name=}\" ;;
  -print-sysroot) echo '' ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls("${_c3}/fake-g++" "GNU" "" "" _got)
  set(_want "${_c3}/tc/bin/libstdc++-6.dll;${_c3}/tc/bin/libgcc_s_sjlj-1.dll;${_c3}/tc/bin/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6c mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()

  # 6d. GNU implicit link directories fallback
  set(_c4 "${WORK}/case_implicit")
  file(MAKE_DIRECTORY "${_c4}/imp/lib" "${_c4}/imp/bin")
  file(WRITE "${_c4}/imp/bin/libstdc++-6.dll" "s")
  file(WRITE "${_c4}/imp/bin/libgcc_s_seh-1.dll" "g")
  file(WRITE "${_c4}/imp/bin/libwinpthread-1.dll" "p")
  _write_fake_compiler("${_c4}/fake-g++"
"case \"\$1\" in
  -print-file-name=*) echo \"\${1#-print-file-name=}\" ;;
  -print-sysroot) echo '' ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls("${_c4}/fake-g++" "GNU" "${_c4}/imp/lib" "" _got)
  set(_want "${_c4}/imp/bin/libstdc++-6.dll;${_c4}/imp/bin/libgcc_s_seh-1.dll;${_c4}/imp/bin/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6d mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()

  # 6e. GNU compiler REALPATH parent fallback (through symlink, with spaces in path)
  set(_c5 "${WORK}/case realpath")
  file(MAKE_DIRECTORY "${_c5}/real bin" "${_c5}/sym bin")
  file(WRITE "${_c5}/real bin/libstdc++-6.dll" "s")
  file(WRITE "${_c5}/real bin/libgcc_s_seh-1.dll" "g")
  file(WRITE "${_c5}/real bin/libwinpthread-1.dll" "p")
  _write_fake_compiler("${_c5}/real bin/fake-g++"
"case \"\$1\" in
  -print-file-name=*) echo \"\${1#-print-file-name=}\" ;;
  -print-sysroot) echo '' ;;
esac")
  file(CREATE_LINK "${_c5}/real bin/fake-g++" "${_c5}/sym bin/fake-g++" SYMBOLIC)
  _vehicle_sil_find_mingw_runtime_dlls("${_c5}/sym bin/fake-g++" "GNU" "" "" _got)
  set(_want "${_c5}/real bin/libstdc++-6.dll;${_c5}/real bin/libgcc_s_seh-1.dll;${_c5}/real bin/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6e mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()

  # 6f. Clang + libc++ resolution (CLANG64: libc++.dll, libunwind.dll, libwinpthread-1.dll only)
  set(_c6 "${WORK}/case_clang_libcxx")
  file(MAKE_DIRECTORY "${_c6}/bin")
  file(WRITE "${_c6}/bin/libc++.dll" "c")
  file(WRITE "${_c6}/bin/libunwind.dll" "u")
  file(WRITE "${_c6}/bin/libwinpthread-1.dll" "p")
  file(WRITE "${_c6}/bin/libstdc++-6.dll" "unused-gnu")
  _write_fake_compiler("${_c6}/bin/fake-clang++"
"case \"\$1\" in
  -print-file-name=*) echo \"\${1#-print-file-name=}\" ;;
  -print-sysroot) echo '' ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls("${_c6}/bin/fake-clang++" "Clang" "" "c++;mingw32;moldname" _got)
  set(_want "${_c6}/bin/libc++.dll;${_c6}/bin/libunwind.dll;${_c6}/bin/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6f mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()

  # 6g. Clang + libstdc++ resolution (UCRT64 clang++: stdc++ in implicit link libraries)
  set(_c7 "${WORK}/case_clang_libstdcxx")
  file(MAKE_DIRECTORY "${_c7}/bin")
  file(WRITE "${_c7}/bin/libstdc++-6.dll" "s")
  file(WRITE "${_c7}/bin/libgcc_s_seh-1.dll" "g")
  file(WRITE "${_c7}/bin/libwinpthread-1.dll" "p")
  _write_fake_compiler("${_c7}/bin/fake-clang++"
"case \"\$1\" in
  -print-file-name=*) echo \"\${1#-print-file-name=}\" ;;
  -print-sysroot) echo '' ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls("${_c7}/bin/fake-clang++" "Clang" "" "stdc++;mingw32;gcc_s;gcc" _got)
  set(_want "${_c7}/bin/libstdc++-6.dll;${_c7}/bin/libgcc_s_seh-1.dll;${_c7}/bin/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6g mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()

  # 6h. Relative -print-file-name= return is ignored (never leaked as a relative path)
  set(_c8 "${WORK}/case_relative")
  file(MAKE_DIRECTORY "${_c8}/bin")
  _write_fake_compiler("${_c8}/bin/fake-g++"
"case \"\$1\" in
  -print-file-name=libstdc++-6.dll) echo 'cmake/VehicleSilHelpers.cmake' ;;
  -print-file-name=*) echo \"\${1#-print-file-name=}\" ;;
  -print-sysroot) echo '' ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls("${_c8}/bin/fake-g++" "GNU" "" "" _got)
  if(NOT _got STREQUAL "")
    message(FATAL_ERROR "case 6h mismatch: relative -print-file-name path must be rejected, got ${_got}")
  endif()

  # 6i. Unknown compiler ID returns empty
  _vehicle_sil_find_mingw_runtime_dlls("${_c6}/bin/fake-clang++" "MSVC" "" "" _got)
  if(NOT _got STREQUAL "")
    message(FATAL_ERROR "case 6i mismatch: expected empty, got ${_got}")
  endif()

  # 6j. COMPILER_TARGET and SYSROOT are forwarded to both -print-file-name= and -print-sysroot probes
  set(_c10 "${WORK}/case_target_sysroot_probe")
  set(_c10_sys "${_c10}/custom sysroot")
  file(MAKE_DIRECTORY "${_c10}/bin" "${_c10}/target_dlls" "${_c10}/sysroot_from_probe/bin" "${_c10_sys}")
  file(WRITE "${_c10}/target_dlls/libc++.dll" "c-target")
  file(WRITE "${_c10}/sysroot_from_probe/bin/libunwind.dll" "u-probed-sysroot")
  file(WRITE "${_c10}/sysroot_from_probe/bin/libwinpthread-1.dll" "p-probed-sysroot")
  _write_fake_compiler("${_c10}/bin/fake-clang++"
"got_target=''
got_sysroot=''
probe=''
for a in \"\$@\"; do
  case \"\$a\" in
    --target=*) got_target=\"\${a#--target=}\" ;;
    --sysroot=*) got_sysroot=\"\${a#--sysroot=}\" ;;
    *) probe=\"\$a\" ;;
  esac
done
if [ \"\$got_target\" != 'x86_64-w64-windows-gnu' ] || [ \"\$got_sysroot\" != '${_c10_sys}' ]; then
  echo \"unexpected flags: target='\$got_target' sysroot='\$got_sysroot'\" >&2
  exit 2
fi
case \"\$probe\" in
  -print-sysroot) echo '${_c10}/sysroot_from_probe' ;;
  -print-file-name=libc++.dll) echo '${_c10}/target_dlls/libc++.dll' ;;
  -print-file-name=*) echo \"\${probe#-print-file-name=}\" ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls(
    "${_c10}/bin/fake-clang++" "Clang" "" "c++;mingw32" _got
    "x86_64-w64-windows-gnu"
    "${_c10_sys}")
  set(_want "${_c10}/target_dlls/libc++.dll;${_c10}/sysroot_from_probe/bin/libunwind.dll;${_c10}/sysroot_from_probe/bin/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6j mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()

  # 6k. Explicit SYSROOT adds <sysroot>/bin and <sysroot>/mingw/bin to fallback dirs (and GNU never passes --target= even when compiler_target is non-empty)
  set(_c11 "${WORK}/case_explicit_sysroot_fallback")
  set(_c11_sys "${_c11}/explicit sysroot")
  file(MAKE_DIRECTORY "${_c11}/compiler_bin" "${_c11_sys}/bin" "${_c11_sys}/mingw/bin")
  file(WRITE "${_c11_sys}/bin/libstdc++-6.dll" "s-sysroot")
  file(WRITE "${_c11_sys}/mingw/bin/libgcc_s_seh-1.dll" "g-mingw-bin")
  file(WRITE "${_c11_sys}/mingw/bin/libwinpthread-1.dll" "p-mingw-bin")
  file(WRITE "${_c11}/compiler_bin/libstdc++-6.dll" "s-wrong-compiler-bin")
  _write_fake_compiler("${_c11}/compiler_bin/fake-g++"
"got_sysroot=''
probe=''
for a in \"\$@\"; do
  case \"\$a\" in
    --target=*) echo 'unexpected --target flag passed to GNU g++' >&2; exit 3 ;;
    --sysroot=*) got_sysroot=\"\${a#--sysroot=}\" ;;
    *) probe=\"\$a\" ;;
  esac
done
if [ \"\$got_sysroot\" != '${_c11_sys}' ]; then
  echo \"missing --sysroot flag: got='\$got_sysroot'\" >&2
  exit 2
fi
case \"\$probe\" in
  -print-sysroot) echo '' ;;
  -print-file-name=*) echo \"\${probe#-print-file-name=}\" ;;
esac")
  _vehicle_sil_find_mingw_runtime_dlls(
    "${_c11}/compiler_bin/fake-g++" "GNU" "" "" _got
    "x86_64-w64-mingw32"
    "${_c11_sys}")
  set(_want "${_c11_sys}/bin/libstdc++-6.dll;${_c11_sys}/mingw/bin/libgcc_s_seh-1.dll;${_c11_sys}/mingw/bin/libwinpthread-1.dll")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "case 6k mismatch:\n  got:  ${_got}\n  want: ${_want}")
  endif()
endif()

file(REMOVE_RECURSE "${WORK}")
