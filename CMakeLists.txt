# CMake build for exile-cpp — the cross-platform (primarily Linux) build.
#
# The canonical Windows build remains the Visual Studio solution
# (exile-cpp.sln / *.vcxproj); this file is purely additive and does not
# touch it. It produces the same two artefacts:
#   * exile        — the game (sokol_app + sokol_gfx, GL on Linux / *nix)
#   * exile_tests  — the headless test runner (NullRenderer, no GUI/audio)
#
# Quick start (Linux):
#   sudo apt install build-essential cmake \
#        libx11-dev libxi-dev libxcursor-dev libgl1-mesa-dev libasound2-dev
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ./build/exile_tests          # run tests (cwd-independent)
#   ./build/exile                # run the game (from the repo root so
#                                #   exile.ini / data/ resolve)

cmake_minimum_required(VERSION 3.16)
project(exile-cpp LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Default to Release for single-config generators when the caller didn't
# pick one — matches the Windows project's shipping configuration.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(EXILE_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/data
  ${CMAKE_CURRENT_SOURCE_DIR}/deps)

# Shared game/simulation sources, used by BOTH the game and the test exe.
# Excludes the two entry points (src/main.cpp, tests/main.cpp) and the
# sokol implementation TU (src/rendering/sokol_impl.cpp) — the latter is
# the only TU that pulls in the GL/X11 system headers and is game-only.
file(GLOB_RECURSE EXILE_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM EXILE_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/sokol_impl.cpp)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# exile — the game
# ---------------------------------------------------------------------------
add_executable(exile
  ${EXILE_CORE_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/sokol_impl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/fenster_audio_impl.c)
target_include_directories(exile PRIVATE ${EXILE_INCLUDE_DIRS})
target_link_libraries(exile PRIVATE Threads::Threads)

if(WIN32)
  # The .vcxproj is the supported Windows path; this branch exists so a
  # CMake/clang-cl build is at least wired correctly. SOKOL_D3D11 + winsock.
  target_link_libraries(exile PRIVATE winmm ws2_32)
elseif(APPLE)
  # sokol picks Metal here; audio uses fenster's AudioToolbox path.
  target_link_libraries(exile PRIVATE
    "-framework Cocoa" "-framework QuartzCore" "-framework Metal"
    "-framework MetalKit" "-framework AudioToolbox")
else()
  # Linux / *nix: sokol GLCORE over GLX/X11, audio over ALSA (libasound).
  find_package(X11 REQUIRED)
  find_package(OpenGL REQUIRED)
  find_package(ALSA REQUIRED)   # libasound; fenster links it directly
  target_link_libraries(exile PRIVATE
    X11::X11 X11::Xi X11::Xcursor
    OpenGL::GL
    ALSA::ALSA
    ${CMAKE_DL_LIBS} m)
endif()

# ---------------------------------------------------------------------------
# exile_tests — headless test runner (no window, no audio device)
# ---------------------------------------------------------------------------
# Deliberately dependency-free: built against NullRenderer, with audio
# compiled out (EXILE_NO_AUDIO) and the sokol impl / fenster impl TUs left
# out entirely. Result links with only the C++ runtime + pthreads, so the
# CI test job needs no system libraries at all.
file(GLOB EXILE_TEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(exile_tests
  ${EXILE_CORE_SOURCES}
  ${EXILE_TEST_SOURCES})
target_include_directories(exile_tests PRIVATE ${EXILE_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(exile_tests PRIVATE EXILE_NO_AUDIO)
target_link_libraries(exile_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME exile_tests COMMAND exile_tests)
