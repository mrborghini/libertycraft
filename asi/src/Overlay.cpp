// Unity-built into dllmain.cpp (needs IV-SDK). See Overlay.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "overlay"
#include "Overlay.h"

#include "Link.h"
#include "Log.h"

namespace lc::Overlay
{
	namespace
	{
		std::uint32_t frames = 0;
		std::uint64_t lastFrameId = 0;
		bool          loggedFirst = false;
	}

	void Frame()
	{
		auto& link = Link::Get();
		if (!link.AcquireOverlayFrame()) {
			return;
		}
		++frames;
		link.WithFrontSlot([](const proto::OverlaySlotHdr* a_hdr, const std::uint8_t* a_pixels) {
			// TODO(render stream): upload a_hdr->width x a_hdr->height RGBA8 rows (bottom-up when
			// a_hdr->flags & 1) from a_pixels into a dynamic D3D9 texture and draw it full screen.
			(void)a_pixels;
			lastFrameId = a_hdr->frameId;
			if (!loggedFirst) {
				loggedFirst = true;
				LC_LOG("first overlay frame: %ux%u flags 0x%X frame %llu", a_hdr->width, a_hdr->height, a_hdr->flags,
					static_cast<unsigned long long>(a_hdr->frameId));
			}
		});
	}

	std::uint32_t TakeFrames()
	{
		const auto n = frames;
		frames = 0;
		return n;
	}
}
