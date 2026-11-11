include_guard(GLOBAL)

# vehicle_sil_repository_python(<out-var>)
#
# Sets <out-var> to the Python of the repository's uv environment (.venv/bin/python at the top of
# vehicle-sil, made by `uv sync`) when vehicle-sil is the top-level project and the environment
# exists, else to "". A project that pulls vehicle-sil in (FetchContent, add_subdirectory) never
# gets it: it keeps its own Python. Not on a Windows host: a uv environment there holds a Python
# that the MinGW (MSYS2) builds have not been checked with, so they keep finding the Python as
# before (the MSYS2 packages).
function(vehicle_sil_repository_python out)
  set(python "")
  if(vehicle_sil_IS_TOP_LEVEL AND NOT CMAKE_HOST_WIN32)
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    if(EXISTS "${root}/.venv/bin/python")
      set(python "${root}/.venv/bin/python")
    endif()
  endif()
  set(${out} "${python}" PARENT_SCOPE)
endfunction()

# vehicle_sil_python_can_import(<out-var> <python> <module>...)
#
# Sets <out-var> to TRUE if <python> (a path, or a name on PATH) is Python 3.11+ and imports every
# <module>; if not, <out-var>_REASON says why (the last line of the error, e.g. "No module named 'yaml'").
function(vehicle_sil_python_can_import out python)
  set(ok FALSE)
  set(reason "no Python given")
  if(NOT "${python}" STREQUAL "")
    set(code "import sys; sys.version_info >= (3, 11) or sys.exit('it is Python ' + sys.version.split()[0])")
    foreach(module IN LISTS ARGN)
      string(APPEND code "; import ${module}")
    endforeach()
    execute_process(COMMAND "${python}" -c "${code}" RESULT_VARIABLE result OUTPUT_QUIET
                    ERROR_VARIABLE error ERROR_STRIP_TRAILING_WHITESPACE)
    if(result EQUAL 0)
      set(ok TRUE)
      set(reason "")
    elseif(NOT result MATCHES "^[0-9]+$")
      set(reason "${result}")  # it did not start, e.g. "No such file or directory"
    else()
      string(REGEX REPLACE "^.*\n" "" reason "${error}")
      string(REGEX REPLACE "^[A-Za-z]*Error: " "" reason "${reason}")
      if(reason STREQUAL "")
        set(reason "exit code ${result}")
      endif()
    endif()
  endif()
  set(${out} ${ok} PARENT_SCOPE)
  set(${out}_REASON "${reason}" PARENT_SCOPE)
endfunction()

# vehicle_sil_find_python(<out-var> <module>...)
#
# Sets <out-var> to a Python 3.11+ that imports every <module>, or to "" if there is none. It tries
# the repository's uv environment first (vehicle_sil_repository_python()), then the Python 3 that
# find_package(Python3) finds (an active virtual environment first; Python3_EXECUTABLE names one).
# <out-var>_TRIED lists the Pythons that were not taken and why, for a message.
function(vehicle_sil_find_python out)
  set(tried "")
  vehicle_sil_repository_python(python)
  if(python)
    vehicle_sil_python_can_import(ok "${python}" ${ARGN})
    if(NOT ok)
      string(APPEND tried "${python} (${ok_REASON}); ")
    endif()
  else()
    set(ok FALSE)
  endif()
  if(NOT ok)
    find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
    set(python "")
    if(Python3_Interpreter_FOUND)
      set(python "${Python3_EXECUTABLE}")
      vehicle_sil_python_can_import(ok "${python}" ${ARGN})
      if(NOT ok)
        string(APPEND tried "${python} (${ok_REASON}); ")
      endif()
    elseif(Python3_EXECUTABLE)
      string(APPEND tried "${Python3_EXECUTABLE} (Python3_EXECUTABLE; not a Python 3.11+ that runs); ")
    else()
      string(APPEND tried "find_package(Python3 3.11) found no Python 3.11+; ")
    endif()
  endif()
  string(REGEX REPLACE "; $" "" tried "${tried}")
  if(ok)
    set(${out} "${python}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
  set(${out}_TRIED "${tried}" PARENT_SCOPE)
endfunction()

# How to install the Python packages for a message: uv first when the repository's .venv would be used
# (vehicle_sil_repository_python()), then pip (<pip-packages>), the OS packages (<os-packages>) and
# MSYS2 (<msys2-packages>). <python-option> is the -D option that names a Python.
function(vehicle_sil_python_install_hint out pip_packages os_packages msys2_packages python_option)
  set(hint "")
  if(vehicle_sil_IS_TOP_LEVEL AND NOT CMAKE_HOST_WIN32)
    # Plain `uv sync`: --only-group would take the other groups' packages out of .venv.
    set(hint "with uv, run `uv sync` at the top of vehicle-sil and configure again; without uv, ")
  endif()
  string(APPEND hint "pip install ${pip_packages} in an active virtual environment (or name a Python with "
                     "${python_option}=...), ${os_packages}, or on MSYS2 pacman -S ${msys2_packages}")
  set(${out} "${hint}" PARENT_SCOPE)
endfunction()
