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
  PRIVATE src/plugin-main.c
          src/playlist.h
          src/playlist-context.h
          src/playlist-context.c
          src/playlist-dock.h
          src/playback-coordinator.h
          src/playback-coordinator.c
          src/media-playlist-source.h
          src/media-playlist-source.c
          src/shuffler.h
          src/shuffler.c)
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
endif()
