# Cross-platform (mainly Linux) build. The Windows build stays on the VS
# solution; this is additive and doesn't touch it. Build docs in README.md.

cmake_minimum_required(VERSION 3.16)
project(exile-cpp LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(EXILE_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/data
  ${CMAKE_CURRENT_SOURCE_DIR}/deps)

# Sources shared by the game and the test exe. sokol_impl.cpp is the only TU
# that pulls in the GL/X11 headers, so it's game-only; main.cpp is too.
file(GLOB_RECURSE EXILE_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM EXILE_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/sokol_impl.cpp)

find_package(Threads REQUIRED)

add_executable(exile
  ${EXILE_CORE_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/sokol_impl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/fenster_audio_impl.c)
target_include_directories(exile PRIVATE ${EXILE_INCLUDE_DIRS})
target_link_libraries(exile PRIVATE Threads::Threads)

if(WIN32)
  target_link_libraries(exile PRIVATE winmm ws2_32)
elseif(APPLE)
  # Untested. sokol_impl.cpp picks SOKOL_GLCORE on all non-Windows, so this
  # is desktop GL (deprecated on macOS but working), not Metal.
  target_link_libraries(exile PRIVATE
    "-framework Cocoa" "-framework QuartzCore" "-framework OpenGL"
    "-framework AudioToolbox")
else()
  find_package(X11 REQUIRED)
  find_package(OpenGL REQUIRED)
  find_package(ALSA REQUIRED)
  target_link_libraries(exile PRIVATE
    X11::X11 X11::Xi X11::Xcursor OpenGL::GL ALSA::ALSA ${CMAKE_DL_LIBS} m)
endif()

# Headless tests: NullRenderer, audio compiled out, no sokol/fenster impl TUs,
# so this links with just the C++ runtime + pthreads.
file(GLOB EXILE_TEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(exile_tests ${EXILE_CORE_SOURCES} ${EXILE_TEST_SOURCES})
target_include_directories(exile_tests PRIVATE ${EXILE_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(exile_tests PRIVATE EXILE_NO_AUDIO)
target_link_libraries(exile_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME exile_tests COMMAND exile_tests)
