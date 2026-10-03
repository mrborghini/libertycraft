# CMake toolchain: cross-compile a 32-bit Windows DLL from Linux with clang-cl + lld-link and an
# xwin sysroot (Microsoft CRT + Windows SDK headers/libs, see tools/xwin-splat).
#
#   XWIN_DIR  env var or cache var; default <repo>/.tools/xwin (what `xwin splat --output` wrote:
#             <xwin>/crt/{include,lib/x86} and <xwin>/sdk/{include/{ucrt,um,shared},lib/{ucrt,um}/x86}).
#   LLVM tools are taken from PATH (clang-cl, lld-link, llvm-rc, llvm-lib); set LLVM_BIN to override.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR X86)

if(NOT XWIN_DIR)
	if(DEFINED ENV{XWIN_DIR} AND NOT "$ENV{XWIN_DIR}" STREQUAL "")
		set(XWIN_DIR "$ENV{XWIN_DIR}")
	else()
		get_filename_component(XWIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.tools/xwin" ABSOLUTE)
	endif()
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "xwin splat output (MSVC CRT + Windows SDK)")
if(NOT EXISTS "${XWIN_DIR}/crt/include" OR NOT EXISTS "${XWIN_DIR}/sdk/include/um")
	message(FATAL_ERROR "xwin sysroot not found at ${XWIN_DIR} (expected crt/include and sdk/include/um). "
		"Run: xwin --accept-license --arch x86 --arch x86_64 splat --output ${XWIN_DIR}")
endif()

set(_llvm_bin "$ENV{LLVM_BIN}")
if(_llvm_bin)
	set(_llvm_bin "${_llvm_bin}/")
endif()
set(CMAKE_C_COMPILER   "${_llvm_bin}clang-cl" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${_llvm_bin}clang-cl" CACHE FILEPATH "")
set(CMAKE_LINKER       "${_llvm_bin}lld-link" CACHE FILEPATH "")
set(CMAKE_RC_COMPILER  "${_llvm_bin}llvm-rc"  CACHE FILEPATH "")
set(CMAKE_AR           "${_llvm_bin}llvm-lib" CACHE FILEPATH "")
set(CMAKE_MT "" CACHE FILEPATH "no manifest tool when cross-compiling")

set(_target i686-pc-windows-msvc)
set(CMAKE_C_COMPILER_TARGET   ${_target})
set(CMAKE_CXX_COMPILER_TARGET ${_target})

# Where the MSVC CRT and the Windows SDK live. clang-cl turns these into the right include dirs;
# lld-link needs the lib dirs spelled out because CMake calls it directly.
set(_sysroot_flags "/vctoolsdir \"${XWIN_DIR}/crt\" /winsdkdir \"${XWIN_DIR}/sdk\"")
set(CMAKE_C_FLAGS_INIT   "${_sysroot_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_sysroot_flags}")
set(_libpaths "/libpath:\"${XWIN_DIR}/crt/lib/x86\" /libpath:\"${XWIN_DIR}/sdk/lib/um/x86\" /libpath:\"${XWIN_DIR}/sdk/lib/ucrt/x86\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "/machine:x86 ${_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "/machine:x86 ${_libpaths}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "/machine:x86 ${_libpaths}")
set(CMAKE_RC_FLAGS_INIT "/I \"${XWIN_DIR}/sdk/include/um\" /I \"${XWIN_DIR}/sdk/include/shared\"")

# Don't try to run or fully link test programs during compiler detection.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
