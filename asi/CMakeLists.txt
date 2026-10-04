cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)  # CMAKE_MSVC_RUNTIME_LIBRARY
project(LibertyCraft LANGUAGES CXX)

# Configure with the toolchain file (cmake --preset release); a native MSVC configure also works
# in principle but is untested.
if(NOT WIN32)
	message(FATAL_ERROR "LibertyCraft.asi is a Windows x86 DLL; configure with -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake (or `cmake --preset release`).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "GTA IV is a 32-bit game; this must be an x86 build (got ${CMAKE_SIZEOF_VOID_P}-byte pointers).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # /MT: no MSVCRT dependency, nothing to ship

# ---- IV-SDK: a patched copy of the header-only SDK -------------------------------------------
# The submodule is GPL-3.0 and kept pristine. Two of its constructs don't compile with clang:
#   * `*reinterpret_cast<T*>(nullptr)` (NewAddressSet.h): MSVC lets you reinterpret_cast a nullptr_t
# so at configure time the headers are copied into the build tree with those lines rewritten, and
# that copy is what dllmain.cpp includes. Re-run CMake after updating the submodule.
set(IVSDK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/extern/iv-sdk/include")
set(IVSDK_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/ivsdk-patched/include")
if(NOT EXISTS "${IVSDK_SRC}/IVSDK.cpp")
	message(FATAL_ERROR "IV-SDK submodule missing: run `git submodule update --init` in the repo root.")
endif()
file(GLOB_RECURSE _ivsdk_files RELATIVE "${IVSDK_SRC}" "${IVSDK_SRC}/*")
foreach(_rel IN LISTS _ivsdk_files)
	set(_src "${IVSDK_SRC}/${_rel}")
	set(_dst "${IVSDK_PATCHED}/${_rel}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
	if(NOT EXISTS "${_dst}" OR "${_src}" IS_NEWER_THAN "${_dst}")
		file(READ "${_src}" _text)
		string(REPLACE "*reinterpret_cast<T*>(nullptr)" "*static_cast<T*>(nullptr)" _text "${_text}")
		get_filename_component(_dir "${_dst}" DIRECTORY)
		file(MAKE_DIRECTORY "${_dir}")
		file(WRITE "${_dst}" "${_text}")
	endif()
endforeach()
unset(_text)

# ---- the plugin -------------------------------------------------------------------------------
# dllmain.cpp is the single translation unit that includes IV-SDK (its headers define non-inline
# globals), and it #includes the SDK-dependent sources below unity-style. Everything else is
# SDK-free and compiles on its own.
set(LC_SDK_UNITY_SOURCES
	src/Game.cpp
	src/HostDrive.cpp
	src/Input.cpp
	src/Collision.cpp
	src/Render.cpp
	src/Overlay.cpp
	src/Combat.cpp
	src/MobFight.cpp
	src/SkyControl.cpp
	src/NpcBlocks.cpp
	src/Doors.cpp
	src/NikoBody.cpp
	src/Hazards.cpp
	src/ViewportRoom.cpp
	src/BlockyCity.cpp
	src/PauseMenu.cpp
)
set(LC_SOURCES
	src/dllmain.cpp
	src/Log.cpp
	src/Config.cpp
	src/Link.cpp
	src/CrashLog.cpp
	src/render/World.cpp
	src/render/ShadowPass.cpp
)
set(LC_HEADERS
	src/Log.h src/Config.h src/Link.h src/LinkCore.h src/Coords.h src/Perf.h src/CrashLog.h
	src/Game.h src/Input.h src/Collision.h src/Render.h src/Overlay.h src/Sdk.h
	src/Combat.h src/combat/CombatMath.h src/NpcBlocks.h src/combat/BlockPush.h
	src/render/Frame.h src/render/RenderMath.h src/render/Shaders.h src/render/D3D9Util.h src/render/World.h src/render/Shadows.h src/render/ShadowPass.h
	src/HostDrive.h src/DriveLogic.h src/Doors.h src/NikoBody.h src/render/Body.h src/Hazards.h src/hazard/HazardGrid.h src/ViewportRoom.h src/BlockyCity.h src/PauseMenu.h src/MobFight.h src/SkyControl.h
	src/compat/d3dx9.h
	../protocol/libertycraft_protocol.h
)
add_library(LibertyCraft SHARED ${LC_SOURCES} ${LC_HEADERS} ${LC_SDK_UNITY_SOURCES})
set_source_files_properties(${LC_SDK_UNITY_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)
set_target_properties(LibertyCraft PROPERTIES
	OUTPUT_NAME "LibertyCraft"
	PREFIX ""
	SUFFIX ".asi"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
target_include_directories(LibertyCraft PRIVATE
	src/compat           # d3dx9.h stand-in, must come before the SDK include dirs
	src
	"${CMAKE_CURRENT_SOURCE_DIR}/../protocol"
)
target_include_directories(LibertyCraft SYSTEM PRIVATE "${IVSDK_PATCHED}")
target_compile_definitions(LibertyCraft PRIVATE
	_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
	LC_VERSION_STRING="0.1.0"
)
target_compile_options(LibertyCraft PRIVATE
	/W3 /EHsc /bigobj /Zi
	# IV-SDK: VALIDATE_OFFSET on non-standard-layout classes, _CRT_SECURE_NO_WARNINGS redefined
	-Wno-invalid-offsetof -Wno-macro-redefined
	# naked functions with inline asm (the SDK's hooks)
	-Wno-unused-command-line-argument
)
# /RTC1 (CMake's MSVC debug default) isn't supported by clang-cl.
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
target_link_options(LibertyCraft PRIVATE
	/safeseh:no     # the SDK's __declspec(naked) hooks have no SEH tables
	/debug:full     # LibertyCraft.pdb next to the .asi, for CrashLog's addresses
	/incremental:no
)
target_link_libraries(LibertyCraft PRIVATE kernel32 user32 d3d9 version)

# Where the game expects it: <gamedir>/plugins/LibertyCraft.asi (tools/launch.sh copies it).
add_custom_command(TARGET LibertyCraft POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E echo "built $<TARGET_FILE:LibertyCraft>"
)
