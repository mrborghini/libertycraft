// Unity-built into dllmain.cpp. See Overlay.h. (Needs no IV-SDK itself.)
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "overlay"
#include "Overlay.h"

#include "render/D3D9Util.h"
#include "render/RenderMath.h"
#include "render/Shaders.h"

#include "Link.h"
#include "Log.h"

#include <algorithm>
#include <utility>

namespace lc::Overlay
{
	namespace
	{
		struct QuadVertex
		{
			float x, y, z, w;
			float u, v;
		};

		IDirect3DDevice9*            device = nullptr;
		bool                         initTried = false, ready = false;
		IDirect3DVertexShader9*      vs = nullptr;
		IDirect3DPixelShader9*       ps = nullptr;
		IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DTexture9*           texture = nullptr;  // Minecraft's RGBA bytes in an ARGB texture (the shader swizzles)
		std::uint32_t                texW = 0, texH = 0;
		bool                         flipY = false;
		bool                         haveFrame = false;    // texture holds a frame
		bool                         stale = true;         // a newer frame was acquired but not copied
		std::uint32_t                frames = 0, uploads = 0;
		double                       uploadMs = 0.0;
		bool                         loggedFirst = false;

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

		bool Init(IDirect3DDevice9* a_device)
		{
			if (device != a_device) {
				// A new device: the old one's objects went with it.
				vs = nullptr;
				ps = nullptr;
				decl = nullptr;
				texture = nullptr;
				texW = texH = 0;
				haveFrame = false;
				ready = initTried = false;
				device = a_device;
			}
			if (ready || initTried) {
				return ready;
			}
			initTried = true;
			std::vector<DWORD> vsCode, psCode;
			if (!render::CompileShader(render::shaders::kOverlay, "libertycraft_overlay", "VSMain", "vs_3_0", vsCode) ||
				!render::CompileShader(render::shaders::kOverlay, "libertycraft_overlay", "PSMain", "ps_3_0", psCode)) {
				return false;
			}
			const D3DVERTEXELEMENT9 elements[] = {
				{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
				{ 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
				D3DDECL_END(),
			};
			if (FAILED(a_device->CreateVertexShader(vsCode.data(), &vs)) || FAILED(a_device->CreatePixelShader(psCode.data(), &ps)) ||
				FAILED(a_device->CreateVertexDeclaration(elements, &decl))) {
				LC_LOG("ERROR: overlay shaders/declaration failed");
				return false;
			}
			ready = true;
			LC_LOG("overlay renderer ready");
			return true;
		}

		bool EnsureTexture(std::uint32_t a_w, std::uint32_t a_h)
		{
			if (texture && texW == a_w && texH == a_h) {
				return true;
			}
			render::SafeRelease(texture);
			texW = texH = 0;
			haveFrame = false;
			if (FAILED(device->CreateTexture(a_w, a_h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
				LC_LOG("ERROR: overlay texture %ux%u failed", a_w, a_h);
				return false;
			}
			texW = a_w;
			texH = a_h;
			LC_LOG("overlay texture %ux%u", a_w, a_h);
			return true;
		}

		// Copies the current front slot into the texture.
		void CopyFront()
		{
			const double t0 = NowMs();
			Link::Get().WithFrontSlot([&](const proto::OverlaySlotHdr* a_hdr, const std::uint8_t* a_pixels) {
				if (!a_hdr->width || !a_hdr->height || a_hdr->width > proto::kMaxOverlayW || a_hdr->height > proto::kMaxOverlayH) {
					return;
				}
				if (!loggedFirst) {
					loggedFirst = true;
					LC_LOG("first overlay frame: %ux%u flags 0x%X frame %llu", a_hdr->width, a_hdr->height, a_hdr->flags,
						static_cast<unsigned long long>(a_hdr->frameId));
				}
				if (!EnsureTexture(a_hdr->width, a_hdr->height)) {
					return;
				}
				D3DLOCKED_RECT lr{};
				if (FAILED(texture->LockRect(0, &lr, nullptr, 0))) {
					return;
				}
				render::CopyRows(lr.pBits, lr.Pitch, a_pixels, a_hdr->width * 4, a_hdr->height);
				texture->UnlockRect(0);
				flipY = (a_hdr->flags & 1) != 0;
				haveFrame = true;
				stale = false;
				++uploads;
			});
			uploadMs += NowMs() - t0;
		}
	}

	void Upload(IDirect3DDevice9* a_device, bool a_wanted)
	{
		auto& link = Link::Get();
		if (link.AcquireOverlayFrame()) {
			++frames;
			stale = true;
		}
		if (!a_wanted || !stale || !a_device || !Init(a_device)) {
			return;
		}
		CopyFront();
	}

	void Draw(IDirect3DDevice9* a_device, const render::FrameSnapshot& a_f, std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!ready || a_device != device || !haveFrame || !texture || !a_width || !a_height) {
			return;
		}
		auto*       d = a_device;
		const float w = float(a_width), h = float(a_height);
		const float hx = 1.0f / w, hy = 1.0f / h;  // half a pixel in clip units (D3D9 pixel centres)
		const QuadVertex quad[4] = {
			{ -1.0f - hx, 1.0f + hy, 0.0f, 1.0f, 0.0f, 0.0f },
			{ 1.0f - hx, 1.0f + hy, 0.0f, 1.0f, 1.0f, 0.0f },
			{ -1.0f - hx, -1.0f + hy, 0.0f, 1.0f, 0.0f, 1.0f },
			{ 1.0f - hx, -1.0f + hy, 0.0f, 1.0f, 1.0f, 1.0f },
		};
		// The cursor and the GUI scale are in overlay pixels: scale to the render target.
		const float sx = w / float(texW), sy = h / float(texH);
		const bool  cursorOn = (a_f.flags & render::kFrameScreenOpen) != 0;
		const float c0[4] = { float(a_f.cursorX) * sx, float(a_f.cursorY) * sy, cursorOn ? 1.0f : 0.0f, flipY ? 1.0f : 0.0f };
		float       rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const bool  invert = (a_f.flags & render::kFrameCrosshair) && a_f.guiScale > 0;
		if (invert) {
			render::CrosshairRect(static_cast<int>(a_f.guiScale), sx, w, h, rect);
		}
		const float mainPass[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const float invertPass[4] = { 1.0f, 0.0f, 0.0f, 0.0f };

		D3DVIEWPORT9 vp{ 0, 0, a_width, a_height, 0.0f, 1.0f };
		d->SetViewport(&vp);
		d->SetVertexShader(vs);
		d->SetPixelShader(ps);
		d->SetVertexDeclaration(decl);
		d->SetPixelShaderConstantF(0, c0, 1);
		d->SetPixelShaderConstantF(1, rect, 1);
		d->SetPixelShaderConstantF(2, mainPass, 1);
		d->SetTexture(0, texture);
		const bool exact = texW == a_width && texH == a_height;
		d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		d->SetSamplerState(0, D3DSAMP_MAGFILTER, exact ? D3DTEXF_POINT : D3DTEXF_LINEAR);
		d->SetSamplerState(0, D3DSAMP_MINFILTER, exact ? D3DTEXF_POINT : D3DTEXF_LINEAR);
		d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
		d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
		d->SetRenderState(D3DRS_FOGENABLE, FALSE);
		d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);  // premultiplied alpha, straight from Minecraft
		d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
		if (invert) {
			// Minecraft's crosshair and attack indicator: its invert blend against GTA's picture.
			d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
			d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCCOLOR);
			d->SetPixelShaderConstantF(2, invertPass, 1);
			d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
		}
	}

	std::uint32_t TakeFrames()
	{
		return std::exchange(frames, 0u);
	}

	std::uint32_t TakeUploads()
	{
		return std::exchange(uploads, 0u);
	}

	double TakeUploadMs()
	{
		return std::exchange(uploadMs, 0.0);
	}
}
