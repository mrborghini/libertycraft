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
add_executable(drive_test drive_test.cpp)
target_include_directories(drive_test PRIVATE ../src)
target_compile_options(drive_test PRIVATE -Wall -Wextra -O2)
enable_testing()
add_test(NAME link_test COMMAND link_test)

# combat/CombatMath.h (Stream X): actor ids, damage scaling, weapon classes, hurt pacing.
add_executable(combat_test combat_test.cpp)
target_include_directories(combat_test PRIVATE ../src ../../protocol)
target_compile_options(combat_test PRIVATE -Wall -Wextra -O2)
add_test(NAME combat_test COMMAND combat_test)

# Collision v2 geometry (collision/Geometry.h) against a synthetic one-sided ray caster.
add_executable(collision_test collision_test.cpp)
target_include_directories(collision_test PRIVATE ../src ../../protocol)
target_compile_options(collision_test PRIVATE -Wall -Wextra -O2)
add_test(NAME collision_test COMMAND collision_test)

# render/RenderMath.h (Stream R): camera matrices, frustum, lighting, mips, section meshes, entities.
add_executable(render_test render_test.cpp)
target_include_directories(render_test PRIVATE ../src ../../protocol)
target_compile_options(render_test PRIVATE -Wall -Wextra -O2)
add_test(NAME render_test COMMAND render_test)
add_test(NAME drive_test COMMAND drive_test)

# render/Body.h (Stream K): the Minecraft body's parts on GTA's skeleton.
add_executable(body_test body_test.cpp)
target_include_directories(body_test PRIVATE ../src ../../protocol)
target_compile_options(body_test PRIVATE -Wall -Wextra -O2)
add_test(NAME body_test COMMAND body_test)

# hazard/HazardGrid.h (Stream K): Minecraft's fire, lava and magma for GTA's peds.
add_executable(hazard_test hazard_test.cpp)
target_include_directories(hazard_test PRIVATE ../src ../../protocol)
target_compile_options(hazard_test PRIVATE -Wall -Wextra -O2)
add_test(NAME hazard_test COMMAND hazard_test)
