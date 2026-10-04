// See OpaqueDepth.h. SDK-free: the device comes from Render.cpp.
#define LC_MODULE "render"
#include "render/OpaqueDepth.h"

#include "render/D3D9Util.h"
#include "render/Shaders.h"

#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace lc::render::opaquedepth
{
	namespace
	{
		// IDirect3DDevice9 vtable slots.
		constexpr unsigned kSlotReset = 16, kSlotSetRenderTarget = 37, kSlotSetTexture = 65;

		using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
		using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
		using SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);

		ResetFn           oReset = nullptr;
		SetRenderTargetFn oSetRenderTarget = nullptr;
		SetTextureFn      oSetTexture = nullptr;
		bool              hooksTried = false, hooksOk = false;
		std::uint32_t     hookCalls = 0;  // SetTexture calls seen (the install's self-check)

		IDirect3DDevice9* device = nullptr;  // the one our resources belong to
		bool              ours = false;      // our draw command is drawing
		bool              busy = false;      // a copy is being made (its own device calls)
		bool              armed = false;     // the last drawn command drew depth-tested blocks: copy
		bool              gbuffer = false;   // GTA's G-buffer pass ran since our last draw command
		bool              taken = false;     // this frame's copy was tried
		bool              copied = false;    // ... and is in `snap`
		bool              latched = false;   // the drawing command's frame has a copy (Restore uses it)

		IDirect3DBaseTexture9* sceneTex = nullptr;  // GTA's scene depth texture (compared only)
		IDirect3DSurface9*     sceneDs = nullptr;   // its surface (compared only)
		std::uint32_t          sceneW = 0, sceneH = 0;

		IDirect3DTexture9*           snap = nullptr;  // D3DPOOL_DEFAULT R32F
		IDirect3DSurface9*           snapSurface = nullptr;
		std::uint32_t                snapW = 0, snapH = 0, failedW = 0, failedH = 0;
		bool                         shadersTried = false, shadersOk = false;
		IDirect3DVertexShader9*      vs = nullptr;
		IDirect3DPixelShader9*       copyPs = nullptr;
		IDirect3DPixelShader9*       restorePs = nullptr;
		IDirect3DVertexDeclaration9* decl = nullptr;

		struct Stats
		{
			std::uint32_t drawn = 0, wanted = 0, gbuffers = 0, copies = 0, restored = 0, failed = 0;
			double        copyMs = 0.0, restoreMs = 0.0;
			double        copyGpuMs = 0.0, restoreGpuMs = 0.0;
			std::uint32_t gpuFrames = 0;
		} stats;
		std::uint64_t nextStatsLog = 0;
		bool          loggedFirstCopy = false, loggedFirstRestore = false;
		int           lastAB = -1;

		double NowMs()
		{
			static const double freq = [] {
				LARGE_INTEGER f;
				::QueryPerformanceFrequency(&f);
				return double(f.QuadPart) / 1000.0;
			}();
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return double(t.QuadPart) / freq;
		}

		bool Patch(void** a_slot, void* a_value)
		{
			DWORD old = 0;
			if (!::VirtualProtect(a_slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
				return false;
			}
			*a_slot = a_value;
			::VirtualProtect(a_slot, sizeof(void*), old, &old);
			::FlushInstructionCache(::GetCurrentProcess(), a_slot, sizeof(void*));
			return true;
		}

		template <class F>
		bool Hook(void** a_vtbl, unsigned a_slot, F a_hook, F& a_orig)
		{
			void* was = a_vtbl[a_slot];
			if (!Patch(a_vtbl + a_slot, reinterpret_cast<void*>(a_hook))) {
				return false;
			}
			a_orig = reinterpret_cast<F>(was);
			return true;
		}

		const char* Fmt(D3DFORMAT a_f, char (&a_buf)[16])
		{
			switch (a_f) {
			case D3DFMT_A8R8G8B8: return "A8R8G8B8";
			case D3DFMT_X8R8G8B8: return "X8R8G8B8";
			case D3DFMT_A2R10G10B10: return "A2R10G10B10";
			case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
			case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
			case D3DFMT_R32F: return "R32F";
			case D3DFMT_R16F: return "R16F";
			case D3DFMT_G16R16F: return "G16R16F";
			case D3DFMT_G32R32F: return "G32R32F";
			case D3DFMT_D24S8: return "D24S8";
			case D3DFMT_D24X8: return "D24X8";
			case D3DFMT_D16: return "D16";
			case D3DFMT_D32F_LOCKABLE: return "D32F";
			case D3DFMT_D24FS8: return "D24FS8";
			case D3DFMT_DXT1: return "DXT1";
			case D3DFMT_DXT5: return "DXT5";
			default: break;
			}
			const auto v = static_cast<std::uint32_t>(a_f);
			if (v > 0xFFFF) {
				std::snprintf(a_buf, sizeof(a_buf), "%c%c%c%c", char(v), char(v >> 8), char(v >> 16), char(v >> 24));
			} else {
				std::snprintf(a_buf, sizeof(a_buf), "fmt%u", v);
			}
			return a_buf;
		}

		// ---- DebugFrameTrace: GTA's device calls from one drawn frame's draw command to the next ----------
		// (to <gamedir>/libertycraft-frametrace-N.txt; render targets, depth buffers, clears, the render
		// target and depth textures bound, and the draws between, counted by depth write and blending)
		namespace trace
		{
			using StretchRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
			using SetDepthStencilSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
			using ClearFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
			using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
			using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
			using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
			using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

			StretchRectFn            oStretchRect = nullptr;
			SetRenderTargetFn        oSetRenderTargetT = nullptr;
			SetDepthStencilSurfaceFn oSetDepthStencilSurface = nullptr;
			ClearFn                  oClear = nullptr;
			SetTextureFn             oSetTextureT = nullptr;
			DrawPrimitiveFn          oDrawPrimitive = nullptr;
			DrawIndexedPrimitiveFn   oDrawIndexedPrimitive = nullptr;
			DrawPrimitiveUPFn        oDrawPrimitiveUP = nullptr;
			DrawIndexedPrimitiveUPFn oDrawIndexedPrimitiveUP = nullptr;

			bool                     parsed = false, installed = false, recording = false;
			std::vector<float>       times;  // s after the first drawn frame
			std::size_t              nextTime = 0;
			std::uint64_t            t0 = 0;
			int                      fileIndex = 0;
			IDirect3DSurface9*       curDs = nullptr;
			IDirect3DSurface9*       curRt = nullptr;
			std::vector<std::string> lines;
			std::unordered_map<const void*, int> ids;

			struct Run
			{
				std::uint32_t  n = 0, zw = 0, blend = 0, blendZw = 0, noZ = 0, noColor = 0, stencil = 0;
				std::uint32_t  psCount = 0;
				std::uint32_t  psDraws[8]{};
				std::uintptr_t ps[8]{};
			} run;

			bool Active()
			{
				return recording && !ours && !busy;
			}

			int Id(const void* a_p)
			{
				if (!a_p) {
					return 0;
				}
				const auto it = ids.find(a_p);
				if (it != ids.end()) {
					return it->second;
				}
				const int id = int(ids.size()) + 1;
				ids.emplace(a_p, id);
				return id;
			}

			void Line(const char* a_fmt, ...)
			{
				if (lines.size() >= 6000) {
					return;
				}
				char    buf[512];
				va_list a;
				va_start(a, a_fmt);
				std::vsnprintf(buf, sizeof(buf), a_fmt, a);
				va_end(a);
				lines.emplace_back(buf);
			}

			std::string Surf(IDirect3DSurface9* a_s)
			{
				if (!a_s) {
					return "null";
				}
				D3DSURFACE_DESC d{};
				a_s->GetDesc(&d);
				IDirect3DTexture9* tex = nullptr;
				int                texId = 0;
				if (SUCCEEDED(a_s->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) && tex) {
					texId = Id(tex);
					tex->Release();
				}
				char f[16], buf[160];
				std::snprintf(buf, sizeof(buf), "s%d %ux%u %s ms%u%s", Id(a_s), d.Width, d.Height, Fmt(d.Format, f), unsigned(d.MultiSampleType),
					a_s == sceneDs ? " SCENE-DS" : "");
				std::string out = buf;
				if (texId) {
					std::snprintf(buf, sizeof(buf), " (tex t%d)", texId);
					out += buf;
				}
				return out;
			}

			void FlushRun()
			{
				if (!run.n) {
					return;
				}
				char ps[200] = "";
				int  k = 0;
				for (std::uint32_t i = 0; i < run.psCount && k < int(sizeof(ps)) - 24; ++i) {
					k += std::snprintf(ps + k, sizeof(ps) - k, " ps%d x%u", Id(reinterpret_cast<const void*>(run.ps[i])), run.psDraws[i]);
				}
				Line("  draws %u: zwrite %u, blend %u, blend+zwrite %u%s, no ztest %u, no colour %u, stencil %u", run.n, run.zw, run.blend, run.blendZw, ps, run.noZ,
					run.noColor, run.stencil);
				run = Run{};
			}

			void OnDraw(IDirect3DDevice9* a_d)
			{
				if (!Active()) {
					return;
				}
				DWORD z = 0, zw = 0, ab = 0, cw = 0, st = 0;
				a_d->GetRenderState(D3DRS_ZENABLE, &z);
				a_d->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
				a_d->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
				a_d->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
				a_d->GetRenderState(D3DRS_STENCILENABLE, &st);
				++run.n;
				const bool zOn = z != D3DZB_FALSE && curDs;
				run.zw += (zOn && zw) ? 1 : 0;
				run.blend += ab ? 1 : 0;
				run.noZ += zOn ? 0 : 1;
				run.noColor += cw == 0 ? 1 : 0;
				run.stencil += st ? 1 : 0;
				if (zOn && zw && ab) {
					++run.blendZw;
					IDirect3DPixelShader9* p = nullptr;
					a_d->GetPixelShader(&p);
					const auto ptr = reinterpret_cast<std::uintptr_t>(p);
					if (p) {
						p->Release();
					}
					std::uint32_t i = 0;
					while (i < run.psCount && run.ps[i] != ptr) {
						++i;
					}
					if (i < run.psCount) {
						++run.psDraws[i];
					} else if (run.psCount < 8) {
						run.ps[run.psCount] = ptr;
						run.psDraws[run.psCount++] = 1;
					}
				}
			}

			HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DDevice9* a_d, IDirect3DSurface9* a_s, const RECT* a_sr, IDirect3DSurface9* a_t, const RECT* a_tr,
				D3DTEXTUREFILTERTYPE a_f)
			{
				if (Active()) {
					FlushRun();
					Line("StretchRect %s -> %s", Surf(a_s).c_str(), Surf(a_t).c_str());
				}
				return oStretchRect(a_d, a_s, a_sr, a_t, a_tr, a_f);
			}

			HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DDevice9* a_d, DWORD a_i, IDirect3DSurface9* a_s)
			{
				if (a_i == 0) {
					curRt = a_s;
				}
				if (Active()) {
					FlushRun();
					Line("RT%lu = %s", a_i, Surf(a_s).c_str());
				}
				return oSetRenderTargetT(a_d, a_i, a_s);
			}

			HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DDevice9* a_d, IDirect3DSurface9* a_s)
			{
				curDs = a_s;
				if (Active()) {
					FlushRun();
					Line("DS = %s", Surf(a_s).c_str());
				}
				return oSetDepthStencilSurface(a_d, a_s);
			}

			HRESULT STDMETHODCALLTYPE Clear(IDirect3DDevice9* a_d, DWORD a_n, const D3DRECT* a_r, DWORD a_flags, D3DCOLOR a_c, float a_z, DWORD a_s)
			{
				if (Active()) {
					FlushRun();
					Line("Clear%s%s%s rects %lu colour %08lX z %.3f stencil %lu", (a_flags & D3DCLEAR_TARGET) ? " target" : "", (a_flags & D3DCLEAR_ZBUFFER) ? " z" : "",
						(a_flags & D3DCLEAR_STENCIL) ? " stencil" : "", a_n, static_cast<unsigned long>(a_c), a_z, a_s);
				}
				return oClear(a_d, a_n, a_r, a_flags, a_c, a_z, a_s);
			}

			HRESULT STDMETHODCALLTYPE SetTexture(IDirect3DDevice9* a_d, DWORD a_stage, IDirect3DBaseTexture9* a_t)
			{
				if (Active() && a_t && a_t->GetType() == D3DRTYPE_TEXTURE) {
					D3DSURFACE_DESC d{};
					if (SUCCEEDED(static_cast<IDirect3DTexture9*>(a_t)->GetLevelDesc(0, &d)) && (d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
						FlushRun();
						char f[16];
						Line("  tex %lu = t%d %ux%u %s%s", a_stage, Id(a_t), d.Width, d.Height, Fmt(d.Format, f), a_t == sceneTex ? " SCENE-DEPTH-TEX" : "");
					}
				}
				return oSetTextureT(a_d, a_stage, a_t);
			}

			HRESULT STDMETHODCALLTYPE DrawPrimitive(IDirect3DDevice9* a_d, D3DPRIMITIVETYPE a_t, UINT a_s, UINT a_n)
			{
				OnDraw(a_d);
				return oDrawPrimitive(a_d, a_t, a_s, a_n);
			}

			HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(IDirect3DDevice9* a_d, D3DPRIMITIVETYPE a_t, INT a_b, UINT a_mi, UINT a_nv, UINT a_si, UINT a_pc)
			{
				OnDraw(a_d);
				return oDrawIndexedPrimitive(a_d, a_t, a_b, a_mi, a_nv, a_si, a_pc);
			}

			HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(IDirect3DDevice9* a_d, D3DPRIMITIVETYPE a_t, UINT a_n, const void* a_v, UINT a_st)
			{
				OnDraw(a_d);
				return oDrawPrimitiveUP(a_d, a_t, a_n, a_v, a_st);
			}

			HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(IDirect3DDevice9* a_d, D3DPRIMITIVETYPE a_t, UINT a_mi, UINT a_nv, UINT a_pc, const void* a_i, D3DFORMAT a_if,
				const void* a_v, UINT a_st)
			{
				OnDraw(a_d);
				return oDrawIndexedPrimitiveUP(a_d, a_t, a_mi, a_nv, a_pc, a_i, a_if, a_v, a_st);
			}

			void Install(IDirect3DDevice9* a_d)
			{
				installed = true;
				auto** v = *reinterpret_cast<void***>(a_d);
				Hook(v, 34, &StretchRect, oStretchRect);
				Hook(v, kSlotSetRenderTarget, &SetRenderTarget, oSetRenderTargetT);
				Hook(v, 39, &SetDepthStencilSurface, oSetDepthStencilSurface);
				Hook(v, 43, &Clear, oClear);
				Hook(v, kSlotSetTexture, &SetTexture, oSetTextureT);
				Hook(v, 81, &DrawPrimitive, oDrawPrimitive);
				Hook(v, 82, &DrawIndexedPrimitive, oDrawIndexedPrimitive);
				Hook(v, 83, &DrawPrimitiveUP, oDrawPrimitiveUP);
				Hook(v, 84, &DrawIndexedPrimitiveUP, oDrawIndexedPrimitiveUP);
				LC_LOG("DebugFrameTrace: device calls hooked (%zu frames to trace)", times.size());
			}

			void Write()
			{
				wchar_t path[600];
				std::swprintf(path, 600, L"%lslibertycraft-frametrace-%d.txt", log::GameDirW(), ++fileIndex);
				if (FILE* f = ::_wfopen(path, L"wb")) {
					for (const auto& l : lines) {
						std::fputs(l.c_str(), f);
						std::fputc('\n', f);
					}
					std::fclose(f);
				}
				LC_LOG("DebugFrameTrace: frame %d traced, %zu lines in libertycraft-frametrace-%d.txt", fileIndex, lines.size(), fileIndex);
				lines.clear();
				ids.clear();
			}

			// At every drawing command of ours (after the copy's hooks are in, so these wrap them).
			void AtDrawCommand(IDirect3DDevice9* a_d)
			{
				if (!parsed) {
					parsed = true;
					const auto& s = Config::Get().debugFrameTrace;
					for (const char* p = s.c_str(); *p;) {
						char*       end = nullptr;
						const float v = std::strtof(p, &end);
						if (end == p) {
							++p;
							continue;
						}
						times.push_back(v);
						p = end;
					}
				}
				if (times.empty()) {
					return;
				}
				if (!installed) {
					Install(a_d);
				}
				const auto now = ::GetTickCount64();
				if (!t0) {
					t0 = now;
				}
				if (recording) {
					FlushRun();
					Line("==== our draw command (end)");
					recording = false;
					Write();
					return;
				}
				if (nextTime < times.size() && float(now - t0) / 1000.0f >= times[nextTime]) {
					++nextTime;
					recording = true;
					run = Run{};
					Line("==== our draw command (start): scene depth %s; RT0 now %s, DS now %s", Surf(sceneDs).c_str(), Surf(curRt).c_str(), Surf(curDs).c_str());
				}
			}
		}

		// ---- GPU time of the copy and the restore: timestamps read a few frames later (never waits) ----
		namespace gpu
		{
			constexpr int kSlots = 4;
			struct Slot
			{
				IDirect3DQuery9* disjoint = nullptr;
				IDirect3DQuery9* freq = nullptr;
				IDirect3DQuery9* t[4]{};  // copy begin/end, restore begin/end
				bool             open = false, issued = false, copy = false, restore = false;
			};
			Slot slots[kSlots];
			int  cur = -1;
			bool failed = false;

			void Release()
			{
				for (auto& s : slots) {
					SafeRelease(s.disjoint);
					SafeRelease(s.freq);
					for (auto& q : s.t) {
						SafeRelease(q);
					}
					s = Slot{};
				}
				cur = -1;
			}

			void Forget()
			{
				for (auto& s : slots) {
					s = Slot{};
				}
				cur = -1;
			}

			void Close();

			// Collects the oldest slot if its results are in, then opens it for this frame.
			void Open(IDirect3DDevice9* a_d)
			{
				if (failed) {
					return;
				}
				Close();
				const int next = (cur + 1) % kSlots;
				Slot&     s = slots[next];
				if (!s.disjoint) {
					bool ok = SUCCEEDED(a_d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint)) && SUCCEEDED(a_d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq));
					for (auto& q : s.t) {
						ok = ok && SUCCEEDED(a_d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q));
					}
					if (!ok) {
						failed = true;
						Release();
						return;
					}
				}
				if (s.issued) {
					BOOL       disjoint = TRUE;
					UINT64     freq = 0, t[4]{};
					const bool ready = s.disjoint->GetData(&disjoint, sizeof(disjoint), 0) == S_OK && s.freq->GetData(&freq, sizeof(freq), 0) == S_OK &&
					                   (!s.copy || (s.t[0]->GetData(&t[0], sizeof(UINT64), 0) == S_OK && s.t[1]->GetData(&t[1], sizeof(UINT64), 0) == S_OK)) &&
					                   (!s.restore || (s.t[2]->GetData(&t[2], sizeof(UINT64), 0) == S_OK && s.t[3]->GetData(&t[3], sizeof(UINT64), 0) == S_OK));
					if (ready && !disjoint && freq) {
						if (s.copy) {
							stats.copyGpuMs += double(t[1] - t[0]) * 1000.0 / double(freq);
						}
						if (s.restore) {
							stats.restoreGpuMs += double(t[3] - t[2]) * 1000.0 / double(freq);
						}
						++stats.gpuFrames;
					}
				}
				s.issued = true;
				s.open = true;
				s.copy = s.restore = false;
				s.disjoint->Issue(D3DISSUE_BEGIN);
				s.freq->Issue(D3DISSUE_END);
				cur = next;
			}

			void Stamp(int a_i)
			{
				if (failed || cur < 0 || !slots[cur].open) {
					return;
				}
				Slot& s = slots[cur];
				s.t[a_i]->Issue(D3DISSUE_END);
				if (a_i == 1) {
					s.copy = true;
				} else if (a_i == 3) {
					s.restore = true;
				}
			}

			void Close()
			{
				if (!failed && cur >= 0 && slots[cur].open) {
					slots[cur].disjoint->Issue(D3DISSUE_END);
					slots[cur].open = false;
				}
			}
		}

		// ---- resources ------------------------------------------------------------------------------
		void ReleaseDefault()
		{
			SafeRelease(snapSurface);
			SafeRelease(snap);
			snapW = snapH = failedW = failedH = 0;
			copied = latched = false;
			gpu::Release();
		}

		// A new device: its resources went with the old one, drop the pointers.
		void Forget()
		{
			snap = nullptr;
			snapSurface = nullptr;
			snapW = snapH = failedW = failedH = 0;
			vs = nullptr;
			copyPs = restorePs = nullptr;
			decl = nullptr;
			shadersTried = shadersOk = false;
			copied = latched = taken = false;
			gpu::Forget();
		}

		bool Shaders(IDirect3DDevice9* a_d)
		{
			if (shadersTried) {
				return shadersOk;
			}
			shadersTried = true;
			std::vector<DWORD> v, c, r;
			if (!CompileShader(shaders::kDepthCopy, "libertycraft_depthcopy", "VSMain", "vs_3_0", v) ||
				!CompileShader(shaders::kDepthCopy, "libertycraft_depthcopy", "CopyPS", "ps_3_0", c) ||
				!CompileShader(shaders::kDepthCopy, "libertycraft_depthcopy", "RestorePS", "ps_3_0", r)) {
				LC_LOG("behind glass: shaders failed: GTA's glass hides the blocks behind it");
				return false;
			}
			const D3DVERTEXELEMENT9 elements[] = {
				{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
				{ 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
				D3DDECL_END(),
			};
			if (FAILED(a_d->CreateVertexShader(v.data(), &vs)) || FAILED(a_d->CreatePixelShader(c.data(), &copyPs)) ||
				FAILED(a_d->CreatePixelShader(r.data(), &restorePs)) || FAILED(a_d->CreateVertexDeclaration(elements, &decl))) {
				LC_LOG("behind glass: creating the shaders failed: GTA's glass hides the blocks behind it");
				return false;
			}
			shadersOk = true;
			return true;
		}

		bool EnsureTarget(IDirect3DDevice9* a_d, std::uint32_t a_w, std::uint32_t a_h)
		{
			if (snap && snapW == a_w && snapH == a_h) {
				return true;
			}
			if (failedW == a_w && failedH == a_h) {
				return false;
			}
			SafeRelease(snapSurface);
			SafeRelease(snap);
			snapW = snapH = 0;
			HRESULT hr = a_d->CreateTexture(a_w, a_h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &snap, nullptr);
			if (SUCCEEDED(hr)) {
				hr = snap->GetSurfaceLevel(0, &snapSurface);
			}
			if (FAILED(hr)) {
				LC_LOG("behind glass: an R32F target %ux%u failed (0x%08lX): GTA's glass hides the blocks behind it", a_w, a_h, static_cast<unsigned long>(hr));
				SafeRelease(snapSurface);
				SafeRelease(snap);
				failedW = a_w;
				failedH = a_h;
				return false;
			}
			snapW = a_w;
			snapH = a_h;
			return true;
		}

		// A full-screen quad with a_ps reading a_src on s0; the caller binds the targets and sets the depth states.
		void Quad(IDirect3DDevice9* a_d, IDirect3DPixelShader9* a_ps, IDirect3DBaseTexture9* a_src, std::uint32_t a_w, std::uint32_t a_h)
		{
			a_d->SetVertexShader(vs);
			a_d->SetPixelShader(a_ps);
			a_d->SetVertexDeclaration(decl);
			a_d->SetTexture(0, a_src);
			a_d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			a_d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			a_d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			a_d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			a_d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			a_d->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
			a_d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			a_d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			a_d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
			a_d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			a_d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			a_d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
			a_d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
			a_d->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
			a_d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			a_d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
			a_d->SetRenderState(D3DRS_FOGENABLE, FALSE);
			a_d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
			a_d->SetRenderState(D3DRS_DEPTHBIAS, 0);
			a_d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
			a_d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
			a_d->SetRenderState(D3DRS_CLIPPING, TRUE);
			D3DVIEWPORT9 vp{ 0, 0, a_w, a_h, 0.0f, 1.0f };
			a_d->SetViewport(&vp);
			const float tx = 1.0f / float(a_w), ty = 1.0f / float(a_h);
			const float quad[4][6] = {
				{ -1.0f, -1.0f, 0.0f, 1.0f, tx, ty },
				{ -1.0f, 1.0f, 0.0f, 1.0f, tx, ty },
				{ 1.0f, -1.0f, 0.0f, 1.0f, tx, ty },
				{ 1.0f, 1.0f, 0.0f, 1.0f, tx, ty },
			};
			a_d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
			a_d->SetTexture(0, nullptr);
		}

		// GTA's scene depth into `snap`, from inside GTA's frame: its render targets, depth buffer and every
		// other state put back as they were.
		void Copy(IDirect3DDevice9* a_d)
		{
			busy = true;
			taken = true;  // one try per frame, whatever comes of it
			const double t0 = NowMs();
			IDirect3DStateBlock9* sb = nullptr;
			if (!Shaders(a_d) || !EnsureTarget(a_d, sceneW, sceneH) || FAILED(a_d->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) {
				++stats.failed;
				busy = false;
				return;
			}
			IDirect3DSurface9* rt[4]{};
			IDirect3DSurface9* ds = nullptr;
			for (DWORD i = 0; i < 4; ++i) {
				a_d->GetRenderTarget(i, &rt[i]);  // null past the bound ones
			}
			a_d->GetDepthStencilSurface(&ds);
			if (!rt[0]) {
				for (auto& s : rt) {
					SafeRelease(s);
				}
				SafeRelease(ds);
				sb->Release();
				++stats.failed;
				busy = false;
				return;
			}
			if (trace::recording) {
				trace::FlushRun();
				trace::Line("== behind glass: GTA's scene depth copied here (first read of it after the G-buffer pass)");
			}
			gpu::Open(a_d);
			gpu::Stamp(0);
			a_d->SetRenderTarget(0, snapSurface);
			for (DWORD i = 1; i < 4; ++i) {
				if (rt[i]) {
					a_d->SetRenderTarget(i, nullptr);
				}
			}
			a_d->SetDepthStencilSurface(nullptr);  // read, not bound
			a_d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
			a_d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			a_d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			Quad(a_d, copyPs, sceneTex, sceneW, sceneH);
			gpu::Stamp(1);
			a_d->SetRenderTarget(0, rt[0]);
			for (DWORD i = 1; i < 4; ++i) {
				if (rt[i]) {
					a_d->SetRenderTarget(i, rt[i]);
				}
			}
			a_d->SetDepthStencilSurface(ds);
			sb->Apply();  // after the render targets: setting one resets the viewport and scissor rect
			sb->Release();
			for (auto& s : rt) {
				SafeRelease(s);
			}
			SafeRelease(ds);
			copied = true;
			++stats.copies;
			stats.copyMs += NowMs() - t0;
			busy = false;
			if (!loggedFirstCopy) {
				loggedFirstCopy = true;
				LC_LOG("behind glass: GTA's scene depth copied before its transparent pass (%ux%u R32F): its glass no longer hides the blocks and the body", sceneW,
					sceneH);
			}
		}

		// ---- the hooks -----------------------------------------------------------------------------
		HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* a_d, DWORD a_i, IDirect3DSurface9* a_s)
		{
			// A third or fourth render target: GTA's G-buffer pass (it begins a frame's scene).
			if (a_i >= 2 && a_s && !busy && !ours) {
				if (!gbuffer) {
					++stats.gbuffers;
				}
				gbuffer = true;
				taken = copied = false;
			}
			return oSetRenderTarget(a_d, a_i, a_s);
		}

		HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9* a_d, DWORD a_stage, IDirect3DBaseTexture9* a_t)
		{
			++hookCalls;
			// The first read of the scene depth after the G-buffer pass: before the transparent pass.
			if (a_t && a_t == sceneTex && armed && gbuffer && !taken && !busy && !ours) {
				Copy(a_d);
			}
			return oSetTexture(a_d, a_stage, a_t);
		}

		HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* a_d, D3DPRESENT_PARAMETERS* a_pp)
		{
			const bool had = snap != nullptr;
			ReleaseDefault();
			sceneDs = nullptr;  // looked up again at the next draw command (GTA makes its targets anew)
			sceneTex = nullptr;
			const HRESULT hr = oReset(a_d, a_pp);
			if (had) {
				LC_LOG("device Reset: our copy of GTA's depth released first (0x%08lX)", static_cast<unsigned long>(hr));
			}
			return hr;
		}

		void InstallHooks(IDirect3DDevice9* a_d)
		{
			hooksTried = true;
			auto** v = *reinterpret_cast<void***>(a_d);
			if (!Hook(v, kSlotReset, &HookReset, oReset)) {
				LC_LOG("behind glass: can't hook IDirect3DDevice9::Reset (VirtualProtect error %lu): GTA's glass hides the blocks behind it", ::GetLastError());
				return;
			}
			if (!Hook(v, kSlotSetRenderTarget, &HookSetRenderTarget, oSetRenderTarget) || !Hook(v, kSlotSetTexture, &HookSetTexture, oSetTexture)) {
				LC_LOG("behind glass: can't hook SetRenderTarget/SetTexture (VirtualProtect error %lu): GTA's glass hides the blocks behind it", ::GetLastError());
				return;
			}
			// The slot must be SetTexture: our own call has to come through.
			IDirect3DBaseTexture9* t = nullptr;
			a_d->GetTexture(0, &t);
			const auto before = hookCalls;
			a_d->SetTexture(0, t);
			if (t) {
				t->Release();
			}
			if (hookCalls == before) {
				Patch(v + kSlotSetTexture, reinterpret_cast<void*>(oSetTexture));
				Patch(v + kSlotSetRenderTarget, reinterpret_cast<void*>(oSetRenderTarget));
				LC_LOG("behind glass: vtable slot %u isn't SetTexture: unhooked; GTA's glass hides the blocks behind it", kSlotSetTexture);
				return;
			}
			hooksOk = true;
			LC_LOG("behind glass: watching SetRenderTarget and SetTexture (device %p) for GTA's scene depth before its transparent pass", static_cast<void*>(a_d));
		}

		void LogStats()
		{
			const auto now = ::GetTickCount64();
			if (now < nextStatsLog) {
				return;
			}
			const bool first = nextStatsLog == 0;
			nextStatsLog = now + 10000;
			if (first || !stats.wanted) {
				stats = Stats{};
				return;
			}
			const double n = std::max(1u, stats.copies), r = std::max(1u, stats.restored), g = std::max(1u, stats.gpuFrames);
			LC_LOG("behind glass in %u of %u drawn frames (%u wanted it; GTA's G-buffer pass seen in %u, copy failed in %u): copy %.3f ms CPU %.3f ms GPU, "
				   "restore %.3f ms CPU %.3f ms GPU (GPU times from %u frames)",
				stats.restored, stats.drawn, stats.wanted, stats.gbuffers, stats.failed, stats.copyMs / n, stats.copyGpuMs / g, stats.restoreMs / r,
				stats.restoreGpuMs / g, stats.gpuFrames);
			stats = Stats{};
		}
	}

	void AtDrawCommand(IDirect3DDevice9* a_d, IDirect3DSurface9* a_scene, bool a_drawn, bool a_want)
	{
		const auto& cfg = Config::Get();
		if (device != a_d) {
			if (device) {
				Forget();
			}
			device = a_d;
		}
		if (!hooksTried && cfg.renderBehindGlass) {
			InstallHooks(a_d);
		}
		// GTA's scene depth texture, as bound at the drawing command.
		if (a_drawn && a_scene && a_scene != sceneDs) {
			sceneDs = a_scene;
			sceneTex = nullptr;
			sceneW = sceneH = 0;
			IDirect3DTexture9* tex = nullptr;
			D3DSURFACE_DESC    d{};
			if (SUCCEEDED(a_scene->GetDesc(&d)) && d.MultiSampleType == D3DMULTISAMPLE_NONE &&
				SUCCEEDED(a_scene->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) && tex) {
				sceneTex = tex;  // compared only; GTA keeps it alive
				sceneW = d.Width;
				sceneH = d.Height;
				tex->Release();
			}
			char f[16];
			LC_LOG_EVERY(10000, "behind glass: GTA's scene depth %ux%u %s %s", d.Width, d.Height, Fmt(d.Format, f),
				sceneTex ? "is a texture: copied before GTA's transparent pass" : "isn't a texture: GTA's glass hides the blocks behind it");
		}
		if (a_drawn) {
			trace::AtDrawCommand(a_d);
		}
		// This frame is over: its copy goes to the drawing command, the next frame starts afresh.
		latched = a_drawn && copied;
		taken = copied = gbuffer = false;
		if (a_drawn) {
			bool want = a_want && hooksOk && sceneTex;
			if (cfg.debugBehindGlassAB > 0.0f) {
				const auto period = std::uint64_t(std::max(1.0f, cfg.debugBehindGlassAB) * 1000.0f);
				const bool on = (::GetTickCount64() / period) % 2 == 1;
				want = want && on;
				if (int(on) != lastAB) {
					lastAB = int(on);
					LC_LOG("DebugBehindGlassAB: GTA's glass %s", on ? "shows the blocks behind it" : "hides the blocks behind it");
				}
			}
			latched = latched && want;
			armed = want;
			++stats.drawn;
			stats.wanted += want ? 1 : 0;
		}
		if (!latched) {
			gpu::Close();  // a copy no restore follows
		}
		if (a_drawn) {
			LogStats();
		}
	}

	bool Restore(IDirect3DDevice9* a_d, std::uint32_t a_w, std::uint32_t a_h)
	{
		if (!latched) {
			return false;
		}
		latched = false;
		IDirect3DSurface9* ds = nullptr;
		const bool         same = SUCCEEDED(a_d->GetDepthStencilSurface(&ds)) && ds == sceneDs;
		SafeRelease(ds);
		if (!same || !snap || a_w != snapW || a_h != snapH || !shadersOk) {
			gpu::Close();
			return false;
		}
		const double t0 = NowMs();
		gpu::Stamp(2);
		a_d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		a_d->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER);
		a_d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		a_d->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
		Quad(a_d, restorePs, snap, a_w, a_h);
		a_d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		a_d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
		gpu::Stamp(3);
		gpu::Close();
		++stats.restored;
		stats.restoreMs += NowMs() - t0;
		if (!loggedFirstRestore) {
			loggedFirstRestore = true;
			LC_LOG("behind glass: the blocks are drawn against GTA's depth from before its transparent pass");
		}
		return true;
	}

	void SetOurs(bool a_ours)
	{
		ours = a_ours;
	}
}
