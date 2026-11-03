cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(CTest)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE MPS_ENABLE_FRONTEND_API)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# cmake-format: off
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/playlist.h
    src/playlist-context.h
    src/playlist-context.c
    src/playlist-telemetry.h
    src/playlist-telemetry.c
    src/duration-inventory.h
    src/duration-inventory.c
    src/playlist-vendor.h
    src/playlist-vendor.c
    src/obs-websocket-api.h
    src/playlist-dock.h
    src/playback-coordinator.h
    src/playback-coordinator.c
    src/media-playlist-source.h
    src/media-playlist-source.c
    src/shuffler.h
    src/shuffler.c
)
# cmake-format: on

if(ENABLE_FRONTEND_API AND ENABLE_QT)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/playlist-dock.hpp src/playlist-dock.cpp)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE MPS_ENABLE_PLAYLIST_DOCK)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(BUILD_TESTING)
  add_executable(playback-coordinator-test tests/playback-coordinator-test.c src/playback-coordinator.c)
  target_include_directories(playback-coordinator-test PRIVATE src)
  add_test(NAME playback-coordinator COMMAND playback-coordinator-test)

  add_executable(playlist-context-test tests/playlist-context-test.c src/playlist-context.c src/shuffler.c)
  target_include_directories(playlist-context-test PRIVATE src)
  target_link_libraries(playlist-context-test PRIVATE OBS::libobs)
  add_test(NAME playlist-context COMMAND playlist-context-test)

  add_executable(shuffler-test tests/shuffler-test.c src/shuffler.c)
  target_include_directories(shuffler-test PRIVATE src)
  target_compile_definitions(shuffler-test PRIVATE TEST_SHUFFLER)
  target_link_libraries(shuffler-test PRIVATE OBS::libobs)
  add_test(NAME shuffler COMMAND shuffler-test)

  add_executable(
    shuffle-activation-test
    tests/shuffle-activation-test.c
    src/shuffler.c
    src/playlist-context.c
    src/playback-coordinator.c
    src/playlist-telemetry.c
    src/duration-inventory.c
  )
  target_include_directories(shuffle-activation-test PRIVATE src)
  target_link_libraries(shuffle-activation-test PRIVATE OBS::libobs)
  add_test(NAME shuffle-activation COMMAND shuffle-activation-test)

  add_executable(
    playlist-telemetry-test
    tests/playlist-telemetry-test.c
    src/playlist-telemetry.c
    src/playlist-context.c
    src/shuffler.c
  )
  target_include_directories(playlist-telemetry-test PRIVATE src)
  target_link_libraries(playlist-telemetry-test PRIVATE OBS::libobs)
  add_test(NAME playlist-telemetry COMMAND playlist-telemetry-test)

  add_executable(duration-inventory-test tests/duration-inventory-test.c src/duration-inventory.c)
  target_include_directories(duration-inventory-test PRIVATE src)
  target_link_libraries(duration-inventory-test PRIVATE OBS::libobs)
  add_test(NAME duration-inventory COMMAND duration-inventory-test)

  add_executable(
    playlist-telemetry-runtime-test
    tests/playlist-telemetry-runtime-test.c
    src/playlist-telemetry.c
    src/playlist-context.c
    src/shuffler.c
    src/playback-coordinator.c
  )
  target_include_directories(playlist-telemetry-runtime-test PRIVATE src)
  target_link_libraries(playlist-telemetry-runtime-test PRIVATE OBS::libobs)
  add_test(NAME playlist-telemetry-runtime COMMAND playlist-telemetry-runtime-test)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(nlohmann_json 3.11 REQUIRED)
    add_executable(
      playlist-vendor-test
      tests/playlist-vendor-test.c
      tests/vendor-wire-json.cpp
      src/playlist-vendor.c
      src/playlist-telemetry.c
      src/playlist-context.c
      src/shuffler.c
    )
    target_include_directories(playlist-vendor-test PRIVATE src tests)
    target_compile_features(playlist-vendor-test PRIVATE cxx_std_17)
    target_link_libraries(playlist-vendor-test PRIVATE OBS::libobs nlohmann_json::nlohmann_json)
    add_test(NAME playlist-vendor-wire COMMAND playlist-vendor-test)
    set_tests_properties(playlist-vendor-wire PROPERTIES TIMEOUT 30)
  endif()
  set_tests_properties(duration-inventory playlist-telemetry-runtime PROPERTIES TIMEOUT 30)

  # Explicit opt-in runtime benchmark; requires the installed obs-ffmpeg module.
  add_executable(duration-probe-benchmark EXCLUDE_FROM_ALL tests/duration-probe-benchmark.c src/duration-inventory.c)
  target_include_directories(duration-probe-benchmark PRIVATE src)
  target_link_libraries(duration-probe-benchmark PRIVATE OBS::libobs)
endif()
