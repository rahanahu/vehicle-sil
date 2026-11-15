# The ECU that the guide docs/howto/first-ecu.md adds to the ECU project template (template/): these
# are the four lines the guide appends to the template's CMakeLists.txt. tests/template adds this
# directory to the template's project in the build tree, so that cooler_ecu is built next to the
# template's engine_ecu and dashboard_ecu (bin/), and the test template.first_ecu compares the lines
# below, the other files here and the outputs with what the guide shows.
add_executable(cooler_ecu cooler/main.cpp)
target_include_directories(cooler_ecu PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(cooler_ecu PRIVATE vehicle_sil::simulation)
vehicle_sil_copy_runtime_dlls(cooler_ecu)
