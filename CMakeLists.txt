cmake_minimum_required(VERSION 3.29)
# Disable C++ module dependency scanning so compile_commands.json works with clangd
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Axolotl)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Xbyak (JIT assembler for x86/x64) ───────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  xbyak
  GIT_REPOSITORY https://github.com/herumi/xbyak.git
  GIT_TAG        v7.33.3
)
FetchContent_MakeAvailable(xbyak)

# Output exe and dlls to out, have everything else (that are not required to run) in build-artifacts
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../out)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../out)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../out)

# ── DOP JIT Compiler Flags ────────────────────────────────────────────────
# -fno-exceptions & -fno-rtti: Prevents C++ from generating hidden vtables 
#  and stack-unwinding code, keeping the memory layout perfectly flat.
set(JIT_FLAGS "-fno-exceptions -fno-rtti")

# Automatically enable SIMD if compiling for x86_64 host (for the Decoder)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64.*|x86_64.*|AMD64.*")
    set(JIT_FLAGS "${JIT_FLAGS} -msse4.2 -mavx2")
endif()

# Optional: AddressSanitizer (Debug only; catches use-after-free, buffer overflows in host + JIT-triggered bad accesses)
# Usage: cmake -DUSE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug -B build-asan && cmake --build build-asan
option(USE_ASAN "Build with AddressSanitizer in Debug builds" OFF)
if(USE_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(ASAN_FLAGS "-fsanitize=address -fno-omit-frame-pointer -g")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ASAN_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${ASAN_FLAGS}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${ASAN_FLAGS}")
endif()

# Debug Profile (Strict warnings, debug symbols)
# DISASM_LAST_BLOCK: run Python+Capstone on last block's ARM/x86 before exit (run from project root).
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DB_DEBUG -DDISASM_LAST_BLOCK -Wall -Wextra -Wpedantic ${JIT_FLAGS}")

# Release Profile (Maximum speed, Link-Time Optimization)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -flto ${JIT_FLAGS}")

# ── GLFW + OpenGL ────────────────────────────────────────────────────────
# Download and build GLFW via FetchContent (no system install required)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

find_package(OpenGL REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cc" "src/*.cpp" "src/*.c")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "inc/*.h" "inc/*.hpp" "src/*.h" "src/*.hpp")
# Exclude main.cc so ir_dump can link against the lib without duplicate main
list(REMOVE_ITEM SOURCES "${CMAKE_SOURCE_DIR}/src/main.cc")

add_library(Axolotl_lib
        ${SOURCES}
        ${HEADERS}
)
target_include_directories(Axolotl_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/system
        ${CMAKE_CURRENT_SOURCE_DIR}/src/data
        ${CMAKE_CURRENT_SOURCE_DIR}/src/util
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
target_link_libraries(Axolotl_lib PUBLIC Qt6::Core Qt6::Widgets)
target_link_libraries(Axolotl_lib PRIVATE xbyak::xbyak)
if(UNIX AND NOT APPLE)
  find_package(ALSA)
  if(ALSA_FOUND)
    target_link_libraries(Axolotl_lib PRIVATE ALSA::ALSA)
    target_compile_definitions(Axolotl_lib PRIVATE AXOLOTL_HAS_ALSA=1)
  endif()
endif()
# These units use throw/catch (Xbyak, IR builder); compile with exceptions enabled.
set_source_files_properties(src/system/jit_emitter.cc src/system/jit_lifter.cc src/system/dispatcher.cc
    PROPERTIES COMPILE_FLAGS "-fexceptions -frtti")

add_executable(Axolotl src/main.cc)

target_link_libraries(Axolotl PRIVATE Axolotl_lib glfw OpenGL::GL Qt6::Core Qt6::Widgets)
if(UNIX)
  target_link_options(Axolotl PRIVATE -rdynamic)
endif()
target_link_options(Axolotl PRIVATE -static-libgcc -static-libstdc++)

# Copy res to out/build-debug (if debug build) or out/build-release (if release build)
if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/res")
  add_custom_command(TARGET Axolotl POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/res $<TARGET_FILE_DIR:Axolotl>/res)
endif()

include(CTest)
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
