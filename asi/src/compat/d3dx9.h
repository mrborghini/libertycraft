// IV-SDK's IVSDK.cpp does `#include <d3dx9.h>` but never uses a D3DX symbol. The D3DX utility
// headers aren't in the Windows SDK (they were in the June 2010 DirectX SDK), so this stand-in,
// first on the include path, gives it the plain Direct3D 9 header instead.
#pragma once
#include <d3d9.h>
