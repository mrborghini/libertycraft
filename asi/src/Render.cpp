// Unity-built into dllmain.cpp (needs IV-SDK). See Render.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "render"
#include "Render.h"

#include "Game.h"
#include "Link.h"
#include "Log.h"
#include "Overlay.h"
#include "Perf.h"

#include <atomic>
#include <cstdio>

namespace lc::Render
{
	namespace
	{
		constexpr int  kTypes = 12;  // RenType values 0..11
		std::uint64_t  bytesByType[kTypes]{};
		std::uint32_t  msgsByType[kTypes]{};
		std::uint32_t  otherMsgs = 0;
		std::uint64_t  drainedBytes = 0;
		std::uint32_t  calls = 0;
		std::uint64_t  lastLogMs = 0;
		std::uint32_t  sizeCheck = 0;
		bool           loggedViewport = false;

		// The back buffer is what Minecraft's overlay and cursor coordinates refer to.
		void CaptureViewport()
		{
			if (sizeCheck++ % 120 != 0) {
				return;
			}
			IDirect3DDevice9* device = rage::g_pDirect3DDevice;
			if (!device) {
				return;
			}
			IDirect3DSurface9* surface = nullptr;
			if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) || !surface) {
				return;
			}
			D3DSURFACE_DESC desc{};
			if (SUCCEEDED(surface->GetDesc(&desc)) && desc.Width && desc.Height) {
				auto& st = Game::State();
				if (st.viewportW != static_cast<int>(desc.Width) || st.viewportH != static_cast<int>(desc.Height) || !loggedViewport) {
					LC_LOG("back buffer %ux%u", desc.Width, desc.Height);
					loggedViewport = true;
				}
				st.viewportW = static_cast<int>(desc.Width);
				st.viewportH = static_cast<int>(desc.Height);
			}
			surface->Release();
		}
	}

	void Draw()
	{
		Perf::Scope timer(Perf::kDraw);
		++calls;
		CaptureViewport();

		// TODO(render stream): build GPU sections from kRenAtlas / kRenSection / kRenScene / ... and
		// draw them in the right render phase. For now: consume and count, so Minecraft never
		// stalls on a full ring.
		drainedBytes += Link::Get().DrainRender(
			[](std::uint32_t a_type, const std::uint8_t*, std::uint32_t a_bytes) {
				if (a_type < static_cast<std::uint32_t>(kTypes)) {
					++msgsByType[a_type];
					bytesByType[a_type] += a_bytes;
				} else {
					++otherMsgs;
				}
			},
			64ull << 20);

		Overlay::Frame();

		const auto now = ::GetTickCount64();
		if (now - lastLogMs < 1000) {
			return;
		}
		const double secs = lastLogMs ? double(now - lastLogMs) / 1000.0 : 1.0;
		lastLogMs = now;
		const auto overlayFrames = Overlay::TakeFrames();
		if (drainedBytes || overlayFrames) {
			char        line[512];
			int         n = std::snprintf(line, sizeof(line), "render ring drained %llu KiB in %.1fs (%u draw calls):",
                static_cast<unsigned long long>(drainedBytes >> 10), secs, calls);
			static const char* kNames[kTypes] = { "pad", "atlas", "section", "clearAll", "texture", "avatar", "scene", "atlasRegion", "lights", "ragdoll",
				"solids", "dug" };
			for (int t = 0; t < kTypes && n > 0 && n < static_cast<int>(sizeof(line)); ++t) {
				if (msgsByType[t]) {
					n += std::snprintf(line + n, sizeof(line) - n, " %s %u (%llu KiB)", kNames[t], msgsByType[t],
						static_cast<unsigned long long>(bytesByType[t] >> 10));
				}
			}
			if (otherMsgs && n > 0 && n < static_cast<int>(sizeof(line))) {
				n += std::snprintf(line + n, sizeof(line) - n, " unknown %u", otherMsgs);
			}
			LC_LOG("%s; overlay frames acquired %u (front slot %u, overlay view %s)", line, overlayFrames, Link::Get().FrontSlot(),
				Link::Get().OverlayMapped() ? "mapped" : "NOT mapped");
		}
		drainedBytes = 0;
		otherMsgs = 0;
		calls = 0;
		for (int t = 0; t < kTypes; ++t) {
			msgsByType[t] = 0;
			bytesByType[t] = 0;
		}
	}
}
