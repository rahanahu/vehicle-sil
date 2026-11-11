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
# <module>.
function(vehicle_sil_python_can_import out python)
  set(ok FALSE)
  if(NOT "${python}" STREQUAL "")
    set(code "import sys; assert sys.version_info >= (3, 11)")
    foreach(module IN LISTS ARGN)
      string(APPEND code "; import ${module}")
    endforeach()
    execute_process(COMMAND "${python}" -c "${code}" RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if(result EQUAL 0)
      set(ok TRUE)
    endif()
  endif()
  set(${out} ${ok} PARENT_SCOPE)
endfunction()

# vehicle_sil_find_python(<out-var> <module>...)
#
# Sets <out-var> to a Python 3.11+ that imports every <module>, or to "" if there is none. It tries
# the repository's uv environment first (vehicle_sil_repository_python()), then the Python 3 that
# find_package(Python3) finds (an active virtual environment first; Python3_EXECUTABLE names one).
function(vehicle_sil_find_python out)
  vehicle_sil_repository_python(python)
  vehicle_sil_python_can_import(ok "${python}" ${ARGN})
  if(NOT ok)
    find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
    set(python "")
    if(Python3_Interpreter_FOUND)
      set(python "${Python3_EXECUTABLE}")
      vehicle_sil_python_can_import(ok "${python}" ${ARGN})
    endif()
  endif()
  if(ok)
    set(${out} "${python}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

# How to install the Python packages of <group> (a dependency group of pyproject.toml) for a message:
# uv first when the repository's .venv would be used (vehicle_sil_repository_python()), then pip, the
# OS packages (<os-packages>) and MSYS2 (<msys2-packages>).
function(vehicle_sil_python_install_hint out group pip_packages os_packages msys2_packages)
  set(hint "")
  if(vehicle_sil_IS_TOP_LEVEL AND NOT CMAKE_HOST_WIN32)
    string(CONCAT hint "with uv, run `uv sync` (everything) or `uv sync --only-group ${group}` at the top of "
                       "vehicle-sil and configure again; without uv, ")
  endif()
  string(APPEND hint "pip install ${pip_packages} in an active virtual environment (or name a Python with "
                     "-DPython3_EXECUTABLE=...), ${os_packages}, or on MSYS2 pacman -S ${msys2_packages}")
  set(${out} "${hint}" PARENT_SCOPE)
endfunction()
