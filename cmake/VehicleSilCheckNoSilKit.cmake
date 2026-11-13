# Checks that the sources and headers under DIR include no SIL Kit header, directly or through
# a vehicle-sil header that needs SIL Kit: of vehicle-sil's core headers only those of
# vehicle_sil::sim_base (sim_time.hpp, step_inbox.hpp, periodic.hpp) are allowed, and no header
# of a *_silkit library. The vehicle-sil headers they include are checked the same way, and the
# headers those include, and so on (e.g. vehicle_sil/peer/peer.hpp fails: it includes
# vehicle_sil/can_silkit/... and vehicle_sil/core/participant_runtime.hpp). A failure names the
# file under DIR and the chain of includes to the forbidden one. A vehicle_sil/... include that is
# in none of the project's include directories fails too (it could hide SIL Kit), unless it is
# listed in ALLOW_UNRESOLVED (e.g. a header generated into the build tree). The check reads the
# text: an include inside #if 0 or a comment counts, which errs on the safe side.
# Usage: cmake -DDIR=<directory> [-DALLOW_UNRESOLVED=<path;...>] -P VehicleSilCheckNoSilKit.cmake
cmake_minimum_required(VERSION 3.21) # cmake -P sets no policies without it (CMake 3 then rejects IN_LIST)
if(NOT DEFINED DIR)
  message(FATAL_ERROR "VehicleSilCheckNoSilKit.cmake: DIR is required")
endif()
file(GLOB_RECURSE files "${DIR}/*.hpp" "${DIR}/*.h" "${DIR}/*.cpp")
if(NOT files)
  message(FATAL_ERROR "VehicleSilCheckNoSilKit.cmake: no sources under ${DIR}")
endif()

# Where a vehicle_sil/... include is found: every include directory of the project.
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB include_roots LIST_DIRECTORIES true
     "${root}/libs/*/include" "${root}/libs/j1939/codec/include" "${root}/libs/j1939/parts/*/include"
     "${root}/bridges/*/include" "${root}/tools/*/include")

set(allowed_core vehicle_sil/core/sim_time.hpp vehicle_sil/core/step_inbox.hpp vehicle_sil/core/periodic.hpp)
set(bad "")
set(queue ${files})
set(done "")
foreach(file IN LISTS files)
  string(MD5 key "${file}")
  set(chain_${key} "${file}:")
endforeach()
while(queue)
  list(POP_FRONT queue file)
  if(file IN_LIST done)
    continue()
  endif()
  list(APPEND done "${file}")
  string(MD5 key "${file}")
  set(chain "${chain_${key}}")
  file(STRINGS "${file}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]")
  foreach(line IN LISTS includes)
    string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[\"<]([^\">]+)[\">].*" "\\1" path "${line}")
    string(TOLOWER "${path}" lower)
    if(lower MATCHES "^silkit/" OR lower MATCHES "_silkit/")
      list(APPEND bad "${chain} ${path}")
    elseif(path MATCHES "^vehicle_sil/core/" AND NOT path IN_LIST allowed_core)
      list(APPEND bad "${chain} ${path}")
    elseif(path MATCHES "^vehicle_sil/")
      set(found "")
      foreach(dir IN LISTS include_roots)
        if(EXISTS "${dir}/${path}")
          set(found "${dir}/${path}")
          string(MD5 found_key "${found}")
          if(NOT DEFINED chain_${found_key})
            set(chain_${found_key} "${chain} ${path} ->")
          endif()
          list(APPEND queue "${found}")
          break()
        endif()
      endforeach()
      if(NOT found AND NOT path IN_LIST ALLOW_UNRESOLVED)
        list(APPEND bad "${chain} ${path} (not found in the project's include directories, so not checked; "
                        "a generated header can be listed in ALLOW_UNRESOLVED)")
      endif()
    endif()
  endforeach()
endwhile()
if(bad)
  list(REMOVE_DUPLICATES bad)
  list(JOIN bad "\n  " text)
  message(FATAL_ERROR "${DIR} must not include SIL Kit:\n  ${text}")
endif()
list(LENGTH files count)
list(LENGTH done checked)
message(STATUS "${DIR}: ${count} files (${checked} with the vehicle-sil headers they include), no SIL Kit include")
