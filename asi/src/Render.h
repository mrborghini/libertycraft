// drawingEvent (CRenderPhasePostRenderViewport, twice per frame in game).
// STUB: another stream draws Minecraft's blocks (kRenAtlas / kRenSection / ...) here. For now it
// drains the render ring every frame so Minecraft never stalls on a full ring, counts what went
// by (logged at 1 Hz while anything arrives), and captures the back buffer size for SkyState.
#pragma once

namespace lc::Render
{
	void Draw();
}
