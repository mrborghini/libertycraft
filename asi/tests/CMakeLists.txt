# Host-side (Linux) unit tests for the portable parts of the plugin: LinkCore.h (seqlocks, rings,
# overlay swap, header reset) and Coords.h. Not part of the Windows build.
#   cmake -S asi/tests -B asi/build/tests && cmake --build asi/build/tests && ctest --test-dir asi/build/tests
cmake_minimum_required(VERSION 3.21)
project(LibertyCraftTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
add_executable(link_test link_test.cpp)
target_include_directories(link_test PRIVATE ../src ../../protocol)
target_compile_options(link_test PRIVATE -Wall -Wextra -O2)
# GCC cannot see that ring positions are 8-aligned (pad headers never straddle the end).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options(link_test PRIVATE -Wno-stringop-overflow)
endif()
target_link_libraries(link_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME link_test COMMAND link_test)
