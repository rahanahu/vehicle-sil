# Checks that the listed files (FILES, relative to ROOT) include none of the headers a layer
# must not depend on, directly or through the headers they include: a SIL Kit header (silkit/...,
# also any *_silkit/ library), vehicle_sil/core/participant_runtime.hpp,
# vehicle_sil/process/child_process.hpp, vehicle_sil/process/process.hpp,
# vehicle_sil/launch/run.hpp, and headers named events.hpp, time_commands.hpp or run_*.hpp. The
# check follows the vehicle_sil/... includes (found in the project's include directories) and the
# "..." includes of a header in the same directory as the including file (the private headers of
# libs/launch/src). A failure names the listed file and the chain of includes to the forbidden
# header; to fix it, move what the file needs out of that header, or take the file off the list
# if it is meant to depend on it. The check reads the text: an include inside #if 0 or a comment
# counts, which errs on the safe side.
# Usage: cmake -DROOT=<directory> -DFILES=<file>[,<file>...] -P VehicleSilCheckIncludes.cmake
cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED ROOT OR NOT DEFINED FILES)
  message(FATAL_ERROR "VehicleSilCheckIncludes.cmake: ROOT and FILES are required")
endif()
string(REPLACE "," ";" FILES "${FILES}")

# Where a vehicle_sil/... include is found: every include directory of the project.
get_filename_component(project "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB include_roots LIST_DIRECTORIES true
     "${project}/libs/*/include" "${project}/libs/j1939/codec/include" "${project}/libs/j1939/parts/*/include"
     "${project}/bridges/*/include" "${project}/tools/*/include")

set(forbidden_exact vehicle_sil/core/participant_runtime.hpp vehicle_sil/process/child_process.hpp
                    vehicle_sil/process/process.hpp vehicle_sil/launch/run.hpp)
set(bad "")
set(checked 0)
foreach(listed IN LISTS FILES)
  if(NOT EXISTS "${ROOT}/${listed}")
    message(FATAL_ERROR "VehicleSilCheckIncludes.cmake: ${ROOT}/${listed} does not exist (renamed or moved? "
                        "update the list of files)")
  endif()
  math(EXPR checked "${checked} + 1")
  set(done "")
  # queue entries are "<file>|<chain>"; the chain is the includes followed so far.
  set(queue "${ROOT}/${listed}|${listed}:")
  while(queue)
    list(POP_FRONT queue item)
    string(REPLACE "|" ";" parts "${item}")
    list(GET parts 0 file)
    list(GET parts 1 chain)
    if(file IN_LIST done)
      continue()
    endif()
    list(APPEND done "${file}")
    get_filename_component(here "${file}" DIRECTORY)
    file(STRINGS "${file}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]")
    foreach(line IN LISTS includes)
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*([\"<])([^\">]+)[\">].*" "\\1\\2" spelled "${line}")
      string(SUBSTRING "${spelled}" 0 1 open)
      string(SUBSTRING "${spelled}" 1 -1 path)
      string(TOLOWER "${path}" lower)
      get_filename_component(base "${path}" NAME)
      if(lower MATCHES "^silkit/" OR lower MATCHES "_silkit/" OR path IN_LIST forbidden_exact
         OR base STREQUAL "events.hpp" OR base STREQUAL "time_commands.hpp" OR base MATCHES "^run_[^/]*\\.hpp$")
        list(APPEND bad "${chain} ${path}")
        continue()
      endif()
      set(next "")
      if(path MATCHES "^vehicle_sil/")
        foreach(dir IN LISTS include_roots)
          if(EXISTS "${dir}/${path}")
            set(next "${dir}/${path}")
            break()
          endif()
        endforeach()
      elseif(open STREQUAL "\"" AND EXISTS "${here}/${path}")
        set(next "${here}/${path}")
      endif()
      if(next)
        list(APPEND queue "${next}|${chain} ${path} ->")
      endif()
    endforeach()
  endwhile()
endforeach()
if(bad)
  list(REMOVE_DUPLICATES bad)
  list(JOIN bad "\n  " text)
  message(FATAL_ERROR "Files that include a header their layer must not depend on (file: include chain to the "
                      "forbidden header):\n  ${text}\n"
                      "Move what the file needs out of that header into one without the dependency, or take the file "
                      "off the list in tests/launch/CMakeLists.txt if it is meant to depend on it.")
endif()
message(STATUS "${checked} files, no forbidden include (directly or through the headers they include)")
