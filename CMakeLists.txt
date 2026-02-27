cmake_minimum_required(VERSION 3.29)
# Disable C++ module dependency scanning so compile_commands.json works with clangd
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(Axolotl)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src inc)

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

# Debug Profile (Strict warnings, debug symbols)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DB_DEBUG -Wall -Wextra -Wpedantic ${JIT_FLAGS}")

# Release Profile (Maximum speed, Link-Time Optimization)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -flto ${JIT_FLAGS}")

# ── GLFW + OpenGL ────────────────────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # Cross-compiling to Windows: build GLFW from source (no host glfw needed)
  include(FetchContent)
  FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
  )
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw)
else()
  find_package(glfw3 REQUIRED)
endif()
find_package(OpenGL REQUIRED)

file(GLOB_RECURSE SOURCES "src/*.cc" "src/*.cpp" "src/*.c")
file(GLOB_RECURSE HEADERS "inc/*.h" "inc/*.hpp" "src/*.h" "src/*.hpp")
# Exclude main.cc so ir_dump can link against the lib without duplicate main
list(REMOVE_ITEM SOURCES "${CMAKE_SOURCE_DIR}/src/main.cc")

add_library(Axolotl_lib
        ${SOURCES}
        ${HEADERS}
)
target_link_libraries(Axolotl_lib PRIVATE xbyak::xbyak)

add_executable(Axolotl src/main.cc)

target_link_libraries(Axolotl PRIVATE Axolotl_lib glfw OpenGL::GL)
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

# ir_dump: dump IR for given ARM instruction hex (for fuzzer / manual comparison)
add_executable(ir_dump tools/ir_dump.cc)
target_link_libraries(ir_dump PRIVATE Axolotl_lib)

add_subdirectory(tests)
