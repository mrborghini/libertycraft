// GTA's depth from before its transparent pass, so that GTA's glass (vehicle windows, shop windows)
// doesn't hide Minecraft's blocks, entities and body behind it. Render thread only.
//
// Our draw command runs at the end of GTA's frame and depth-tests against GTA's scene depth buffer
// (INTZ), into which GTA's forward pass has written its glass. A GTA IV frame (DebugFrameTrace):
// the G-buffer pass (4 render targets, a depth buffer of its own), its depth copied into the scene
// depth buffer, decals, the deferred lights, the water, then the first time the scene depth buffer is
// read as a texture (GTA copies it to an R32F for its refraction and depth of field), then the forward
// pass (vehicle glass and the like, blended with depth writes), post-processing, our draw command.
//
// So hooks on SetRenderTarget and SetTexture (device vtable slots 37 and 65) wait for the G-buffer
// pass (render target 2 or 3 bound) and then for that first read of the scene depth texture, and right
// there copy the scene depth (raw, whatever its encoding: FusionFix's logarithmic depth included) into
// an R32F texture of ours, with the device state saved and put back around it. At our draw command,
// before the blocks, a full-screen pass writes that copy back into the scene depth buffer with a
// GREATER depth test: only where the forward pass left something nearer (its glass) does the depth go
// back to what is behind it. GTA clears that depth buffer right after our draw command (for its HUD),
// so nothing of GTA's sees the change. The glass then shows under the blocks instead of over them.
// Without a copy this frame (no G-buffer pass seen, no read of the depth after it) the blocks test
// against GTA's depth as it is.
//
// The R32F copy is D3DPOOL_DEFAULT: a hook on IDirect3DDevice9::Reset releases it first.
#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace lc::render::opaquedepth
{
	// Every draw command of ours, drawn or not, before anything is drawn: a_scene is the depth buffer
	// bound there (GTA's scene depth; compared only), a_drawn true when this command draws the frame,
	// a_want (with a_drawn) that its blocks are depth-tested, so the next frames need the copy. Hooks the
	// device on the first call (RenderBehindGlass=1).
	void AtDrawCommand(IDirect3DDevice9* a_d, IDirect3DSurface9* a_scene, bool a_drawn, bool a_want);
	// In the drawing command (the caller saves and restores the device state), before the blocks: GTA's
	// depth buffer gets this frame's copy back where the transparent pass left it nearer. False: no copy
	// this frame.
	bool Restore(IDirect3DDevice9* a_d, std::uint32_t a_w, std::uint32_t a_h);
	// Set while our draw command draws (our own device calls are not GTA's).
	void SetOurs(bool a_ours);
}
