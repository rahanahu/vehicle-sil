include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/VehicleSilPython.cmake)

# gRPC and Protocol Buffers for the control server and the control agent's link to it.
# Only with VEHICLE_SIL_BUILD_CONTROL_SERVER: the rest of vehicle-sil never needs them.
#
# gRPC is taken from find_package(gRPC CONFIG) (a system or MSYS2 package, vcpkg, ...) when it is
# found, otherwise built with FetchContent at a pinned version. To reuse a source tree that is
# already downloaded, pass -DFETCHCONTENT_SOURCE_DIR_GRPC=<path> (a standard FetchContent setting).
#
# Targets afterwards:
#   vehicle_sil::grpcpp           gRPC C++ (with protobuf)
#   vehicle_sil_protoc            the protoc executable (generator expression in VEHICLE_SIL_PROTOC)
#   vehicle_sil_grpc_cpp_plugin   the gRPC C++ plugin (generator expression in VEHICLE_SIL_GRPC_CPP_PLUGIN)

set(VEHICLE_SIL_GRPC_VERSION "1.76.0" CACHE STRING "gRPC version that FetchContent builds when none is found")

function(_vehicle_sil_fetch_grpc)
  # FetchContent_Declare(SYSTEM) needs 3.25 and EXCLUDE_FROM_ALL 3.28.
  if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR "vehicle-sil: building gRPC with FetchContent needs CMake 3.28 or newer "
                        "(this is ${CMAKE_VERSION}); install gRPC so that find_package(gRPC CONFIG) finds it, or update CMake")
  endif()
  include(FetchContent)
  # Normal variables in this function scope; gRPC's and its dependencies' option() calls take them
  # (CMP0077). Install rules are off: they fail when abseil is a subproject, and nothing installs it.
  set(gRPC_BUILD_TESTS OFF)
  set(gRPC_BUILD_CODEGEN ON)
  set(gRPC_INSTALL OFF)
  set(protobuf_INSTALL OFF)
  set(protobuf_BUILD_TESTS OFF)
  set(utf8_range_ENABLE_INSTALL OFF)
  set(ABSL_ENABLE_INSTALL ON) # gRPC's own export sets refer to abseil's targets (nothing is installed)
  set(ABSL_PROPAGATE_CXX_STD ON)
  set(ABSL_BUILD_TESTING OFF)
  foreach(plugin CSHARP NODE OBJECTIVE_C PHP PYTHON RUBY)
    set(gRPC_BUILD_GRPC_${plugin}_PLUGIN OFF)
  endforeach()
  # Warnings of third-party code are not ours to fix.
  set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
  # gRPC, protobuf, abseil and BoringSSL debug info is ~2.8 GB of object files and not needed for
  # vehicle-sil tests; keep vehicle-sil itself in Debug while stripping FetchContent gRPC debug info.
  set(CMAKE_C_FLAGS_DEBUG "-g0")
  set(CMAKE_CXX_FLAGS_DEBUG "-g0")
  if(MINGW)
    # BoringSSL without its assembly (it would need NASM): the control server's TLS is not a hot path.
    set(OPENSSL_NO_ASM ON)
  endif()
  FetchContent_Declare(
    grpc
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG v${VEHICLE_SIL_GRPC_VERSION}
    GIT_SHALLOW TRUE
    # Only what the C++ library and its code generator need (the full tree is several GB).
    GIT_SUBMODULES third_party/abseil-cpp third_party/protobuf third_party/re2 third_party/cares/cares
                   third_party/boringssl-with-bazel third_party/zlib
    GIT_SUBMODULES_RECURSE FALSE
    SYSTEM # its headers are not ours to warn about
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(grpc)
endfunction()

function(vehicle_sil_require_grpc)
  if(TARGET vehicle_sil::grpcpp)
    return()
  endif()
  find_package(gRPC CONFIG QUIET)
  if(gRPC_FOUND AND TARGET gRPC::grpc++ AND TARGET gRPC::grpc_cpp_plugin AND TARGET protobuf::protoc)
    message(STATUS "vehicle-sil: gRPC ${gRPC_VERSION} found (${gRPC_DIR})")
    add_library(vehicle_sil_grpcpp INTERFACE)
    target_link_libraries(vehicle_sil_grpcpp INTERFACE gRPC::grpc++ protobuf::libprotobuf)
    set(protoc protobuf::protoc)
    set(plugin gRPC::grpc_cpp_plugin)
  else()
    message(STATUS "vehicle-sil: gRPC not found; building gRPC ${VEHICLE_SIL_GRPC_VERSION} with FetchContent")
    _vehicle_sil_fetch_grpc()
    add_library(vehicle_sil_grpcpp INTERFACE)
    target_link_libraries(vehicle_sil_grpcpp INTERFACE grpc++ libprotobuf)
    set(protoc protoc)
    set(plugin grpc_cpp_plugin)
  endif()
  add_library(vehicle_sil::grpcpp ALIAS vehicle_sil_grpcpp)
  set(VEHICLE_SIL_PROTOC "$<TARGET_FILE:${protoc}>" CACHE INTERNAL "")
  set(VEHICLE_SIL_PROTOC_TARGET "${protoc}" CACHE INTERNAL "")
  set(VEHICLE_SIL_GRPC_CPP_PLUGIN "$<TARGET_FILE:${plugin}>" CACHE INTERNAL "")
  set(VEHICLE_SIL_GRPC_CPP_PLUGIN_TARGET "${plugin}" CACHE INTERNAL "")
endfunction()

# vehicle_sil_add_grpc_proto(<target> ROOT <dir> PROTOS <file.proto>...)
# Generates the C++ messages and gRPC services of the .proto files (paths relative to ROOT, which
# is also the import root) into the build tree and builds them as the static library <target>.
function(vehicle_sil_add_grpc_proto target)
  cmake_parse_arguments(ARG "" "ROOT" "PROTOS" ${ARGN})
  vehicle_sil_require_grpc()
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${target}_gen")
  file(MAKE_DIRECTORY "${out}")
  set(sources)
  foreach(proto IN LISTS ARG_PROTOS)
    string(REGEX REPLACE "\\.proto$" "" stem "${proto}")
    set(generated "${out}/${stem}.pb.cc" "${out}/${stem}.pb.h" "${out}/${stem}.grpc.pb.cc" "${out}/${stem}.grpc.pb.h")
    add_custom_command(
      OUTPUT ${generated}
      COMMAND "${VEHICLE_SIL_PROTOC}" "--proto_path=${ARG_ROOT}" "--cpp_out=${out}" "--grpc_out=${out}"
              "--plugin=protoc-gen-grpc=${VEHICLE_SIL_GRPC_CPP_PLUGIN}" "${ARG_ROOT}/${proto}"
      DEPENDS "${ARG_ROOT}/${proto}" ${VEHICLE_SIL_PROTOC_TARGET} ${VEHICLE_SIL_GRPC_CPP_PLUGIN_TARGET}
      COMMENT "Generating C++ for ${proto}"
      VERBATIM)
    list(APPEND sources ${generated})
  endforeach()
  add_library(${target} STATIC ${sources})
  target_include_directories(${target} SYSTEM PUBLIC $<BUILD_INTERFACE:${out}>)
  target_link_libraries(${target} PUBLIC vehicle_sil::grpcpp)
  target_compile_features(${target} PUBLIC cxx_std_20)
endfunction()

# vehicle_sil_add_python_grpc_stubs(<target>
#   OUT <dir>
#   PYTHON_VAR <var>
#   [PYTHONPATH <dir>]
#   [DEPENDS <dep>...]
#   [BYPRODUCTS <file>...]
#   [ALL])
#
# Finds Python 3.11+ with grpc and grpc_tools (using <var> if already set, else
# VEHICLE_SIL_CONTROL_PYTHON, else vehicle_sil_find_python()), generates the Python Control API
# stubs into <dir> at build time, and defines custom target <target>. Sets <var> to the Python
# executable (or "" if none was found, leaving <target> undefined and setting <var>_REASON and
# <var>_TRIED).
function(vehicle_sil_add_python_grpc_stubs target)
  cmake_parse_arguments(ARG "ALL" "OUT;PYTHON_VAR;PYTHONPATH" "DEPENDS;BYPRODUCTS" ${ARGN})
  if(NOT ARG_OUT)
    message(FATAL_ERROR "vehicle_sil_add_python_grpc_stubs(${target}): OUT is required")
  endif()
  if(NOT ARG_PYTHON_VAR)
    message(FATAL_ERROR "vehicle_sil_add_python_grpc_stubs(${target}): PYTHON_VAR is required")
  endif()

  get_filename_component(_repo_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  set(_pythonpath "${ARG_PYTHONPATH}")
  if(_pythonpath STREQUAL "")
    set(_pythonpath "${_repo_root}/python")
  endif()

  set(_python "")
  set(_tried "")
  if(NOT "${${ARG_PYTHON_VAR}}" STREQUAL "")
    set(_python "${${ARG_PYTHON_VAR}}")
  elseif(VEHICLE_SIL_CONTROL_PYTHON)
    set(_python "${VEHICLE_SIL_CONTROL_PYTHON}")
  else()
    vehicle_sil_find_python(_python grpc grpc_tools)
    set(_tried "${_python_TRIED}")
  endif()
  vehicle_sil_python_can_import(_ok "${_python}" grpc grpc_tools)
  if(NOT _ok)
    set(${ARG_PYTHON_VAR} "" PARENT_SCOPE)
    set(${ARG_PYTHON_VAR}_REASON "${_ok_REASON}" PARENT_SCOPE)
    set(${ARG_PYTHON_VAR}_TRIED "${_tried}" PARENT_SCOPE)
    return()
  endif()

  set(_pb2 "${ARG_OUT}/vehicle_sil/control/v1/control_pb2.py")
  set(_grpc_pb2 "${ARG_OUT}/vehicle_sil/control/v1/control_pb2_grpc.py")
  set(_byproducts "${_grpc_pb2}" ${ARG_BYPRODUCTS})
  list(REMOVE_DUPLICATES _byproducts)

  set(_file_deps
    "${_repo_root}/proto/vehicle_sil/control/v1/control.proto"
    "${_repo_root}/python/vehicle_sil_control/generate.py")
  set(_target_deps)
  foreach(_dep IN LISTS ARG_DEPENDS)
    if(TARGET "${_dep}")
      list(APPEND _target_deps "${_dep}")
    else()
      list(APPEND _file_deps "${_dep}")
    endif()
  endforeach()

  add_custom_command(
    OUTPUT "${_pb2}"
    BYPRODUCTS ${_byproducts}
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${_pythonpath}"
            "${_python}" -m vehicle_sil_control.generate
            --proto-root "${_repo_root}/proto" --out "${ARG_OUT}"
    DEPENDS ${_file_deps}
    COMMENT "Generating Python Control API stubs (${target})"
    VERBATIM)
  set(_all_flag)
  if(ARG_ALL)
    set(_all_flag ALL)
  endif()
  add_custom_target(${target} ${_all_flag} DEPENDS "${_pb2}")
  if(_target_deps)
    add_dependencies(${target} ${_target_deps})
  endif()

  set(${ARG_PYTHON_VAR} "${_python}" PARENT_SCOPE)
  set(${ARG_PYTHON_VAR}_REASON "" PARENT_SCOPE)
  set(${ARG_PYTHON_VAR}_TRIED "${_tried}" PARENT_SCOPE)
endfunction()
