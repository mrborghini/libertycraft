// GTA IV's lighting for Minecraft's blocks: its sun/moon and ambient, its distance fog and its tone
// mapping, so the blocks sit in GTA's picture at every hour and in every weather. Pure (SDK- and
// D3D-free) so tests/render_test.cpp checks it on Linux; the pixel shader (render/Shaders.h)
// mirrors LightFace, FogAmount and ToneMap.
//
// What GTA does (read from the game's own shaders, common/shaders/win32_30/*.fxc and FusionFix's
// update/ copies; every GTA shader shares its global parameters at fixed registers):
//  1. deferred_lighting.fxc, the sun pass, in HDR units:
//       albedo * (gDirectionalColour.rgb * gDirectionalColour.w * saturate(dot(n, -gDirectionalLight))
//                 + (gLightAmbient0 + gLightAmbient1 * saturate(0.5 - 0.5 n.z)) * ao)
//     (in game gLightAmbient1 holds the timecycle's Amb1 minus Amb0: the ambient blends from Amb0 on
//     upward faces to Amb1 on downward ones, so it can be negative per channel)
//  2. rage_postfx.fxc, the fog pass, by view depth d: far things desaturate (gDepthFxParams), then
//       lerp(colour, lerp(globalFogColorN, globalFogColor, ramp), amount)
//       ramp = saturate((d - start) / (end - start)),
//       amount = w * saturate(d / start) + (1 - w) * ramp + z      (globalFogParams = start, end, z, w)
//  3. rage_postfx.fxc, the tone mapping (the last pass before the HUD):
//       x = hdr * Exposure * ToneMapParams.y / adaptedLuminance      (a 1x1 R32F texture)
//       x += max(x - ToneMapParams.x, 0) * ToneMapParams.z / 4       (bloom, from a blurred copy; here
//                                                                      the pixel's own light stands in)
//       x = lerp(lum(x), x, deSatContrastGamma.x), ColorShift, * ColorCorrect * 2,
//       * saturate(lum(x)) ^ (deSatContrastGamma.z - 1)
// Our draw command runs right after 3, where the fog/tone constants and the adapted luminance are
// still on the device; the sun/ambient registers are reused by later passes, so Render.cpp keeps
// the values the scene's sun pass set (GtaSun).
#pragma once

