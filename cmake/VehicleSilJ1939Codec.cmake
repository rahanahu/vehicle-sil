include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/VehicleSilPython.cmake)

set(VEHICLE_SIL_J1939_CODEGEN "${CMAKE_CURRENT_LIST_DIR}/../tools/j1939_codegen/j1939_codegen.py"
    CACHE INTERNAL "The generator of typed J1939 messages")

# Finds Python 3.11+ with PyYAML for the generator: the repository's uv environment (.venv) first
# when vehicle-sil is the top-level project, else the Python 3 of find_package(Python3). Sets
# VEHICLE_SIL_J1939_CODEGEN_PYTHON (empty if there is none: then it looks again at the next
# configuration, after PyYAML was installed) and, with REQUIRED, stops the configuration with the
# reason. It looks again when the Python it found is gone, and when a .venv appeared after it found
# another Python.
function(vehicle_sil_find_j1939_codegen)
  cmake_parse_arguments(ARG "REQUIRED" "" "" ${ARGN})
  vehicle_sil_repository_python(venv)
  if(NOT VEHICLE_SIL_J1939_CODEGEN_PYTHON OR NOT EXISTS "${VEHICLE_SIL_J1939_CODEGEN_PYTHON}"
     OR (venv AND NOT VEHICLE_SIL_J1939_CODEGEN_PYTHON STREQUAL venv
         AND NOT VEHICLE_SIL_J1939_CODEGEN_VENV_CHECKED STREQUAL venv))
    vehicle_sil_find_python(python yaml)
    set(VEHICLE_SIL_J1939_CODEGEN_PYTHON "${python}" CACHE INTERNAL "Python 3.11+ with PyYAML for j1939_codegen")
    # A .venv without PyYAML is not checked again at every call.
    set(VEHICLE_SIL_J1939_CODEGEN_VENV_CHECKED "${venv}" CACHE INTERNAL "The .venv checked for PyYAML")
  endif()
  if(ARG_REQUIRED AND VEHICLE_SIL_J1939_CODEGEN_PYTHON STREQUAL "")
    vehicle_sil_python_install_hint(hint codegen pyyaml "dnf install python3-pyyaml, apt install python3-yaml"
                                    mingw-w64-ucrt-x86_64-python-yaml)
    message(FATAL_ERROR "vehicle-sil: generating J1939 message types needs Python 3.11+ with PyYAML: ${hint}")
  endif()
endfunction()

# vehicle_sil_add_j1939_codec(<target> <definition.yaml>...)
#
# Generates one header per definition (<file name without its last extension>.hpp) at build time and makes
# <target> an INTERFACE library that puts them on the include path and links
# vehicle_sil::j1939_codec. The headers are regenerated when a definition or the generator
# changes. Needs Python 3.11+ with PyYAML.
function(vehicle_sil_add_j1939_codec target)
  if(ARGC LESS 2)
    message(FATAL_ERROR "vehicle_sil_add_j1939_codec(${target}): give at least one YAML definition")
  endif()
  vehicle_sil_find_j1939_codegen(REQUIRED)
  set(directory "${CMAKE_CURRENT_BINARY_DIR}/${target}_codec")
  set(headers "")
  foreach(definition IN LISTS ARGN)
    get_filename_component(path "${definition}" ABSOLUTE)
    get_filename_component(stem "${definition}" NAME_WLE)
    set(header "${directory}/${stem}.hpp")
    if(header IN_LIST headers)
      message(FATAL_ERROR "vehicle_sil_add_j1939_codec(${target}): two definitions named ${stem}")
    endif()
    add_custom_command(
      OUTPUT "${header}"
      COMMAND "${VEHICLE_SIL_J1939_CODEGEN_PYTHON}" "${VEHICLE_SIL_J1939_CODEGEN}" "${path}" -o "${header}"
      DEPENDS "${path}" "${VEHICLE_SIL_J1939_CODEGEN}"
      COMMENT "Generating J1939 message types ${stem}.hpp"
      VERBATIM)
    list(APPEND headers "${header}")
  endforeach()
  add_custom_target(${target}_generate DEPENDS ${headers})
  add_library(${target} INTERFACE)
  target_include_directories(${target} INTERFACE "${directory}")
  target_link_libraries(${target} INTERFACE vehicle_sil::j1939_codec)
  add_dependencies(${target} ${target}_generate)
endfunction()
