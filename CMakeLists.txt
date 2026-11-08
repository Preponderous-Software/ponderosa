cmake_minimum_required(VERSION 3.24)
project(ponderosa VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Strata, pinned to a full commit SHA (see strata.ref). Not cached, so a new ref reaches an existing build.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/strata.ref" _strata_ref)
string(STRIP "${_strata_ref}" _strata_ref)
option(PONDEROSA_WITH_WINDOW "Build Strata's SDL3 window surface (interactive viewer)" ON)

include(FetchContent)
set(STRATA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(STRATA_WITH_USD OFF CACHE BOOL "" FORCE)
set(STRATA_WITH_SDL ${PONDEROSA_WITH_WINDOW} CACHE BOOL "" FORCE)
FetchContent_Declare(strata
  GIT_REPOSITORY https://github.com/Preponderous-Software/strata.git
  GIT_TAG ${_strata_ref})
FetchContent_MakeAvailable(strata)

# streetkit, the map library Ponderosa is built on, pinned like Strata (see streetkit.ref).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/streetkit.ref" _streetkit_ref)
string(STRIP "${_streetkit_ref}" _streetkit_ref)
set(STREETKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(streetkit
  GIT_REPOSITORY https://github.com/Preponderous-Software/streetkit.git
  GIT_TAG ${_streetkit_ref})
FetchContent_MakeAvailable(streetkit)

add_library(ponderosa_tiles INTERFACE)
target_include_directories(ponderosa_tiles INTERFACE app)
target_link_libraries(ponderosa_tiles INTERFACE streetkit::streetkit)

add_executable(ponderosa app/main.cpp)
target_link_libraries(ponderosa PRIVATE ponderosa_tiles strata::static)
target_compile_definitions(ponderosa PRIVATE PONDEROSA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT MSVC)
  target_compile_options(ponderosa PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_executable(test_app tests/cpp/test_app.cpp)
target_link_libraries(test_app PRIVATE ponderosa_tiles)
target_compile_definitions(test_app PRIVATE PONDEROSA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME app_units COMMAND test_app)
# Headless render (needs build/tiles from `uv run ponderosa build`), passing through enough tiles to unload some.
add_test(NAME render_fly
  COMMAND $<TARGET_FILE:ponderosa> --headless --width 640 --height 360 --fly --altitude 120 --yaw 90 --pitch -35 --walk-ahead 3
          --tiles ${CMAKE_CURRENT_SOURCE_DIR}/build/tiles --screenshot ${CMAKE_CURRENT_BINARY_DIR}/fly.png)
add_test(NAME render_overview
  COMMAND $<TARGET_FILE:ponderosa> --headless --width 640 --height 360 --fly --altitude 400 --yaw 90 --pitch -30 --radius 300
          --tiles ${CMAKE_CURRENT_SOURCE_DIR}/build/tiles --screenshot ${CMAKE_CURRENT_BINARY_DIR}/overview.png)
add_test(NAME render_walk
  COMMAND $<TARGET_FILE:ponderosa> --headless --width 640 --height 360 --yaw 90 --radius 300 --pass-east -900
          --tiles ${CMAKE_CURRENT_SOURCE_DIR}/build/tiles --screenshot ${CMAKE_CURRENT_BINARY_DIR}/walk.png)
set_tests_properties(render_walk render_fly render_overview PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe")
