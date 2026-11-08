# Checks that codec.hpp (the typed J1939 messages) stays independent of the engine: of
# vehicle-sil's headers it includes only vehicle_sil/j1939/types.hpp (the value types), never the
# Network and Node. Usage: cmake -DHEADER=<codec.hpp> -P VehicleSilCheckJ1939Codec.cmake
if(NOT DEFINED HEADER)
  message(FATAL_ERROR "VehicleSilCheckJ1939Codec.cmake: HEADER is required")
endif()
file(STRINGS "${HEADER}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*(\"|<vehicle_sil/)")
set(allowed "vehicle_sil/j1939/types.hpp")
set(bad "")
foreach(line IN LISTS includes)
  string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[\"<]([^\">]+)[\">].*" "\\1" path "${line}")
  if(NOT path IN_LIST allowed)
    list(APPEND bad "${path}")
  endif()
endforeach()
if(bad)
  message(FATAL_ERROR "${HEADER} must include only ${allowed} of vehicle-sil's headers, found: ${bad}")
endif()
message(STATUS "${HEADER}: includes ${allowed} only")
