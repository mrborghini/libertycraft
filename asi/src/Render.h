// Minecraft's blocks, entities and HUD inside GTA IV's frame.
//
// drawingEvent (IV-SDK: CRenderPhasePostRenderViewport's draw-list build, on the GAME thread,
// twice per frame in game) is where GTA IV records the frame's draw commands; the render thread
// executes them later. Direct3D must only be touched on the render thread, so Draw() records a
// draw command of our own (a game DC with our vtable, see Render.cpp) carrying a snapshot of the
// frame's camera and state; its Execute runs on the render thread at that point of the frame
// (after the 3D scene and post-processing, before GTA's HUD) and drains the render ring,
// uploads, draws the blocks (render/World) and composites the overlay (Overlay).
#pragma once

namespace lc::Render
{
	void Draw();  // drawingEvent
}
