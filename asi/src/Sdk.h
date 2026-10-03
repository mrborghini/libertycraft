// Guard for the sources that use IV-SDK (Game, Input, Collision, Render, Overlay). IV-SDK's headers
// define non-inline globals, so it is included exactly once, by dllmain.cpp, which then
// #includes those sources (unity build; CMake marks them HEADER_FILE_ONLY).
#pragma once

#ifndef LC_IVSDK_INCLUDED
#	error "This file uses IV-SDK: it is compiled as part of dllmain.cpp, not on its own."
#endif
