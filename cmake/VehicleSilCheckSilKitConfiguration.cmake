# Checks that every SIL Kit participant configuration that the sources under DIRS make from a text
# goes through vehicle-sil's defaults (vehicle_sil/core/participant_runtime.hpp): each call of
# ParticipantConfigurationFromString( must name default_participant_configuration (also ..._with),
# participant_configuration_or_default or default_registry_configuration in its statement (up to
# the next ';'). A "{}" or another text given directly would take SIL Kit's defaults, which
# vehicle-sil changes (the participants of a loopback registry listen on loopback only; TCP only on
# Windows: docs/dependencies.md). The check reads the text: a call in a comment counts, which errs
# on the safe side. A failure names the file and the statement.
# Usage: cmake -DDIRS=<directory>[,<directory>...] [-DEXCLUDE=<directory>[,<directory>...]]
#              -P VehicleSilCheckSilKitConfiguration.cmake
cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED DIRS)
  message(FATAL_ERROR "VehicleSilCheckSilKitConfiguration.cmake: DIRS is required")
endif()
string(REPLACE "," ";" DIRS "${DIRS}")
set(files "")
foreach(dir IN LISTS DIRS)
  file(GLOB_RECURSE found "${dir}/*.hpp" "${dir}/*.h" "${dir}/*.cpp")
  list(APPEND files ${found})
endforeach()
if(DEFINED EXCLUDE)
  string(REPLACE "," ";" EXCLUDE "${EXCLUDE}")
  set(kept "")
  foreach(file IN LISTS files)
    set(excluded FALSE)
    foreach(dir IN LISTS EXCLUDE)
      string(FIND "${file}" "${dir}/" at)
      if(at EQUAL 0)
        set(excluded TRUE)
      endif()
    endforeach()
    if(NOT excluded)
      list(APPEND kept "${file}")
    endif()
  endforeach()
  set(files "${kept}")
endif()
if(NOT files)
  message(FATAL_ERROR "VehicleSilCheckSilKitConfiguration.cmake: no sources under ${DIRS}")
endif()
set(call "ParticipantConfigurationFromString(")
string(LENGTH "${call}" call_length)
set(bad "")
foreach(file IN LISTS files)
  file(READ "${file}" rest)
  while(TRUE)
    string(FIND "${rest}" "${call}" at)
    if(at EQUAL -1)
      break()
    endif()
    math(EXPR after "${at} + ${call_length}")
    string(SUBSTRING "${rest}" ${after} -1 rest)
    string(FIND "${rest}" ";" end)
    string(SUBSTRING "${rest}" 0 ${end} statement)
    if(NOT statement MATCHES "default_participant_configuration|participant_configuration_or_default|default_registry_configuration")
      string(REGEX REPLACE "[ \t\r\n]+" " " shown "${statement}")
      get_filename_component(name "${file}" NAME)
      list(APPEND bad "${name}: ${call}${shown}")
    endif()
  endwhile()
endforeach()
if(bad)
  list(JOIN bad "\n  " text)
  message(FATAL_ERROR "SIL Kit participant configurations that do not go through vehicle-sil's defaults "
                      "(use default_participant_configuration(), default_participant_configuration_with(), "
                      "participant_configuration_or_default() or default_registry_configuration() of "
                      "vehicle_sil/core/participant_runtime.hpp):\n  ${text}")
endif()
