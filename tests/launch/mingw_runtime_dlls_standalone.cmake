# SPDX-License-Identifier: MIT
#
# Windows MinGW (non-static) standalone ECU startup proof: verifies that DLLs copied beside a
# built ECU by vehicle_sil_copy_runtime_dlls() suffice to start the ECU with MSYS2/compiler bin
# directories removed from PATH (outside a UCRT64 shell).
#
# Invoked by CTest with cmake -P:
#   -DECU=<path to vehicle_sil_engine_ecu.exe>
#   -DLAUNCH=<path to vehicle_sil_launch.exe>
#   -DSILKIT_DLL=<SilKit.dll or SilKitd.dll from $<TARGET_FILE_NAME:SilKit::SilKit>>
#   -DCOMPILER_ID=<CMAKE_CXX_COMPILER_ID>
#   -DUSES_LIBSTDCXX=<ON|OFF>
#   -DWORK=<scratch directory>
cmake_minimum_required(VERSION 3.21)

foreach(_var ECU LAUNCH SILKIT_DLL COMPILER_ID WORK)
  if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
    message(FATAL_ERROR "mingw_runtime_dlls_standalone.cmake: ${_var} is required")
  endif()
endforeach()

if(NOT WIN32)
  message(FATAL_ERROR "mingw_runtime_dlls_standalone.cmake is Windows-only")
endif()

cmake_path(SET _ecu NORMALIZE "${ECU}")
cmake_path(SET _launch NORMALIZE "${LAUNCH}")
cmake_path(GET _ecu PARENT_PATH _ecu_dir)
cmake_path(GET _ecu FILENAME _ecu_name)

if(NOT EXISTS "${_ecu_dir}/${SILKIT_DLL}")
  message(FATAL_ERROR "expected ${SILKIT_DLL} beside ${_ecu} in ${_ecu_dir}")
endif()

if(USES_LIBSTDCXX)
  if(NOT EXISTS "${_ecu_dir}/libstdc++-6.dll")
    message(FATAL_ERROR "expected libstdc++-6.dll beside ${_ecu} in ${_ecu_dir}")
  endif()
  set(_has_libgcc FALSE)
  foreach(_gcc_dll IN ITEMS libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libgcc_s_sjlj-1.dll)
    if(EXISTS "${_ecu_dir}/${_gcc_dll}")
      set(_has_libgcc TRUE)
    endif()
  endforeach()
  if(NOT _has_libgcc)
    message(FATAL_ERROR "expected libgcc_s_{seh,dw2,sjlj}-1.dll beside ${_ecu} in ${_ecu_dir}")
  endif()
elseif(COMPILER_ID STREQUAL "Clang")
  # Do not hard-require libwinpthread-1.dll here so win32-thread LLVM-MinGW toolchains pass;
  # the stripped-PATH run and negative control below prove runtime completeness.
  foreach(_req IN ITEMS libc++.dll libunwind.dll)
    if(NOT EXISTS "${_ecu_dir}/${_req}")
      message(FATAL_ERROR "expected Clang runtime DLL ${_req} beside ${_ecu} in ${_ecu_dir}")
    endif()
  endforeach()
endif()

# Strip PATH down to Windows system directories only (no MSYS2/MinGW/compiler bin directories).
set(_sysroot "$ENV{SystemRoot}")
if("${_sysroot}" STREQUAL "")
  set(_sysroot "C:\\Windows")
endif()
cmake_path(SET _sysroot_cmake NORMALIZE "${_sysroot}")
file(TO_NATIVE_PATH "${_sysroot_cmake}/System32" _sys32_nat)
file(TO_NATIVE_PATH "${_sysroot_cmake}" _sysroot_nat)
file(TO_NATIVE_PATH "${_sysroot_cmake}/System32/Wbem" _wbem_nat)
set(_minimal_path "${_sys32_nat};${_sysroot_nat};${_wbem_nat}")

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(ENV{PATH} "${_minimal_path}")
file(GLOB _copied_dlls RELATIVE "${_ecu_dir}" "${_ecu_dir}/*.dll")
list(SORT _copied_dlls)
list(JOIN _copied_dlls ", " _copied_dlls_list)
message(STATUS "launch.mingw_runtime_dlls_standalone: stripped PATH=${_minimal_path}")
message(STATUS "launch.mingw_runtime_dlls_standalone: DLLs beside ${_ecu_name}: ${_copied_dlls_list}")