#include "render/RenderMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lc::render
{
	// GTA IV's global pixel shader registers (the same in every GTA shader).
	namespace gtareg
	{
		inline constexpr unsigned kDepthFx = 16;    // gDepthFxParams: x far saturation, y far gamma, z near, w far (m)
		inline constexpr unsigned kDirLight = 17;   // gDirectionalLight: xyz the way the sun/moon light travels
		inline constexpr unsigned kDirColour = 18;  // gDirectionalColour: rgb colour, w intensity
		inline constexpr unsigned kAmbient0 = 37;   // gLightAmbient0: rgb ambient on every face
		inline constexpr unsigned kAmbient1 = 38;   // gLightAmbient1: rgb down-face ambient minus gLightAmbient0 (may be negative)
		inline constexpr unsigned kFogParams = 41;  // globalFogParams: x start, y end (m), z offset, w near-ramp weight
		inline constexpr unsigned kFogColor = 42;   // globalFogColor: far fog colour
		inline constexpr unsigned kFogColorN = 43;  // globalFogColorN: near fog colour
		inline constexpr unsigned kExposure = 66;   // rage_postfx Exposure (x), in every tone mapping variant
	}

	inline float Luminance(const float a_rgb[3])
	{
		return 0.2125f * a_rgb[0] + 0.7154f * a_rgb[1] + 0.0721f * a_rgb[2];
	}

	namespace detail
	{
		inline bool Finite(const float* a_v, int a_n)
		{
			for (int i = 0; i < a_n; ++i) {
				if (!std::isfinite(a_v[i])) {
					return false;
				}
			}
			return true;
		}
		// An HDR colour GTA would plausibly use (lights reach ~20, fog colours a few units).
		inline bool Colour(const float* a_v, float a_max = 1000.0f)
		{
			return Finite(a_v, 3) && a_v[0] >= 0.0f && a_v[1] >= 0.0f && a_v[2] >= 0.0f && a_v[0] < a_max && a_v[1] < a_max && a_v[2] < a_max;
		}
		inline bool Near(float a_a, float a_b, float a_rel)
		{
			return std::fabs(a_a - a_b) <= a_rel * std::max(std::fabs(a_b), 1e-9f);
		}
	}

	// ---- 1. the sun pass's light ------------------------------------------------------------------
	struct GtaSun
	{
		float dir[4]{};     // gDirectionalLight: the way the light travels
		float colour[4]{};  // gDirectionalColour: rgb, w intensity
		float amb0[4]{};
		float amb1[4]{};
	};

	inline bool SunValid(const GtaSun& a_s)
	{
		const float  down[3] = { a_s.amb0[0] + a_s.amb1[0], a_s.amb0[1] + a_s.amb1[1], a_s.amb0[2] + a_s.amb1[2] };  // a downward face's ambient
		const float* d = a_s.dir;
		const float  len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		return detail::Finite(d, 3) && std::fabs(len - 1.0f) < 0.05f && detail::Colour(a_s.colour, 100.0f) && std::isfinite(a_s.colour[3]) &&
		       a_s.colour[3] >= 0.0f && a_s.colour[3] < 1000.0f && detail::Colour(a_s.amb0) && detail::Finite(a_s.amb1, 3) &&
		       detail::Colour(down) && Luminance(a_s.amb0) + Luminance(down) + Luminance(a_s.colour) * a_s.colour[3] > 1e-3f;
	}

	// ---- 2. fog ---------------------------------------------------------------------------------
	struct GtaFog
	{
		float params[4]{};  // start, end, offset, near-ramp weight
		float colour[4]{};  // far
		float colourN[4]{};
		float depthFx[4]{};
	};

	inline bool FogValid(const GtaFog& a_f)
	{
		const float* f = a_f.params;
		return detail::Finite(f, 4) && f[0] >= 0.0f && f[1] > f[0] && f[1] < 1e6f && detail::Colour(a_f.colour) && detail::Colour(a_f.colourN);
	}

	inline bool DepthFxValid(const GtaFog& a_f)
	{
		const float* x = a_f.depthFx;
		return detail::Finite(x, 4) && x[2] >= 0.0f && x[3] > x[2] && x[0] >= 0.0f && x[0] <= 4.0f && x[1] > 0.0f && x[1] <= 4.0f;
	}

	// ---- 3. tone mapping ------------------------------------------------------------------------
	struct GtaTone
	{
		float exposure = 0.0f;  // Exposure.x
		float tmp[4]{};         // ToneMapParams: x bloom threshold, y key, z bloom intensity
		float dsg[4]{};         // deSatContrastGamma: x saturation, z gamma
		float cc[4]{};          // ColorCorrect
		float cs[4]{};          // ColorShift: rgb, w scale
	};

	inline bool ToneValid(const GtaTone& a_t)
	{
		return std::isfinite(a_t.exposure) && a_t.exposure > 0.0f && a_t.exposure < 1000.0f && detail::Finite(a_t.tmp, 4) && a_t.tmp[1] > 0.0f &&
		       a_t.tmp[1] < 100.0f && detail::Finite(a_t.dsg, 4) && a_t.dsg[0] >= 0.0f && a_t.dsg[0] <= 4.0f && a_t.dsg[2] >= 0.1f && a_t.dsg[2] <= 5.0f &&
		       detail::Colour(a_t.cc, 8.0f) && Luminance(a_t.cc) > 0.01f && detail::Finite(a_t.cs, 4) && a_t.cs[3] >= 0.0f;
	}

	// The tone mapping pass's locals sit at different registers per variant (DOF / motion blur on or
	// off). From c64 up (a_c[i] = c64+i, 32 registers), told apart by TexelSize (1/width, 1/height):
	//   TexelSize c76: ToneMapParams c81, deSatContrastGamma c82, ColorCorrect c83, ColorShift c84
	//   TexelSize c72: ToneMapParams c76 .. ColorShift c79, or (the plainest variant) c73 .. c76
	// Returns the variant's ToneMapParams register (0: none fits).
	inline unsigned FindTone(const float (&a_c)[32][4], float a_texelX, float a_texelY, GtaTone& a_out)
	{
		auto at = [&](unsigned a_reg) { return a_c[a_reg - 64]; };
		auto texel = [&](unsigned a_reg) { return detail::Near(at(a_reg)[0], a_texelX, 0.01f) && detail::Near(at(a_reg)[1], a_texelY, 0.01f); };
		auto take = [&](unsigned a_tmp) {
			a_out.exposure = at(gtareg::kExposure)[0];
			std::copy(at(a_tmp), at(a_tmp) + 4, a_out.tmp);
			std::copy(at(a_tmp + 1), at(a_tmp + 1) + 4, a_out.dsg);
			std::copy(at(a_tmp + 2), at(a_tmp + 2) + 4, a_out.cc);
			std::copy(at(a_tmp + 3), at(a_tmp + 3) + 4, a_out.cs);
			return ToneValid(a_out);
		};
		if (texel(76) && take(81)) {
			return 81;
		}
		if (texel(72) && take(76)) {
			return 76;
		}
		if (texel(72) && take(73)) {
			return 73;
		}
		a_out = GtaTone{};
		return 0;
	}

	// ---- what the world pixel shader gets -------------------------------------------------------
	// Registers c1..c13 (Shaders.h), HDR light in GTA's units; the shader tone maps like GTA.
	struct LightingParams
	{
		float sunDir[4]{ 0, 0, 1, 0 };          // towards the sun/moon (GTA axes); w: 1 = GTA lighting on
		float sun[4]{};                         // rgb sun/moon colour x intensity
		float amb0[4]{};                        // rgb ambient
		float amb1[4]{};                        // rgb down-face ambient minus amb0
		float fog[4]{ 1e6f, 2e6f, 0, 0 };       // start, end (m), offset, near-ramp weight
		float fogColor[4]{};                    // rgb far fog colour; w: 1 = fog on
		float fogColorN[4]{};                   // rgb near fog colour
		float depthFx[4]{ 1, 1, 1e6f, 2e6f };   // far saturation, far gamma, near, far (m)
		float tone[4]{ 1, 0.8f, 1, 1 };         // x Exposure x key, y saturation, z gamma, w adapted luminance (without the texture)
		float grade[4]{ 1, 1, 1, 0 };           // rgb ColorCorrect x 2; w: 1 = adapted luminance from GTA's texture (sampler s1)
		float shift[4]{ 0, 0, 0, 0 };           // ColorShift rgb, w scale
		float misc[4]{ 1, 0, 1, 0 };            // x RenderExposure, y wetness (0..1), z block light strength
		float bloom[4]{ 0, 0, 0, 0 };           // x threshold (ToneMapParams.x), y intensity (ToneMapParams.z / 4)
	};

	// Without GTA's tone mapping constants: an auto exposure stand-in. GTA keeps a sunlit surface
	// near the same brightness from dawn to dusk, while its minimum adapted luminance (the
	// timecycle's LumMin) leaves the night dark: adapted = max(ref, floor) / key, ref = the light on
	// an average sunlit surface.
	struct ExposureTuning
	{
		float exposure = 1.0f;    // RenderExposure (the user's knob, also with GTA's tone mapping)
		float key = 0.85f;        // stand-in: screen brightness of a white surface under the reference light
		float floor = 11.0f;      // stand-in: the reference light never counts as less than this
		float saturation = 0.8f;  // stand-in: GTA's tone mapping desaturates (timecycle 0.35 to 0.75)
	};

	inline float ReferenceLight(const GtaSun& a_s)
	{
		const float sun[3] = { a_s.colour[0] * a_s.colour[3], a_s.colour[1] * a_s.colour[3], a_s.colour[2] * a_s.colour[3] };
		return std::max(0.0f, 0.75f * Luminance(sun) + Luminance(a_s.amb0) + 0.5f * Luminance(a_s.amb1));
	}

	inline float StandInAdaptedLuminance(const GtaSun& a_s, const ExposureTuning& a_t)
	{
		return std::max(ReferenceLight(a_s), std::max(a_t.floor, 1e-3f)) / std::max(a_t.key, 1e-3f);
	}

	struct LightingInputs
	{
		GtaSun  sun;
		GtaFog  fog;
		GtaTone tone;
		bool    sunOk = false, fogOk = false, depthFxOk = false, toneOk = false;
		bool    adaptedTexture = false;  // GTA's adapted luminance texture is bound for the shader
		float   rain = 0.0f;             // CWeather::Rain
	};

	inline LightingParams MakeLighting(const LightingInputs& a_in, const ExposureTuning& a_t)
	{
		LightingParams p;
		p.misc[0] = a_t.exposure > 0.0f ? a_t.exposure : 1.0f;
		p.misc[1] = std::isfinite(a_in.rain) ? std::clamp(a_in.rain, 0.0f, 1.0f) : 0.0f;
		if (!a_in.sunOk) {
			return p;  // sunDir.w = 0: the shader keeps Minecraft's own lighting
		}
		const auto& s = a_in.sun;
		p.sunDir[0] = -s.dir[0];
		p.sunDir[1] = -s.dir[1];
		p.sunDir[2] = -s.dir[2];
		p.sunDir[3] = 1.0f;
		for (int i = 0; i < 3; ++i) {
			p.sun[i] = s.colour[i] * s.colour[3];
			p.amb0[i] = s.amb0[i];
			p.amb1[i] = s.amb1[i];
		}
		if (a_in.fogOk) {
			for (int i = 0; i < 4; ++i) {
				p.fog[i] = a_in.fog.params[i];
			}
			for (int i = 0; i < 3; ++i) {
				p.fogColor[i] = a_in.fog.colour[i];
				p.fogColorN[i] = a_in.fog.colourN[i];
			}
			p.fogColor[3] = 1.0f;
		}
		if (a_in.depthFxOk) {
			for (int i = 0; i < 4; ++i) {
				p.depthFx[i] = a_in.fog.depthFx[i];
			}
		}
		if (a_in.toneOk && a_in.adaptedTexture) {
			const auto& t = a_in.tone;
			p.tone[0] = t.exposure * t.tmp[1];
			p.tone[1] = t.dsg[0];
			p.tone[2] = t.dsg[2];
			p.tone[3] = 1.0f;
			for (int i = 0; i < 3; ++i) {
				p.grade[i] = 2.0f * t.cc[i];
				p.shift[i] = t.cs[i];
			}
			p.grade[3] = 1.0f;
			p.shift[3] = t.cs[3];
			p.bloom[0] = t.tmp[0];
			p.bloom[1] = std::clamp(t.tmp[2], 0.0f, 16.0f) * 0.25f;
		} else {
			p.tone[0] = 1.0f;
			p.tone[1] = std::clamp(a_t.saturation, 0.0f, 2.0f);
			p.tone[2] = 1.0f;
			p.tone[3] = StandInAdaptedLuminance(s, a_t);
		}
		return p;
	}

	// ---- mirrored in the pixel shader -----------------------------------------------------------
	// HDR light on a face with normal a_n (GTA axes, unit; zero = unknown) from GTA's sun and
	// ambient, Minecraft's sky light a_sky (0..1, after its curve) as the occlusion: what Minecraft
	// roofs over gets less ambient and no sun.
	inline void LightFace(const LightingParams& a_p, const float a_n[3], float a_sky, float a_out[3])
	{
		const bool  has = a_n[0] != 0.0f || a_n[1] != 0.0f || a_n[2] != 0.0f;
		const float ndl = has ? std::max(0.0f, a_n[0] * a_p.sunDir[0] + a_n[1] * a_p.sunDir[1] + a_n[2] * a_p.sunDir[2])
		                      : 0.35f + 0.4f * std::max(0.0f, a_p.sunDir[2]);
		const float down = has ? std::clamp(0.5f - 0.5f * a_n[2], 0.0f, 1.0f) : 0.5f;
		const float occ = 0.3f + 0.7f * a_sky;
		for (int i = 0; i < 3; ++i) {
			a_out[i] = (a_p.amb0[i] + a_p.amb1[i] * down) * occ + a_p.sun[i] * ndl * a_sky * a_sky;
		}
	}

	// GTA's fog amount and the fog colour's near-to-far blend at view depth a_d (m).
	inline float FogAmount(const LightingParams& a_p, float a_d, float* a_ramp = nullptr)
	{
		const float start = a_p.fog[0], end = a_p.fog[1];
		const float ramp = std::clamp((a_d - start) / std::max(end - start, 1e-3f), 0.0f, 1.0f);
		const float nearRamp = std::clamp(a_d / std::max(start, 1e-3f), 0.0f, 1.0f);
		if (a_ramp) {
			*a_ramp = ramp;
		}
		if (a_p.fogColor[3] < 0.5f) {
			return 0.0f;
		}
		return std::clamp(a_p.fog[3] * nearRamp + (1.0f - a_p.fog[3]) * ramp + a_p.fog[2], 0.0f, 1.0f);
	}

	// GTA's tone mapping of an HDR colour (bloom left out), times RenderExposure; a_adapted is the
	// adapted luminance (GTA's texture, or tone.w).
	inline void ToneMap(const LightingParams& a_p, float a_adapted, const float a_hdr[3], float a_out[3])
	{
		const float e = a_p.tone[0] / std::max(a_adapted, 1e-6f);
		float       x[3] = { a_hdr[0] * e, a_hdr[1] * e, a_hdr[2] * e };
		for (float& v : x) {
			v += std::max(v - a_p.bloom[0], 0.0f) * a_p.bloom[1];
		}
		const float lum = Luminance(x);
		const float a = lum * a_p.shift[3];
		const float sa = std::clamp(a, 0.0f, 1.0f);
		const float g = std::pow(std::clamp(lum, 0.0f, 1.0f), a_p.tone[2] - 1.0f);
		for (int i = 0; i < 3; ++i) {
			x[i] = lum + a_p.tone[1] * (x[i] - lum);
			x[i] = sa * (x[i] - a_p.shift[i] * a) + a_p.shift[i] * a;
			a_out[i] = std::max(0.0f, x[i] * a_p.grade[i] * g) * a_p.misc[0];
		}
	}

	// ---- the rider's mount in a vehicle ---------------------------------------------------------
	// Minecraft's mount (the boat under the player riding in GTA's vehicle) comes in the scene mesh
	// among the other entities. These are the triangles that move with the rider: centroid within
	// this box around Minecraft's feet (blocks; a boat/horse/minecart and its rider fit).
	inline bool InMountBox(float a_dx, float a_dy, float a_dz)
	{
		return a_dx * a_dx + a_dz * a_dz <= 2.5f * 2.5f && a_dy >= -2.0f && a_dy <= 2.0f;
	}

	// Splits batched triangles (positions relative to a_origin, Minecraft blocks) into the ones
	// around a_feet (InMountBox) and the rest, keeping each batch's texture and flags.
	inline void SplitMount(const std::vector<proto::RenBatch>& a_batches, const std::vector<Vertex>& a_verts, const double a_origin[3], const double a_feet[3],
		std::vector<proto::RenBatch>& a_restBatches, std::vector<Vertex>& a_rest, std::vector<proto::RenBatch>& a_mountBatches, std::vector<Vertex>& a_mount)
	{
		a_restBatches.clear();
		a_rest.clear();
		a_mountBatches.clear();
		a_mount.clear();
		const float ox = float(a_origin[0] - a_feet[0]), oy = float(a_origin[1] - a_feet[1]), oz = float(a_origin[2] - a_feet[2]);
		for (const auto& b : a_batches) {
			const std::size_t r0 = a_rest.size(), m0 = a_mount.size();
			const std::size_t end = std::min<std::size_t>(a_verts.size(), std::size_t(b.first) + b.count);
			for (std::size_t k = b.first; k + 3 <= end; k += 3) {
				const Vertex* v = &a_verts[k];
				const float   cx = (v[0].x + v[1].x + v[2].x) / 3.0f + ox;
				const float   cy = (v[0].y + v[1].y + v[2].y) / 3.0f + oy;
				const float   cz = (v[0].z + v[1].z + v[2].z) / 3.0f + oz;
				auto&         dst = InMountBox(cx, cy, cz) ? a_mount : a_rest;
				dst.insert(dst.end(), v, v + 3);
			}
			if (a_rest.size() > r0) {
				a_restBatches.push_back({ b.texture, std::uint32_t(r0), std::uint32_t(a_rest.size() - r0), b.flags });
			}
			if (a_mount.size() > m0) {
				a_mountBatches.push_back({ b.texture, std::uint32_t(m0), std::uint32_t(a_mount.size() - m0), b.flags });
			}
		}
	}
}
