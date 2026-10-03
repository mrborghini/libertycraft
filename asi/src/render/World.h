// Minecraft's world inside GTA IV, on Direct3D 9: the D3D9 port of SkyCraft's WorldRender.
// Consumes the render ring (kRenAtlas, kRenAtlasRegion, kRenSection, kRenClearAll, kRenTexture,
// kRenAvatar, kRenScene; kRenLights/kRenSolids/kRenDug/kRenRagdoll are counted and ignored) and
// draws the block sections, Minecraft's entities and particles, and the WorldEntities table
// (dropped items/blocks, arrows, cracks, the selection outline) with Minecraft-style lighting.
//
// Render thread only (our draw command, see Render.cpp). Static data lives in D3DPOOL_MANAGED
// resources and per-frame geometry goes through DrawPrimitiveUP, so nothing has to be released
// or rebuilt around the game's device Reset.
#pragma once

#include "render/Frame.h"
#include "render/Lighting.h"

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DBaseTexture9;

namespace lc::render
{
	struct TargetInfo
	{
		std::uint32_t width = 0, height = 0;  // the bound render target
		bool          depthTest = false;      // the bound depth buffer matches it: test against it
		IDirect3DBaseTexture9* adaptedLum = nullptr;  // GTA's adapted luminance (1x1), for LightingParams::grade.w
	};

	struct WorldStats
	{
		// since the last TakeStats
		std::uint32_t messages[12]{};
		std::uint64_t messageBytes[12]{};
		std::uint32_t otherMessages = 0;
		std::uint64_t drainedBytes = 0;
		std::uint32_t frames = 0;
		std::uint64_t sectionsDrawn = 0, sectionsCulled = 0, drawCalls = 0, triangles = 0;
		double        drainMs = 0.0, drawMs = 0.0, maxDrawMs = 0.0;
		// current
		std::uint32_t sections = 0;
		std::uint64_t sectionBytes = 0;
		bool          atlas = false;
		std::uint32_t atlasW = 0, atlasH = 0;
	};

	class World
	{
	public:
		static World& Get();

		// Every frame, drawn or not: applies pending render-ring messages (uploads), so Minecraft
		// never stalls on a full ring.
		void Drain(IDirect3DDevice9* a_device);
		// Draws into the bound render target. The caller saves/restores device state. a_light: GTA's
		// lighting for this frame (sunDir.w 0: Minecraft's own). a_mountFrom (kFrameMountShift):
		// where Minecraft's scene has the rider's feet; the mount around them moves to a_frame.feet.
		void Draw(IDirect3DDevice9* a_device, const FrameSnapshot& a_frame, const TargetInfo& a_target, const LightingParams& a_light,
			const double* a_mountFrom);

		WorldStats TakeStats();

	private:
		World() = default;
	};
}
