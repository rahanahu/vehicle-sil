include_guard(GLOBAL)

# Warning flags for vehicle-sil's own targets (not for the third-party code).
function(vehicle_sil_set_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
    if(VEHICLE_SIL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Adds -fsanitize=<VEHICLE_SIL_SANITIZERS> to every target defined after this
# call in the calling directory and below, including the dependencies.
function(vehicle_sil_enable_sanitizers)
  if("${VEHICLE_SIL_SANITIZERS}" STREQUAL "")
    return()
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "VEHICLE_SIL_SANITIZERS requires GCC or Clang")
  endif()
  list(JOIN VEHICLE_SIL_SANITIZERS "," sanitizers)
  message(STATUS "vehicle-sil: building with -fsanitize=${sanitizers}")
  add_compile_options(-fsanitize=${sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${sanitizers})
endfunction()

# Settings every vehicle-sil target uses: C++20 without compiler extensions and the
# warning flags above. Libraries require C++20 of their users too.
function(vehicle_sil_target_defaults target)
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "EXECUTABLE")
    target_compile_features(${target} PRIVATE cxx_std_20)
  else()
    target_compile_features(${target} PUBLIC cxx_std_20)
  endif()
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  vehicle_sil_set_warnings(${target})
endfunction()

# On Windows, copies the DLLs a target depends on (SilKit.dll) next to it after
# each build so that it can be started without changing PATH. No-op elsewhere.
function(vehicle_sil_copy_runtime_dlls target)
  if(WIN32)
    add_custom_command(
      TARGET ${target}
      POST_BUILD
      # A target without runtime DLLs (no SIL Kit) gets "cmake -E true" instead of a copy
      # without source files, which fails.
      COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>,copy_if_different,true>
              $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
      COMMAND_EXPAND_LISTS
      VERBATIM)
  endif()
endfunction()