# 1. Run vehicle_sil_launch check first: the launcher sets SEM_FAILCRITICALERRORS |
# SEM_NOOPENFILEERRORBOX, so any unexpected missing transitive DLL fails non-interactively
# instead of opening a Windows error dialog on desktop machines.
file(WRITE "${WORK}/ok.yaml"
  "vehicle_sil_simulation: 2\n"
  "networks:\n"
  "  - name: J1939_CAN\n"
  "    kind: can\n"
  "participants:\n"
  "  - name: Engine\n"
  "    command: [\"${_ecu}\"]\n"
  "    step_ms: 10\n")
execute_process(
  COMMAND "${_launch}" check "${WORK}/ok.yaml"
  WORKING_DIRECTORY "${WORK}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR
    "${_launch} check failed on built ECU with PATH=${_minimal_path} (exit code ${_rc}):\n"
    "stdout: ${_out}\nstderr: ${_err}")
endif()
message(STATUS "launch.mingw_runtime_dlls_standalone: PASS vehicle_sil_launch check without MSYS2 on PATH")

# 2. Direct ECU execution outside UCRT64 shell.
execute_process(
  COMMAND "${_ecu}" --describe --name Engine
  WORKING_DIRECTORY "${WORK}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR
    "${_ecu} --describe failed with PATH=${_minimal_path} (exit code ${_rc}):\n"
    "stdout: ${_out}\nstderr: ${_err}")
endif()
if(NOT _out MATCHES "\"vehicle_sil\"" OR NOT _out MATCHES "\"Engine\"")
  message(FATAL_ERROR "unexpected --describe output from ${_ecu}:\n${_out}")
endif()
message(STATUS "launch.mingw_runtime_dlls_standalone: PASS ${_ecu_name} --describe without MSYS2 on PATH")

# 3. Negative control: copy ONLY the ECU and ${SILKIT_DLL} (omitting MinGW runtime DLLs) and run
# through vehicle_sil_launch check (which sets SEM_FAILCRITICALERRORS so Windows never opens a
# modal dialog). Failing with 0xC0000135 proves PATH/System32 did not supply MinGW DLLs and
# exercises the live 0xC0000135 diagnostic on Windows.
set(_bare_dir "${WORK}/no_mingw")
file(MAKE_DIRECTORY "${_bare_dir}")
file(COPY_FILE "${_ecu}" "${_bare_dir}/${_ecu_name}")
file(COPY_FILE "${_ecu_dir}/${SILKIT_DLL}" "${_bare_dir}/${SILKIT_DLL}")
cmake_path(SET _bare_ecu NORMALIZE "${_bare_dir}/${_ecu_name}")
file(WRITE "${WORK}/missing.yaml"
  "vehicle_sil_simulation: 2\n"
  "networks:\n"
  "  - name: J1939_CAN\n"
  "    kind: can\n"
  "participants:\n"
  "  - name: Engine\n"
  "    command: [\"${_bare_ecu}\"]\n"
  "    step_ms: 10\n")
execute_process(
  COMMAND "${_launch}" check "${WORK}/missing.yaml"
  WORKING_DIRECTORY "${WORK}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
set(_combined "${_out}\n${_err}")
if(_rc EQUAL 0)
  message(FATAL_ERROR
    "negative control unexpectedly passed: ECU started without MinGW runtime DLLs under "
    "PATH=${_minimal_path} (MinGW DLLs may be leaking from the system environment)\n${_combined}")
endif()
if(NOT _combined MATCHES "0xC0000135" OR NOT _combined MATCHES "vehicle_sil_copy_runtime_dlls")
  message(FATAL_ERROR
    "negative control failed with exit code ${_rc}, expected 0xC0000135 + vehicle_sil_copy_runtime_dlls:\n${_combined}")
endif()
string(STRIP "${_combined}" _combined_trimmed)
message(STATUS "launch.mingw_runtime_dlls_standalone: PASS negative control (exit code ${_rc}, 0xC0000135 + vehicle_sil_copy_runtime_dlls):\n${_combined_trimmed}")

file(REMOVE_RECURSE "${WORK}")
