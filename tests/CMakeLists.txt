#
# Copyright 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# A new library defining all tests in the current directory and sub-directories.
# The following command will build and run these tests.
#
#   atest libgtest_example_test
#

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler options.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# The main test executable.
add_executable(
    ir_tests
    ir_tests.cc
)

# Adds the required dependencies to the test executable.
target_link_libraries(
    ir_tests
    GTest::gtest_main
    Axolotl_lib
)

include(GoogleTest)
gtest_discover_tests(ir_tests)