# Checks that the listed files (FILES, relative to ROOT; DIR: every header and source under a
# directory of ROOT, in place of or in addition to FILES) include none of the headers a layer
# must not depend on, directly or through the headers they include: a SIL Kit header (silkit/...,
# also any *_silkit/ library), vehicle_sil/core/participant_runtime.hpp,
# vehicle_sil/core/participant_configuration.hpp (its definitions link SIL Kit; the loopback test is
# in registry_uri.hpp),
# vehicle_sil/process/child_process.hpp, vehicle_sil/process/process.hpp,
# vehicle_sil/launch/run.hpp, vehicle_sil/launch/probe.hpp (what looks at the computer), and
# headers named events.hpp, time_commands.hpp, stop_requests.hpp, runner.hpp, runner_impl.hpp or run_*.hpp
# (run_controls.hpp among them). The check follows the vehicle_sil/... includes (found in the project's include
# directories) and the "..." includes of a header in the same directory as the including file or in a
# directory above it (the private headers of libs/launch/src, also the ones between its parts:
# "format/check_format.hpp"). A failure names the listed file and the chain of includes
# to the forbidden header; to fix it, move what the file needs out of that header, or take the file
# off the list (or put it in another directory) if it is meant to depend on it. The check reads the text: an include inside #if 0 or
# a comment counts, which errs on the safe side.
# FORBIDDEN_PREFIXES (optional, comma separated): more headers that the listed files must not
# reach, each as the start of the path they are included by (vehicle_sil/launch/scenario matches
# scenario.hpp, scenario_run.hpp and scenario_check.hpp): a layer that must not know them. A
# private header included as "..." without a directory matches by its name and the last part of
# the prefix ("scenario_csv.hpp" matches vehicle_sil/launch/scenario): the private headers of a
# layer are named as its public ones.
# A prefix that ends with "/" names a directory of headers (vehicle_sil/launch/ matches every
# header of the launcher's library).
# ONLY_PREFIXES (optional, ON): check FORBIDDEN_PREFIXES only, not the headers listed above: for a layer
# that is meant to use them (the runner), to keep it from the layers that depend on it.
# Usage: cmake -DROOT=<directory> (-DFILES=<file>[,<file>...] | -DDIR=<directory>[,<directory>...])
#        [-DFORBIDDEN_PREFIXES=<prefix>[,<prefix>...]] [-DONLY_PREFIXES=ON] -P VehicleSilCheckIncludes.cmake
cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED ROOT OR (NOT DEFINED FILES AND NOT DEFINED DIR))
  message(FATAL_ERROR "VehicleSilCheckIncludes.cmake: ROOT and FILES or DIR are required")
endif()
if(DEFINED FILES)
  string(REPLACE "," ";" FILES "${FILES}")
else()
  set(FILES "")
endif()
if(DEFINED DIR)
  string(REPLACE "," ";" dirs "${DIR}")
  foreach(dir IN LISTS dirs)
    file(GLOB_RECURSE found RELATIVE "${ROOT}" "${ROOT}/${dir}/*.hpp" "${ROOT}/${dir}/*.h" "${ROOT}/${dir}/*.cpp")
    if(NOT found)
      message(FATAL_ERROR "VehicleSilCheckIncludes.cmake: no sources under ${ROOT}/${dir} (renamed or moved? "
                          "update DIR)")
    endif()
    list(APPEND FILES ${found})
  endforeach()
endif()

# Where a vehicle_sil/... include is found: every include directory of the project.
get_filename_component(project "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB include_roots LIST_DIRECTORIES true
     "${project}/libs/*/include" "${project}/libs/j1939/codec/include" "${project}/libs/j1939/parts/*/include"
     "${project}/bridges/*/include" "${project}/tools/*/include")

set(forbidden_exact vehicle_sil/core/participant_runtime.hpp vehicle_sil/process/child_process.hpp
                    vehicle_sil/process/process.hpp vehicle_sil/launch/run.hpp vehicle_sil/launch/probe.hpp
                    vehicle_sil/core/participant_configuration.hpp)
if(DEFINED FORBIDDEN_PREFIXES)
  string(REPLACE "," ";" FORBIDDEN_PREFIXES "${FORBIDDEN_PREFIXES}")
else()
  set(FORBIDDEN_PREFIXES "")
endif()
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
      if(NOT ONLY_PREFIXES AND (lower MATCHES "^silkit/" OR lower MATCHES "_silkit/" OR path IN_LIST forbidden_exact
         OR base STREQUAL "events.hpp" OR base STREQUAL "time_commands.hpp"
         OR base STREQUAL "stop_requests.hpp" OR base STREQUAL "runner.hpp" OR base STREQUAL "runner_impl.hpp"
         OR base MATCHES "^run_[^/]*\\.hpp$"))
        list(APPEND bad "${chain} ${path}")
        continue()
      endif()
      set(prefixed FALSE)
      foreach(prefix IN LISTS FORBIDDEN_PREFIXES)
        string(LENGTH "${prefix}" prefix_length)
        string(SUBSTRING "${path}" 0 ${prefix_length} start)
        if(start STREQUAL prefix)
          set(prefixed TRUE)
        endif()
        # A private header beside the file: its name against the last part of the prefix.
        get_filename_component(last "${prefix}" NAME)
        string(LENGTH "${last}" last_length)
        string(SUBSTRING "${base}" 0 ${last_length} base_start)
        if(NOT last STREQUAL "" AND open STREQUAL "\"" AND path STREQUAL base AND base_start STREQUAL last)
          set(prefixed TRUE)
        endif()
      endforeach()
      if(prefixed)
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
      elseif(open STREQUAL "\"")
        # beside the including file, or beside a directory above it ("format/check_format.hpp" from
        # src/usecases/, found in src/)
        set(dir "${here}")
        while(TRUE)
          if(EXISTS "${dir}/${path}")
            set(next "${dir}/${path}")
            break()
          endif()
          get_filename_component(parent "${dir}" DIRECTORY)
          string(LENGTH "${ROOT}" root_length)
          string(LENGTH "${parent}" parent_length)
          if(parent STREQUAL dir OR parent_length LESS root_length)
            break()
          endif()
          set(dir "${parent}")
        endwhile()
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
                      "off the list (or out of the directory) in the CMakeLists.txt of its tests if it is meant to depend on it.")
endif()
message(STATUS "${checked} files, no forbidden include (directly or through the headers they include)")
