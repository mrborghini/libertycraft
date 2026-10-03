// Minecraft's 2D overlay (GUI, HUD, screens) from the triple buffer, composited over GTA's frame
// like SkyCraft's Overlay: a full-screen quad with premultiplied alpha, then Minecraft's
// crosshair/attack indicator with its invert blend, and the GUI cursor while a screen is open.
// Render thread only (called from Render's draw command).
#pragma once

#include "render/Frame.h"

#include <cstdint>

struct IDirect3DDevice9;

namespace lc::Overlay
{
	// Takes Minecraft's newest published frame (keeps the swap going). Copies it into the
	// overlay texture when a_wanted (it is about to be drawn).
	void Upload(IDirect3DDevice9* a_device, bool a_wanted);
	// Draws the overlay over the bound render target (a_width x a_height). The caller saves and
	// restores device state.
	void Draw(IDirect3DDevice9* a_device, const render::FrameSnapshot& a_frame, std::uint32_t a_width, std::uint32_t a_height);
	// Frames acquired / uploaded since the last call.
	std::uint32_t TakeFrames();
	std::uint32_t TakeUploads();
	double        TakeUploadMs();
}
