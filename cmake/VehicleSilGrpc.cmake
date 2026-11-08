include_guard(GLOBAL)

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
