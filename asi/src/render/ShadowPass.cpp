// See ShadowPass.h. SDK-free: the device and the frame come from Render.cpp / World.cpp.
#define LC_MODULE "render"
#include "render/ShadowPass.h"

#include "render/D3D9Util.h"
#include "render/Shaders.h"

#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace lc::render::shadowpass
{
	namespace
	{
		IDirect3DDevice9*            device = nullptr;
		bool                         shadersTried = false, shadersOk = false;
		IDirect3DVertexShader9*      casterVs = nullptr;
		IDirect3DPixelShader9*       casterPs = nullptr;
		IDirect3DVertexShader9*      darkenVs = nullptr;
		IDirect3DPixelShader9*       darkenPs = nullptr;
		IDirect3DVertexDeclaration9* darkenDecl = nullptr;

		// Our atlas (D3DPOOL_DEFAULT: released before the device's Reset).
		IDirect3DTexture9*  atlas = nullptr;
		IDirect3DSurface9*  atlasSurface = nullptr;
		IDirect3DSurface9*  atlasDepth = nullptr;
		std::uint32_t       atlasW = 0, atlasH = 0;
		std::uint32_t       failedW = 0, failedH = 0;  // creation failed at this size: don't retry every frame
		bool                filled = false;            // the atlas holds this frame's blocks

		// GTA's targets while ours is bound.
		IDirect3DSurface9* savedRt[4]{};
		IDirect3DSurface9* savedDs = nullptr;
		double             casterT0 = 0.0;

		float ndlRemap[2] = { 1.0f, 0.0f };
		bool  loggedNoDepthTexture = false;
		bool loggedDepth = false;

		ShadowStats stats;

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

		// ---- GPU timing: timestamps around the caster and darkening passes, read a few frames later --
		namespace gpu
		{
			constexpr int kSlots = 4;
			struct Slot
			{
				IDirect3DQuery9* disjoint = nullptr;
				IDirect3DQuery9* freq = nullptr;
				IDirect3DQuery9* t[4]{};  // caster begin/end, darken begin/end
				bool             issued = false, casters = false, darken = false;
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
					s.issued = s.casters = s.darken = false;
				}
				cur = -1;
			}

			// Collects the oldest slot if its results are in (never waits), then starts a new frame.
			void BeginFrame(IDirect3DDevice9* a_d)
			{
				if (failed) {
					return;
				}
				const int next = (cur + 1) % kSlots;
				Slot&     s = slots[next];
				if (!s.disjoint) {
					if (FAILED(a_d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint)) || FAILED(a_d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq))) {
						failed = true;
						Release();
						return;
					}
					for (auto& q : s.t) {
						if (FAILED(a_d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q))) {
							failed = true;
							Release();
							return;
						}
					}
				}
				if (s.issued) {
					BOOL       disjoint = TRUE;
					UINT64     freq = 0, t[4]{};
					const bool ready = s.disjoint->GetData(&disjoint, sizeof(disjoint), 0) == S_OK && s.freq->GetData(&freq, sizeof(freq), 0) == S_OK &&
					                   (!s.casters || (s.t[0]->GetData(&t[0], sizeof(UINT64), 0) == S_OK && s.t[1]->GetData(&t[1], sizeof(UINT64), 0) == S_OK)) &&
					                   (!s.darken || (s.t[2]->GetData(&t[2], sizeof(UINT64), 0) == S_OK && s.t[3]->GetData(&t[3], sizeof(UINT64), 0) == S_OK));
					if (ready && !disjoint && freq) {
						if (s.casters) {
							stats.casterGpuMs += double(t[1] - t[0]) * 1000.0 / double(freq);
						}
						if (s.darken) {
							stats.darkenGpuMs += double(t[3] - t[2]) * 1000.0 / double(freq);
						}
						++stats.gpuFrames;
					}
				}
				s.issued = true;
				s.casters = s.darken = false;
				s.disjoint->Issue(D3DISSUE_BEGIN);
				s.freq->Issue(D3DISSUE_END);
				cur = next;
			}

			void Stamp(int a_i)
			{
				if (failed || cur < 0) {
					return;
				}
				Slot& s = slots[cur];
				s.t[a_i]->Issue(D3DISSUE_END);
				if (a_i == 1) {
					s.casters = true;
				} else if (a_i == 3) {
					s.darken = true;
				}
			}

			void EndFrame()
			{
				if (!failed && cur >= 0 && slots[cur].disjoint) {
					slots[cur].disjoint->Issue(D3DISSUE_END);
				}
			}
		}

		void ReleaseDefault()
		{
			SafeRelease(atlasSurface);
			SafeRelease(atlasDepth);
			SafeRelease(atlas);
			atlasW = atlasH = 0;
			failedW = failedH = 0;
			filled = false;
			gpu::Release();
		}

		// ---- IDirect3DDevice9::Reset (vtable slot 16): our D3DPOOL_DEFAULT resources go first ------
		using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
		ResetFn originalReset = nullptr;
		bool    resetHookTried = false;

		HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* a_d, D3DPRESENT_PARAMETERS* a_pp)
		{
			const bool had = atlas != nullptr;
			ReleaseDefault();
			const HRESULT hr = originalReset(a_d, a_pp);
			LC_LOG("device Reset (%s our shadow atlas): 0x%08lX", had ? "released" : "no", static_cast<unsigned long>(hr));
			return hr;
		}

		bool InstallResetHook(IDirect3DDevice9* a_d)
		{
			if (resetHookTried) {
				return originalReset != nullptr;
			}
			resetHookTried = true;
			auto** vtbl = *reinterpret_cast<void***>(a_d);
			void** slot = vtbl + 16;
			DWORD  old = 0;
			if (!::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
				LC_LOG("WARNING: can't hook IDirect3DDevice9::Reset (VirtualProtect error %lu): the blocks cast no shadows", ::GetLastError());
				return false;
			}
			originalReset = reinterpret_cast<ResetFn>(*slot);
			*slot = reinterpret_cast<void*>(&HookReset);
			::VirtualProtect(slot, sizeof(void*), old, &old);
			::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
			LC_LOG("watching IDirect3DDevice9::Reset (our shadow atlas is released before it)");
			return true;
		}

		bool InitShaders(IDirect3DDevice9* a_d)
		{
			if (shadersTried) {
				return shadersOk;
			}
			shadersTried = true;
			std::vector<DWORD> cvs, cps, dvs, dps;
			if (!CompileShader(shaders::kCaster, "libertycraft_caster", "VSMain", "vs_3_0", cvs) ||
				!CompileShader(shaders::kCaster, "libertycraft_caster", "PSMain", "ps_3_0", cps) ||
				!CompileShader(shaders::kDarken, "libertycraft_darken", "VSMain", "vs_3_0", dvs) ||
				!CompileShader(shaders::kDarken, "libertycraft_darken", "PSMain", "ps_3_0", dps)) {
				return false;
			}
			if (FAILED(a_d->CreateVertexShader(cvs.data(), &casterVs)) || FAILED(a_d->CreatePixelShader(cps.data(), &casterPs)) ||
				FAILED(a_d->CreateVertexShader(dvs.data(), &darkenVs)) || FAILED(a_d->CreatePixelShader(dps.data(), &darkenPs))) {
				LC_LOG("ERROR: creating the shadow shaders failed");
				return false;
			}
			const D3DVERTEXELEMENT9 elements[] = {
				{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
				D3DDECL_END(),
			};
			if (FAILED(a_d->CreateVertexDeclaration(elements, &darkenDecl))) {
				LC_LOG("ERROR: the shadow pass's vertex declaration failed");
				return false;
			}
			shadersOk = true;
			LC_LOG("shadow shaders ready (casters, GTA's world in the blocks' shadow)");
			return true;
		}

		bool EnsureAtlas(IDirect3DDevice9* a_d, std::uint32_t a_w, std::uint32_t a_h)
		{
			if (atlas && atlasW == a_w && atlasH == a_h) {
				return true;
			}
			if (failedW == a_w && failedH == a_h) {
				return false;
			}
			SafeRelease(atlasSurface);
			SafeRelease(atlasDepth);
			SafeRelease(atlas);
			atlasW = atlasH = 0;
			HRESULT hr = a_d->CreateTexture(a_w, a_h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &atlas, nullptr);
			if (SUCCEEDED(hr)) {
				hr = atlas->GetSurfaceLevel(0, &atlasSurface);
			}
			if (SUCCEEDED(hr)) {
				hr = a_d->CreateDepthStencilSurface(a_w, a_h, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &atlasDepth, nullptr);
			}
			if (FAILED(hr)) {
				LC_LOG("ERROR: our shadow atlas %ux%u (R32F + D24X8) failed: 0x%08lX; the blocks cast no shadows", a_w, a_h, static_cast<unsigned long>(hr));
				SafeRelease(atlasSurface);
				SafeRelease(atlasDepth);
				SafeRelease(atlas);
				failedW = a_w;
				failedH = a_h;
				return false;
			}
			atlasW = a_w;
			atlasH = a_h;
			LC_LOG("our shadow atlas: %ux%u R32F (+ D24X8), GTA's cascade layout", a_w, a_h);
			return true;
		}

		void SetPointClamp(IDirect3DDevice9* a_d, DWORD a_s)
		{
			a_d->SetSamplerState(a_s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			a_d->SetSamplerState(a_s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			a_d->SetSamplerState(a_s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			a_d->SetSamplerState(a_s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			a_d->SetSamplerState(a_s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			a_d->SetSamplerState(a_s, D3DSAMP_MAXMIPLEVEL, 0);
			a_d->SetSamplerState(a_s, D3DSAMP_SRGBTEXTURE, FALSE);
		}

		// Render states shared by both passes (the caller's state block restores everything).
		void CommonStates(IDirect3DDevice9* a_d)
		{
			a_d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			a_d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			a_d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
			a_d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
			a_d->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
			a_d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
			a_d->SetRenderState(D3DRS_FOGENABLE, FALSE);
			a_d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
			a_d->SetRenderState(D3DRS_DEPTHBIAS, 0);
			a_d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
			a_d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
			a_d->SetRenderState(D3DRS_CLIPPING, TRUE);
			a_d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		}
	}

	bool Prepare(IDirect3DDevice9* a_d, const ShadowFrame& a_s)
	{
		if (device != a_d) {
			if (device) {
				Forget();
			}
			device = a_d;
		}
		filled = false;
		gpu::BeginFrame(a_d);
		if (!a_s.cast || !a_s.atlasW || !a_s.atlasH || !InstallResetHook(a_d) || !InitShaders(a_d)) {
			return false;
		}
		return EnsureAtlas(a_d, a_s.atlasW, a_s.atlasH);
	}

	bool BeginCasters(IDirect3DDevice9* a_d, const ShadowFrame& a_s)
	{
		if (!atlas || !shadersOk || atlasW != a_s.atlasW || atlasH != a_s.atlasH) {
			return false;
		}
		casterT0 = NowMs();
		gpu::Stamp(0);
		for (DWORD i = 0; i < 4; ++i) {
			savedRt[i] = nullptr;
			a_d->GetRenderTarget(i, &savedRt[i]);  // fails (null) past the first unbound one
		}
		savedDs = nullptr;
		a_d->GetDepthStencilSurface(&savedDs);
		for (DWORD i = 1; i < 4; ++i) {
			if (savedRt[i]) {
				a_d->SetRenderTarget(i, nullptr);
			}
		}
		if (FAILED(a_d->SetRenderTarget(0, atlasSurface)) || FAILED(a_d->SetDepthStencilSurface(atlasDepth))) {
			EndCasters(a_d, 0);
			return false;
		}
		D3DVIEWPORT9 vp{ 0, 0, atlasW, atlasH, 0.0f, 1.0f };
		a_d->SetViewport(&vp);
		a_d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);  // R32F 1.0: nothing of ours
		a_d->SetVertexShader(casterVs);
		a_d->SetPixelShader(casterPs);
		CommonStates(a_d);
		a_d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		a_d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		a_d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		a_d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		a_d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
		a_d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
		a_d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		a_d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		a_d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		a_d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		a_d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		a_d->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
		a_d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
		const float params[4] = { a_s.casterBias, 0.0f, 0.0f, 0.0f };
		a_d->SetVertexShaderConstantF(3, params, 1);
		return true;
	}

	void Cascade(IDirect3DDevice9* a_d, const ShadowFrame& a_s, int a_k)
	{
		float rows[3][4];
		CasterRows(a_s.params, a_k, atlasW, atlasH, rows);
		a_d->SetVertexShaderConstantF(0, &rows[0][0], 3);
		float lo, hi;
		CascadeU(a_k, lo, hi);
		RECT r{ LONG(std::lround(lo * float(atlasW))), 0, LONG(std::lround(hi * float(atlasW))), LONG(atlasH) };
		a_d->SetScissorRect(&r);
	}

	void EndCasters(IDirect3DDevice9* a_d, std::uint32_t a_draws)
	{
		a_d->SetRenderTarget(0, savedRt[0]);
		for (DWORD i = 1; i < 4; ++i) {
			if (savedRt[i]) {
				a_d->SetRenderTarget(i, savedRt[i]);
			}
		}
		a_d->SetDepthStencilSurface(savedDs);
		for (auto& s : savedRt) {
			SafeRelease(s);
		}
		SafeRelease(savedDs);
		a_d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		gpu::Stamp(1);
		filled = a_draws > 0;
		stats.casterDraws += a_draws;
		stats.casterCpuMs += NowMs() - casterT0;
	}

	void DumpAtlases(IDirect3DDevice9* a_d, const ShadowFrame& a_s)
	{
		auto dump = [&](IDirect3DBaseTexture9* a_tex, const char* a_name) {
			if (!a_tex || a_tex->GetType() != D3DRTYPE_TEXTURE) {
				LC_LOG("shadow dump %s: no texture", a_name);
				return;
			}
			IDirect3DSurface9* src = nullptr;
			IDirect3DSurface9* sys = nullptr;
			D3DSURFACE_DESC    d{};
			if (FAILED(static_cast<IDirect3DTexture9*>(a_tex)->GetSurfaceLevel(0, &src)) || !src || FAILED(src->GetDesc(&d)) || d.Format != D3DFMT_R32F ||
				FAILED(a_d->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || FAILED(a_d->GetRenderTargetData(src, sys))) {
				LC_LOG("shadow dump %s: can't read it back", a_name);
				SafeRelease(sys);
				SafeRelease(src);
				return;
			}
			D3DLOCKED_RECT lr{};
			if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
				const std::uint32_t w4 = d.Width / 4, h4 = d.Height / 4;
				std::vector<std::uint8_t> pgm(std::size_t(w4) * h4);
				std::uint32_t             used[4]{};
				float                     lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
				for (std::uint32_t y = 0; y < d.Height; ++y) {
					const auto* row = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(lr.pBits) + std::size_t(y) * lr.Pitch);
					for (std::uint32_t x = 0; x < d.Width; ++x) {
						const float v = row[x];
						const int   q = int(std::min<std::uint32_t>(3, x * 4 / d.Width));
						if (v < 0.9999f) {
							++used[q];
							lo[q] = std::min(lo[q], v);
							hi[q] = std::max(hi[q], v);
						}
						// 0.49 .. 0.51 (sc.z -20 .. +20 m around the camera's plane) as black .. white; nothing: white
						const float g = v >= 0.9999f ? 1.0f : std::clamp((v - 0.49f) / 0.02f, 0.0f, 0.98f);
						auto&       px = pgm[std::size_t(y / 4) * w4 + x / 4];
						const auto  b = std::uint8_t(g * 255.0f);
						if ((y % 4 == 0 && x % 4 == 0) || b < px) {
							px = b;
						}
					}
				}
				sys->UnlockRect();
				wchar_t path[600];
				std::swprintf(path, 600, L"%lslibertycraft-shadow-%hs.pgm", log::GameDirW(), a_name);
				if (FILE* f = ::_wfopen(path, L"wb")) {
					std::fprintf(f, "P5\n%u %u\n255\n", w4, h4);
					std::fwrite(pgm.data(), 1, pgm.size(), f);
					std::fclose(f);
				}
				LC_LOG("shadow dump %s %ux%u: texels in use per cascade %u %u %u %u; values %.5f to %.5f | %.5f to %.5f | %.5f to %.5f | %.5f to %.5f", a_name,
					d.Width, d.Height, used[0], used[1], used[2], used[3], lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
			}
			SafeRelease(sys);
			SafeRelease(src);
		};
		dump(a_s.gtaAtlas, "gta");
		dump(filled ? atlas : nullptr, "ours");
	}

	IDirect3DBaseTexture9* OurAtlas()
	{
		return filled ? atlas : nullptr;
	}

	void BindLookup(IDirect3DDevice9* a_d, const ShadowFrame* a_s)
	{
		ShadowParams p;
		if (a_s && a_s->gtaAtlas && a_s->params.fwd[3] > 0.5f) {
			p = a_s->params;
			p.chss[2] = filled ? 1.0f : 0.0f;
			p.chss[3] = a_s->view ? 1.0f : 0.0f;
			a_d->SetTexture(2, a_s->gtaAtlas);
			a_d->SetTexture(3, filled ? static_cast<IDirect3DBaseTexture9*>(atlas) : a_s->gtaAtlas);
		} else {
			p.fwd[3] = 0.0f;
			p.ndl[0] = ndlRemap[0];
			p.ndl[1] = ndlRemap[1];
			a_d->SetTexture(2, nullptr);
			a_d->SetTexture(3, nullptr);
		}
		SetPointClamp(a_d, 2);
		SetPointClamp(a_d, 3);
		a_d->SetPixelShaderConstantF(14, p.fwd, 14);
	}

	void SetNdlRemap(float a_scale, float a_offset)
	{
		ndlRemap[0] = a_scale;
		ndlRemap[1] = a_offset;
	}

	void Darken(IDirect3DDevice9* a_d, const FrameSnapshot& a_f, const LightingParams& a_light, const ShadowFrame& a_s, std::uint32_t a_w, std::uint32_t a_h)
	{
		if (!filled || !shadersOk || !a_s.gtaAtlas || a_s.strength <= 0.0f || a_light.sunDir[3] < 0.5f || !a_w || !a_h) {
			return;
		}
		const double t0 = NowMs();
		// GTA's depth buffer as a texture: the INTZ texture behind the bound depth-stencil surface.
		IDirect3DSurface9* ds = nullptr;
		if (FAILED(a_d->GetDepthStencilSurface(&ds)) || !ds) {
			return;
		}
		IDirect3DTexture9* depthTex = nullptr;
		D3DSURFACE_DESC    dd{};
		ds->GetDesc(&dd);
		if (FAILED(ds->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&depthTex))) || !depthTex) {
			if (!loggedNoDepthTexture) {
				loggedNoDepthTexture = true;
				LC_LOG("WARNING: GTA's depth buffer isn't a texture (format %u): GTA's world gets no shadows from the blocks", static_cast<unsigned>(dd.Format));
			}
			ds->Release();
			return;
		}
		if (!loggedDepth) {
			loggedDepth = true;
			LC_LOG("GTA's world in the blocks' shadow: reading its depth buffer (%ux%u, format 0x%08X) as a texture", dd.Width, dd.Height,
				static_cast<unsigned>(dd.Format));
		}
		float ray[3][4];
		if (!RayBasis(a_f.clip, ray)) {
			depthTex->Release();
			ds->Release();
			return;
		}
		gpu::Stamp(2);
		a_d->SetDepthStencilSurface(nullptr);  // read, not bound
		a_d->SetVertexShader(darkenVs);
		a_d->SetPixelShader(darkenPs);
		a_d->SetVertexDeclaration(darkenDecl);
		CommonStates(a_d);
		a_d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		a_d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		a_d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		a_d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		a_d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
		a_d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
		a_d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		a_d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
		D3DVIEWPORT9 vp{ 0, 0, a_w, a_h, 0.0f, 1.0f };
		a_d->SetViewport(&vp);
		a_d->SetTexture(0, depthTex);
		SetPointClamp(a_d, 0);
		a_d->SetTexture(1, a_s.gbuffer2);
		SetPointClamp(a_d, 1);
		BindLookup(a_d, &a_s);
		static_assert(sizeof(LightingParams) == 13 * 16);
		a_d->SetPixelShaderConstantF(1, a_light.sunDir, 13);
		const float n = std::max(a_f.nearZ, 1e-3f), f = std::max(a_f.farZ, n * 2.0f);
		float       c[5][4] = {
            { ray[0][0], ray[0][1], ray[0][2], 0.0f },
            { ray[1][0], ray[1][1], ray[1][2], 0.0f },
            { ray[2][0], ray[2][1], ray[2][2], 0.0f },
            { n, std::log2(f / n), (a_f.flags & kFrameLogDepth) ? 1.0f : 0.0f, f },
            { 1.0f / float(a_w), 1.0f / float(a_h), std::clamp(a_s.strength, 0.0f, 1.0f), a_s.gbuffer2 ? 1.0f : 0.0f },
		};
		a_d->SetPixelShaderConstantF(28, &c[0][0], 5);
		const float quad[4][4] = { { -1.0f, -1.0f, 0.0f, 1.0f }, { -1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 0.0f, 1.0f } };
		a_d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		a_d->SetTexture(0, nullptr);
		a_d->SetTexture(1, nullptr);
		a_d->SetDepthStencilSurface(ds);
		depthTex->Release();
		ds->Release();
		gpu::Stamp(3);
		++stats.darkenFrames;
		stats.darkenCpuMs += NowMs() - t0;
	}

	void Forget()
	{
		// The old device's resources went with it: drop the pointers, don't release them.
		casterVs = nullptr;
		casterPs = nullptr;
		darkenVs = nullptr;
		darkenPs = nullptr;
		darkenDecl = nullptr;
		shadersTried = shadersOk = false;
		atlas = nullptr;
		atlasSurface = nullptr;
		atlasDepth = nullptr;
		atlasW = atlasH = failedW = failedH = 0;
		filled = false;
		for (auto& s : gpu::slots) {
			s = gpu::Slot{};
		}
		gpu::cur = -1;
		// The Reset hook sits in the device's vtable: a new device of the same kind shares it.
	}

	void FrameDone()
	{
		gpu::EndFrame();
		++stats.frames;
	}

	ShadowStats TakeStats()
	{
		ShadowStats out = stats;
		stats = ShadowStats{};
		return out;
	}
}
