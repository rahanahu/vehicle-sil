# Checks that the sources of the launcher read the networks of an ECU's description (--describe)
# in one place: under ROOT/DIR (DIR: one directory, or several separated by commas), a line that
# reads a member "networks" of anything (.networks, ->networks, also a pointer to the member,
# ::networks) but the configuration (config.networks, config->networks) must be in one of FILES
# (paths relative to ROOT). Everything else gets the networks from seen_networks(), which gives each under its name
# in the simulation (a participant's network_names applied); a rule or an output that reads
# description.networks itself would see a renamed network under the executable's name, silently.
# The check reads the text: a read in a comment counts, which errs on the safe side. A failure
# names the file, the line and what to do.
# Usage: cmake -DROOT=<directory> -DDIR=<directory under ROOT>[,<directory>...] -DFILES=<file>[,<file>...]
#              -P VehicleSilCheckDescribedNetworks.cmake
cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED ROOT OR NOT DEFINED DIR OR NOT DEFINED FILES)
  message(FATAL_ERROR "VehicleSilCheckDescribedNetworks.cmake: ROOT, DIR and FILES are required")
endif()
string(REPLACE "," ";" FILES "${FILES}")
string(REPLACE "," ";" dirs "${DIR}")
set(files "")
foreach(dir IN LISTS dirs)
  file(GLOB_RECURSE found "${ROOT}/${dir}/*.hpp" "${ROOT}/${dir}/*.h" "${ROOT}/${dir}/*.cpp" "${ROOT}/${dir}/*.cc"
       "${ROOT}/${dir}/*.cxx" "${ROOT}/${dir}/*.ipp" "${ROOT}/${dir}/*.inl")
  if(NOT found)
    message(FATAL_ERROR "VehicleSilCheckDescribedNetworks.cmake: no sources under ${ROOT}/${dir}")
  endif()
  list(APPEND files ${found})
endforeach()
set(bad "")
set(reads 0)
foreach(file IN LISTS files)
  file(STRINGS "${file}" lines REGEX "(\\.|->|::)[ \t]*networks([^A-Za-z0-9_]|$)")
  file(RELATIVE_PATH name "${ROOT}" "${file}")
  foreach(line IN LISTS lines)
    # The networks of the configuration are the file's, under the simulation's names.
    string(REGEX REPLACE "config[ \t]*(\\.|->)[ \t]*networks" "" rest "${line}")
    if(NOT rest MATCHES "(\\.|->|::)[ \t]*networks([^A-Za-z0-9_]|$)")
      continue()
    endif()
    math(EXPR reads "${reads} + 1")
    if(NOT name IN_LIST FILES)
      string(STRIP "${line}" shown)
      list(APPEND bad "${name}: ${shown}")
    endif()
  endforeach()
endforeach()
if(bad)
  list(JOIN bad "\n  " text)
  list(JOIN FILES ", " allowed)
  message(FATAL_ERROR "Sources that read the networks of an ECU's description themselves:\n  ${text}\n"
                      "Read them through seen_networks() (vehicle_sil/launch/check.hpp), which gives each network "
                      "under its name in the simulation (the participant's network_names applied) and keeps the "
                      "executable's name beside it. Only ${allowed} may read the member itself. For a new rule or "
                      "output, add a case with a renamed network to tests/launch/test_check.cpp.")
endif()
if(reads EQUAL 0)
  message(FATAL_ERROR "VehicleSilCheckDescribedNetworks.cmake: no source under ${ROOT}/${DIR} reads the networks of "
                      "a description: the check looks in the wrong place (the reader moved?)")
endif()
