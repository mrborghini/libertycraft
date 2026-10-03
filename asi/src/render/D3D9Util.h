// Small Direct3D 9 helpers shared by the block renderer and the overlay (render thread only).
#pragma once

#include "Log.h"

#include <windows.h>

#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace lc::render
{
	template <class T>
	inline void SafeRelease(T*& a_ptr)
	{
		if (a_ptr) {
			a_ptr->Release();
			a_ptr = nullptr;
		}
	}

	// D3DCompile from d3dcompiler_47.dll (Wine/Proton ship a builtin one on vkd3d-shader), else
	// d3dcompiler_43.dll. Null if neither loads.
	inline pD3DCompile CompilerEntry()
	{
		static pD3DCompile fn = [] {
			for (const wchar_t* name : { L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll" }) {
				if (HMODULE m = ::LoadLibraryW(name)) {
					if (auto f = reinterpret_cast<pD3DCompile>(::GetProcAddress(m, "D3DCompile"))) {
						::lc::log::Write("render", "shader compiler: %ls", name);
						return f;
					}
				}
			}
			::lc::log::Write("render", "ERROR: no d3dcompiler_4x.dll with D3DCompile: Minecraft's blocks and HUD can't be drawn");
			return static_cast<pD3DCompile>(nullptr);
		}();
		return fn;
	}

	// HLSL -> shader bytecode. Logs the compiler's messages on failure.
	inline bool CompileShader(const char* a_src, const char* a_name, const char* a_entry, const char* a_target, std::vector<DWORD>& a_out)
	{
		const auto compile = CompilerEntry();
		if (!compile) {
			return false;
		}
		ID3DBlob*     code = nullptr;
		ID3DBlob*     errors = nullptr;
		const HRESULT hr = compile(a_src, std::strlen(a_src), a_name, nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr) || !code) {
			::lc::log::Write("render", "ERROR: shader %s %s (%s) failed: 0x%08lX %s", a_name, a_entry, a_target, static_cast<unsigned long>(hr),
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			SafeRelease(errors);
			SafeRelease(code);
			return false;
		}
		a_out.resize((code->GetBufferSize() + 3) / 4);
		std::memcpy(a_out.data(), code->GetBufferPointer(), code->GetBufferSize());
		SafeRelease(errors);
		SafeRelease(code);
		return true;
	}

	// Columns of a row-vector matrix as four float4 shader constants (dot(v, cI) = clip.I).
	inline void MatrixColumns(const float a_m[4][4], float a_out[16])
	{
		for (int c = 0; c < 4; ++c) {
			for (int r = 0; r < 4; ++r) {
				a_out[c * 4 + r] = a_m[r][c];
			}
		}
	}

	// Copies a_rows rows of a_rowBytes from a tightly packed source into a locked rect.
	inline void CopyRows(void* a_dst, int a_pitch, const std::uint8_t* a_src, std::uint32_t a_rowBytes, std::uint32_t a_rows)
	{
		auto* d = static_cast<std::uint8_t*>(a_dst);
		if (static_cast<std::uint32_t>(a_pitch) == a_rowBytes) {
			std::memcpy(d, a_src, std::size_t(a_rowBytes) * a_rows);
			return;
		}
		for (std::uint32_t y = 0; y < a_rows; ++y) {
			std::memcpy(d + std::size_t(y) * a_pitch, a_src + std::size_t(y) * a_rowBytes, a_rowBytes);
		}
	}
}
