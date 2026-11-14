# TEST ONLY. Checks that every DLL which DLL needs is where the pcap link lets Windows look when
# it loads a pcap library: next to DLL, or in the system directory (pcap_loader_win.cpp). A DLL
# from anywhere else (PATH: the toolchain's libstdc++-6.dll, libgcc_s_seh-1.dll,
# libwinpthread-1.dll) fails, and the failure names it. Names of Windows' API sets (api-ms-...,
# ext-ms-...) are not files; Windows resolves them itself.
# Usage: cmake -DOBJDUMP=<objdump> -DDLL=<file> [-DSYSTEM_DIR=<directory>] -P check_dll_imports.cmake
#        SYSTEM_DIR: the system directory; %SystemRoot%\System32 without it.
cmake_minimum_required(VERSION 3.21)
if(NOT DLL OR NOT EXISTS "${DLL}")
  message(FATAL_ERROR "check_dll_imports.cmake: DLL '${DLL}' is not there: build it first, and pass -DDLL=<file>")
endif()
if(NOT OBJDUMP)
  message(FATAL_ERROR "check_dll_imports.cmake: no objdump to read what ${DLL} needs: CMake found none "
                      "(CMAKE_OBJDUMP is empty). Install the toolchain's binutils, or pass -DOBJDUMP=<objdump>")
endif()
if(NOT SYSTEM_DIR)
  if(NOT DEFINED ENV{SystemRoot})
    message(FATAL_ERROR "check_dll_imports.cmake: the environment has no SystemRoot: pass -DSYSTEM_DIR=<system directory>")
  endif()
  file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32" SYSTEM_DIR)
endif()
if(NOT IS_DIRECTORY "${SYSTEM_DIR}")
  message(FATAL_ERROR "check_dll_imports.cmake: the system directory '${SYSTEM_DIR}' is not there: pass -DSYSTEM_DIR=<directory>")
endif()

execute_process(COMMAND "${OBJDUMP}" -p "${DLL}" OUTPUT_VARIABLE dump ERROR_VARIABLE problem RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "check_dll_imports.cmake: '${OBJDUMP} -p ${DLL}' failed (${result}): ${problem}")
endif()
string(REGEX MATCHALL "DLL Name: [^\r\n]+" lines "${dump}")
if(NOT lines)
  message(FATAL_ERROR "check_dll_imports.cmake: '${OBJDUMP} -p ${DLL}' names no DLL ('DLL Name: ...'): "
                      "every DLL needs at least one, so this objdump writes another form. Use the toolchain's objdump")
endif()

get_filename_component(own_dir "${DLL}" DIRECTORY)
set(needed "")
set(elsewhere "")
foreach(line IN LISTS lines)
  string(REGEX REPLACE "^DLL Name: *" "" name "${line}")
  string(STRIP "${name}" name)
  list(APPEND needed "${name}")
  string(TOLOWER "${name}" lower)
  if(lower MATCHES "^(api|ext)-ms-" OR EXISTS "${own_dir}/${name}" OR EXISTS "${SYSTEM_DIR}/${name}")
    continue()
  endif()
  list(APPEND elsewhere "${name}")
endforeach()
list(JOIN needed ", " needed_text)
if(elsewhere)
  list(JOIN elsewhere ", " elsewhere_text)
  message(FATAL_ERROR
    "${DLL} needs ${elsewhere_text}, found neither next to it nor in ${SYSTEM_DIR}. The pcap link has Windows look "
    "in these two places only when it loads a pcap library (libs/eth_link/src/pcap/pcap_loader_win.cpp), so this "
    "DLL loads only where the program happens to have the same DLL loaded already. Link the toolchain's libraries "
    "into the fake library (MinGW: the link option -static in tests/eth_link/CMakeLists.txt); do not make the link "
    "search more places. All it needs: ${needed_text}")
endif()
message(STATUS "${DLL} needs only DLLs next to it or in ${SYSTEM_DIR}: ${needed_text}")
