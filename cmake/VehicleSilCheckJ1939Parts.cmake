# Checks that the protocol parts of the J1939 stack (libs/j1939/parts) use only its
# public API and toolbox: no internal header, no test entry, no access tricks, and links only
# vehicle_sil::j1939 and other parts. The compiler enforces the rest (the parts' include
# paths do not contain libs/j1939/src, and the internals are private).
#   cmake -DPARTS_DIR=<libs/j1939/parts> -P VehicleSilCheckJ1939Parts.cmake
# With -DSOURCES_ONLY=ON only the sources are checked (for libraries built on the engine that
# are not parts, such as the test kit and Peer, which link other libraries too).
# Parts also may not set an Observer (the network has one, for tests and recording; parts take
# part through handlers): -DFORBID_OBSERVER=ON, the default without SOURCES_ONLY.
if(NOT PARTS_DIR OR NOT IS_DIRECTORY "${PARTS_DIR}")
  message(FATAL_ERROR "PARTS_DIR is not a directory: ${PARTS_DIR}")
endif()

file(GLOB_RECURSE sources "${PARTS_DIR}/*.hpp" "${PARTS_DIR}/*.cpp" "${PARTS_DIR}/*.h" "${PARTS_DIR}/*.cc"
     "${PARTS_DIR}/*.cxx" "${PARTS_DIR}/*.hxx" "${PARTS_DIR}/*.ipp" "${PARTS_DIR}/*.inl" "${PARTS_DIR}/*.tpp"
     "${PARTS_DIR}/*.ixx" "${PARTS_DIR}/*.cppm")
if(NOT sources)
  message(FATAL_ERROR "no sources under ${PARTS_DIR}")
endif()
set(forbidden
  "#[ \t]*include[ \t]*[\"<][^\">]*/src/"   # an internal header by path
  "#[ \t]*include[ \t]*[\"<]transport\\.hpp" # the internal transport header
  "\\.\\./"                                  # includes or paths that climb out of the part
  "NetworkTestAccess"                        # the tests' way into the internals
  "#[ \t]*define[ \t]+(private|protected|class|struct)" # access tricks
  # befriending classes (friend class X; friend X;). Hidden friend functions such as a
  # defaulted operator== are allowed: they give no access to the engine.
  "friend[ \t\r\n]+(class|struct)[ \t\r\n]"
  "friend[ \t\r\n]+[A-Za-z_:][A-Za-z0-9_:<>, \t\r\n]*\;") # (\; keeps the list element whole)
if(NOT DEFINED FORBID_OBSERVER)
  if(SOURCES_ONLY)
    set(FORBID_OBSERVER OFF)
  else()
    set(FORBID_OBSERVER ON)
  endif()
endif()
if(FORBID_OBSERVER)
  list(APPEND forbidden "set_observer")
endif()
set(failures "")
foreach(source IN LISTS sources)
  file(READ "${source}" text)
  foreach(pattern IN LISTS forbidden)
    if(text MATCHES "${pattern}")
      string(APPEND failures "\n  ${source}: matches '${pattern}'")
    endif()
  endforeach()
endforeach()

if(SOURCES_ONLY)
  set(lists "")
else()
  file(GLOB_RECURSE lists "${PARTS_DIR}/CMakeLists.txt")
endif()
foreach(list IN LISTS lists)
  file(READ "${list}" text)
  string(REGEX MATCHALL "target_link_libraries\\([^)]*\\)" links "${text}")
  foreach(link IN LISTS links)
    string(REGEX REPLACE "target_link_libraries\\([ \t\n]*[A-Za-z0-9_]+" "" link "${link}")
    string(REGEX REPLACE "[()]|PUBLIC|PRIVATE|INTERFACE" " " link "${link}")
    separate_arguments(items UNIX_COMMAND "${link}")
    foreach(item IN LISTS items)
      if(NOT item MATCHES "^vehicle_sil::j1939(_[a-z0-9_]+)?$")
        string(APPEND failures "\n  ${list}: links ${item}")
      endif()
    endforeach()
  endforeach()
  if(text MATCHES "j1939/src|\\.\\./|SOURCE_DIR>|_SOURCE_DIR}/src|PROJECT_SOURCE_DIR|CMAKE_SOURCE_DIR")
    string(APPEND failures "\n  ${list}: refers to the engine's sources or to paths outside the part")
  endif()
  # Compile definitions and options could rename access specifiers (-Dprivate=public); the
  # include paths may only add the part's own include/. The only property a part sets is its
  # export name.
  string(REGEX REPLACE "set_target_properties\\([ \t\n]*[A-Za-z0-9_]+[ \t\n]+PROPERTIES[ \t\n]+EXPORT_NAME[ \t\n]+[A-Za-z0-9_]+[ \t\n]*\\)" "" text "${text}")
  if(text MATCHES "target_compile_definitions|target_compile_options|add_compile_definitions|add_compile_options|add_definitions|COMPILE_DEFINITIONS|COMPILE_OPTIONS|COMPILE_FLAGS|CMAKE_CXX_FLAGS|(^|[^_])include_directories\\(|set_property|set_target_properties|(^|[^_])link_libraries\\(|target_link_options|link_directories|(^|[^_a-z])include\\(")
    string(APPEND failures "\n  ${list}: sets compile definitions, options, flags, properties or link settings, or includes CMake code")
  endif()
  string(REGEX MATCHALL "target_include_directories\\([^)]*\\)" includes "${text}")
  foreach(include IN LISTS includes)
    string(REGEX REPLACE "target_include_directories\\([ \t\n]*[A-Za-z0-9_]+[ \t\n]+(PUBLIC|PRIVATE|INTERFACE)[ \t\n]+" "" rest "${include}")
    if(NOT rest MATCHES "^\\$<BUILD_INTERFACE:\\$\\{CMAKE_CURRENT_SOURCE_DIR\\}/include>\\)$")
      string(APPEND failures "\n  ${list}: include directories other than the part's include/: ${include}")
    endif()
  endforeach()
endforeach()

if(failures)
  message(FATAL_ERROR "protocol parts reach into the J1939 engine:${failures}")
endif()
list(LENGTH sources count)
message(STATUS "j1939 parts: ${count} sources use the public API only")
