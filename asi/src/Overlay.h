// Minecraft's 2D overlay (GUI, HUD, screens) from the triple buffer, drawn over the game.
// STUB: acquires each newly published frame (so the swap keeps going) and counts them.
// TODO(render stream): upload the front slot into a D3D9 texture and draw it full screen.
#pragma once

#include <cstdint>

namespace lc::Overlay
{
	// Once per frame from Render::Draw.
	void Frame();
	// Frames acquired since the last call.
	std::uint32_t TakeFrames();
}
