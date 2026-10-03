// The Direct3D 9 side of the sun shadows (render/Shadows.h has the maths): our cascade atlas (an R32F
// colour target the size of GTA's, plus its depth buffer) that Minecraft's geometry is drawn into
// (World.cpp draws, this sets up each cascade), and the full-screen pass that darkens GTA's world where
// the blocks shade it. Render thread only.
//
// The atlas lives in D3DPOOL_DEFAULT, so a hook on IDirect3DDevice9::Reset releases it before the game
// resets the device (it is made again on the next frame).
#pragma once

#include "render/Frame.h"
#include "render/Lighting.h"
#include "render/Shadows.h"

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DBaseTexture9;

namespace lc::render
{
	// This frame's shadows, from Render.cpp to World::Draw.
	struct ShadowFrame
	{
		ShadowParams           params;              // params.fwd[3] 0: no shadows this frame
		IDirect3DBaseTexture9* gtaAtlas = nullptr;  // GTA's cascade atlas (it sat on sampler 15): R32F, width = 4 x height
		std::uint32_t          atlasW = 0, atlasH = 0;
		bool                   cast = true;         // the blocks cast: our atlas and the darkening pass
		float                  casterBias = 0.0f;   // light-depth units (render/Shadows.h sc.z)
		float                  strength = 1.0f;     // the darkening pass (1: as GTA would shade)
		float                  distance = 128.0f;   // blocks farther from the camera than this (m) cast nothing
		IDirect3DBaseTexture9* gbuffer2 = nullptr;  // GTA's G-buffer 2 (its z scales the ambient), when found
		bool                   view = false;        // DebugShadowView: the blocks show the shadow term
		bool                   dump = false;        // DebugShadows: both atlases to <gamedir>/libertycraft-shadow-*.pgm after casting
	};

	struct ShadowStats
	{
		std::uint32_t frames = 0, casterDraws = 0, darkenFrames = 0;
		double        casterCpuMs = 0.0, darkenCpuMs = 0.0;  // submitting
		double        casterGpuMs = 0.0, darkenGpuMs = 0.0;  // GPU timestamps (gpuFrames of them)
		std::uint32_t gpuFrames = 0;
	};

	namespace shadowpass
	{
		// Our atlas the size of GTA's and the shaders; false: the blocks can't cast this frame.
		bool Prepare(IDirect3DDevice9* a_d, const ShadowFrame& a_s);
		// Binds our atlas (cleared) with the caster shaders and state, for cascade 0..3 in turn; the
		// caller sets the vertex declaration, the index buffer and draws. EndCasters puts GTA's render
		// target and depth buffer back.
		bool BeginCasters(IDirect3DDevice9* a_d, const ShadowFrame& a_s);
		void Cascade(IDirect3DDevice9* a_d, const ShadowFrame& a_s, int a_k);
		void EndCasters(IDirect3DDevice9* a_d, std::uint32_t a_draws);
		// DebugShadows: GTA's atlas and ours read back (a GPU stall) to <gamedir>/libertycraft-shadow-gta.pgm
		// and -ours.pgm (a quarter of the size, the nearest-to-the-sun texel of each 4x4), with a log
		// line per cascade.
		void DumpAtlases(IDirect3DDevice9* a_d, const ShadowFrame& a_s);
		// Our atlas when it holds this frame's blocks, else null.
		IDirect3DBaseTexture9* OurAtlas();
		// GTA's frame darkened where the blocks shade it (before the blocks are drawn).
		void Darken(IDirect3DDevice9* a_d, const FrameSnapshot& a_f, const LightingParams& a_light, const ShadowFrame& a_s, std::uint32_t a_w,
			std::uint32_t a_h);
		// Binds the atlases (s2 GTA's, s3 ours or GTA's again) and c14-c27 for a pixel shader using LC_HLSL_SHADOW.
		void BindLookup(IDirect3DDevice9* a_d, const ShadowFrame* a_s);
		// GTA's N.L remap (FusionFix's c220.zw: saturate(N.L * a_scale + a_offset)), for the blocks' sun
		// term also while there are no shadows.
		void SetNdlRemap(float a_scale, float a_offset);
		// After this frame's shadow passes (closes its GPU timing).
		void FrameDone();
		// A new device: drop everything (its resources went with the old one).
		void Forget();
		ShadowStats TakeStats();
	}
}
