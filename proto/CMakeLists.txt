# The Control API (gRPC and Protocol Buffers). Only with VEHICLE_SIL_BUILD_CONTROL_SERVER.
#   vehicle_sil::control_proto   generated C++ messages and services of vehicle_sil/control/v1
vehicle_sil_add_grpc_proto(vehicle_sil_control_proto ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
                           PROTOS vehicle_sil/control/v1/control.proto)
add_library(vehicle_sil::control_proto ALIAS vehicle_sil_control_proto)
