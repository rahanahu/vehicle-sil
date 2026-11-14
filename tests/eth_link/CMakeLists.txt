# vehicle_sil::eth_link alone (no SIL Kit): MAC addresses, the frame filter, the MAC side table,
# and the fake link the Ethernet bridge tests use.

# TEST ONLY: an in-memory ExternalLink with a receive thread, and the wire between fake links.
add_library(vehicle_sil_eth_link_fake STATIC fake_link/fake_link.cpp)
target_include_directories(vehicle_sil_eth_link_fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fake_link)
target_link_libraries(vehicle_sil_eth_link_fake PUBLIC vehicle_sil::eth_link)
vehicle_sil_target_defaults(vehicle_sil_eth_link_fake)

# TEST ONLY: a fake pcap library (fake_pcap/fake_pcap.cpp says what its devices do), built as the
# shared library the pcap link loads at run time in place of Npcap's wpcap.dll or libpcap; and the
# same library from before a function the link needs. Nothing links them: the tests load them by
# their paths.
set(VEHICLE_SIL_FAKE_PCAP_DIR ${CMAKE_CURRENT_BINARY_DIR}/fake_pcap CACHE INTERNAL "where the fake pcap libraries are built")
set(_fakes vehicle_sil_fake_pcap vehicle_sil_fake_pcap_old vehicle_sil_fake_pcap_no_open)
foreach(_fake ${_fakes})
  add_library(${_fake} MODULE fake_pcap/fake_pcap.cpp)
  target_include_directories(${_fake} PRIVATE ${PROJECT_SOURCE_DIR}/libs/eth_link/src/pcap
                                              ${PROJECT_SOURCE_DIR}/libs/eth_link/include)
  set_target_properties(${_fake} PROPERTIES PREFIX "" DEBUG_POSTFIX ""
                                            LIBRARY_OUTPUT_DIRECTORY ${VEHICLE_SIL_FAKE_PCAP_DIR}
                                            RUNTIME_OUTPUT_DIRECTORY ${VEHICLE_SIL_FAKE_PCAP_DIR})
  vehicle_sil_target_defaults(${_fake})
  if(MINGW)
    # The pcap link has Windows look for the DLLs a library needs next to the library and in the
    # system directory only (never in PATH, where the toolchain's libstdc++-6.dll and the others
    # are). So the fake needs system DLLs only, as Npcap's wpcap.dll does: the toolchain's
    # libraries are linked into it. The search of the link is not widened for the tests.
    target_link_options(${_fake} PRIVATE -static)
  endif()
endforeach()
target_compile_definitions(vehicle_sil_fake_pcap_old PRIVATE FAKE_PCAP_OLD)
target_compile_definitions(vehicle_sil_fake_pcap_no_open PRIVATE FAKE_PCAP_NO_OPEN)
if(WIN32)
  # Loading the fakes in a test does not show a DLL they need from PATH: the test program has the
  # toolchain's DLLs loaded already, and Windows takes those. So the list of what each needs is
  # checked itself, and a failure names the DLL.
  foreach(_fake ${_fakes})
    add_test(NAME eth_link.fake_pcap_imports.${_fake}
             COMMAND ${CMAKE_COMMAND} "-DOBJDUMP=${CMAKE_OBJDUMP}" -DDLL=$<TARGET_FILE:${_fake}>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/fake_pcap/check_dll_imports.cmake)
    set_tests_properties(eth_link.fake_pcap_imports.${_fake} PROPERTIES LABELS "unit;eth_link" TIMEOUT 60)
  endforeach()
endif()

vehicle_sil_add_harness_tests(vehicle_sil_eth_link_tests
  PREFIX eth_link LABELS unit eth_link TIMEOUT 60
  SOURCES
    test_fake_link.cpp
    test_frame_filter.cpp
    test_mac_address.cpp
    test_mac_side_table.cpp
    test_os_link_errors.cpp
    test_pcap_errors.cpp
    test_pcap_link.cpp
  LINK vehicle_sil_eth_link_fake)
# The messages of the OS links and the pcap link's table are in the library's own source directory.
target_include_directories(vehicle_sil_eth_link_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/eth_link/src
                                                              ${PROJECT_SOURCE_DIR}/libs/eth_link/src/pcap)
target_compile_definitions(vehicle_sil_eth_link_tests PRIVATE VEHICLE_SIL_FAKE_PCAP_DIR="${VEHICLE_SIL_FAKE_PCAP_DIR}")
add_dependencies(vehicle_sil_eth_link_tests ${_fakes})

# vehicle_sil::eth_link includes no SIL Kit header, also through the vehicle-sil headers it
# includes (it links nothing of vehicle-sil).
add_test(NAME eth_link.boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/eth_link
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
set_tests_properties(eth_link.boundary PROPERTIES LABELS "unit;eth_link" TIMEOUT 60)
