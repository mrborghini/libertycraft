// Sun shadows for Minecraft's blocks, the pure (SDK- and D3D-free) part: tests/render_test.cpp checks it
// on Linux and the HLSL in render/Shaders.h mirrors ShadowCoord and DarkenFactor.
//
// What GTA IV does (FusionFix's deferred sun pass, update/common/shaders/win32_30/deferred_lighting.fxc,
// pixel programs 22/23; FusionFix 5 makes the cascade atlas an R32F colour target, 4 cascades side by
// side, width = 4 x height, and sets the cascade ranges to 10, 30, 85 and 256 m):
//   cascade flags  f = (1, d >= gFacetCentre.x, d >= .y, d >= .z), d = dot(camera forward, P)
//   one-hot        oh = |f - (f.yzw, 0)|
//   scale, offset  dot(f, gShadowParam0123), dot(f, gShadowParam4567); dot(f, gShadowParam891113),
//                  dot(f, gShadowParam14151617)
//   shadow coords  sc = P * gShadowMatrix (columns x, y and w): atlas uv = sc.xy * scale + offset,
//                  receiver value zr = 0.5 - 0.0005 * sc.z
//   bounds         b = gShadowParam18192021.w - (gFacetCentre.w - gFacetCentre): each cascade's far end
//                  (view depth); c53.z scales the filter, c53.w is where the shadows have faded out
//   normal offset  sc += (N * gShadowMatrix).xyw * lerp(far, next far, t) * c53.z * bias * fov factor
//   filter         4 rotated taps (FusionFix "Definition" off) or 16 rotated Poisson taps, each
//                  "texel >= zr" (1 = lit), x clamped into the cascade's quarter; CHSS widens it by the
//                  blocker distance
//   fade           lit = lerp(lit, 1, saturate(|P - camera| / c53.w)^2)
//   light          albedo * (ambient + sun * saturate(N.L * c220.z + c220.w) * lit) (+ specular * lit)
// The registers (pixel shader globals, the same in every GTA shader): gShadowParam18192021 c53,
// gFacetCentre c54, gShadowParam14151617 c56, gShadowParam0123 c57, gShadowParam4567 c58,
// gShadowParam891113 c59, gShadowMatrix c60-c63, gShadowZSamplerDir s15; FusionFix's own c217 (CHSS max
// softness, light size), c218 (softness, bias, CHSS on, cascade blend size), c220 (N.L remap), c221 (x
// Definition: 16 taps), c223 (y fov factor).
//
// Minecraft's blocks cast into an atlas of our own with the same layout (the same uv and zr for a point),
// so one lookup gives both GTA's shadow and GTA's plus ours, with GTA's own filtering.
#pragma once

#include "render/RenderMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace lc::render
{
	namespace shreg
	{
		inline constexpr unsigned kFirst = 53;  // c53 .. c63
		inline constexpr unsigned kCount = 11;
		// rows of GtaShadowSet::r
		inline constexpr unsigned kParam18 = 0;  // c53 gShadowParam18192021: z filter scale, w fade distance (m)
		inline constexpr unsigned kFacet = 1;    // c54 gFacetCentre
		inline constexpr unsigned kParam14 = 3;  // c56 gShadowParam14151617: offset y per cascade step
		inline constexpr unsigned kParam0 = 4;   // c57 gShadowParam0123: scale x
		inline constexpr unsigned kParam4 = 5;   // c58 gShadowParam4567: scale y
		inline constexpr unsigned kParam8 = 6;   // c59 gShadowParam891113: offset x
		inline constexpr unsigned kMatrix = 7;   // c60 .. c63 gShadowMatrix (rows)
		// FusionFix's globals, c217 .. c223 (rows of FusionShadowTune::r)
		inline constexpr unsigned kFfFirst = 217;
		inline constexpr unsigned kFfCount = 7;
	}

	inline constexpr int   kCascades = 4;
	inline constexpr float kZrScale = -0.0005f;  // zr = 0.5 - 0.0005 * sc.z
	inline constexpr float kZrBias = 0.5f;

	// c53 .. c63 as GTA's sun pass set them.
	struct GtaShadowSet
	{
		float r[shreg::kCount][4]{};
	};

	// FusionFix's c217 .. c223 (zero when unknown: defaults are used).
	struct FusionShadowTune
	{
		float r[shreg::kFfCount][4]{};
		bool  known = false;
	};

	// What the shaders get (registers c14 .. c27 of the world and darkening pixel shaders, Shaders.h).
	// Positions are camera-relative GTA metres.
	struct ShadowParams
	{
		float fwd[4]{};        // xyz the camera's forward axis (view depth = dot(fwd, rel)); w 1 = shadows on
		float split[4]{};      // xyz the cascade thresholds on view depth, w the fade distance (m)
		float mx[4]{}, my[4]{}, mz[4]{};  // shadow coords: sc = (dot(float4(rel, 1), mx), ... my, ... mz)
		float casc[kCascades][4]{};       // per cascade: atlas uv = sc.xy * xy + zw
		float bounds[4]{};     // each cascade's far end (view depth, m)
		float filter[4]{};     // x filter scale (c53.z), y softness, z normal offset bias, w cascade blend size
		float misc[4]{};       // x fov factor, y 1 = 16 taps (else 4), z 1 = CHSS, w aspect (scale.x / scale.y of cascade 0)
		float chss[4]{};       // x CHSS max softness, y CHSS light size, z 1 = our atlas holds the blocks' shadows, w 1 = DebugShadowView
		float ndl[4]{ 1, 0, 0, 0 };  // GTA's N.L remap: saturate(N.L * x + y)
	};
	static_assert(sizeof(ShadowParams) == 14 * 16);

	namespace shdetail
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
		inline float Dot4(const float a_a[4], const float a_b[4])
		{
			return a_a[0] * a_b[0] + a_a[1] * a_b[1] + a_a[2] * a_b[2] + a_a[3] * a_b[3];
		}
		// f = (1, k >= 1, k >= 2, k >= 3)
		inline void Flags(int a_k, float a_f[4])
		{
			a_f[0] = 1.0f;
			a_f[1] = a_k >= 1 ? 1.0f : 0.0f;
			a_f[2] = a_k >= 2 ? 1.0f : 0.0f;
			a_f[3] = a_k >= 3 ? 1.0f : 0.0f;
		}
	}

	// Each cascade's far end on view depth: b = c53.w - (c54.w - c54).
	inline void CascadeBounds(const GtaShadowSet& a_s, float a_out[4])
	{
		const float* p18 = a_s.r[shreg::kParam18];
		const float* fc = a_s.r[shreg::kFacet];
		for (int i = 0; i < 4; ++i) {
			a_out[i] = p18[3] - (fc[3] - fc[i]);
		}
	}

	// Atlas uv scale and offset of cascade a_k.
	inline void CascadeTransform(const GtaShadowSet& a_s, int a_k, float a_out[4])
	{
		float f[4];
		shdetail::Flags(a_k, f);
		a_out[0] = shdetail::Dot4(f, a_s.r[shreg::kParam0]);
		a_out[1] = shdetail::Dot4(f, a_s.r[shreg::kParam4]);
		a_out[2] = shdetail::Dot4(f, a_s.r[shreg::kParam8]);
		a_out[3] = shdetail::Dot4(f, a_s.r[shreg::kParam14]);
	}

	// A set GTA's sun pass could have used: finite, cascades that grow, a fade distance, a usable matrix.
	inline bool ShadowSetValid(const GtaShadowSet& a_s)
	{
		if (!shdetail::Finite(&a_s.r[0][0], int(shreg::kCount * 4))) {
			return false;
		}
		const float* p18 = a_s.r[shreg::kParam18];
		if (!(p18[3] > 1.0f && p18[3] < 1e5f && p18[2] > 0.0f && p18[2] < 1.0f)) {
			return false;
		}
		float b[4];
		CascadeBounds(a_s, b);
		if (!(b[0] > 0.0f && b[1] > b[0] && b[2] > b[1] && b[3] > b[2])) {
			return false;
		}
		for (int k = 0; k < kCascades; ++k) {
			float t[4];
			CascadeTransform(a_s, k, t);
			if (std::fabs(t[0]) < 1e-12f || std::fabs(t[1]) < 1e-12f) {
				return false;
			}
		}
		// The matrix maps somewhere: its x and y columns aren't zero.
		const float(*m)[4] = a_s.r + shreg::kMatrix;
		const float cx = std::fabs(m[0][0]) + std::fabs(m[1][0]) + std::fabs(m[2][0]);
		const float cy = std::fabs(m[0][1]) + std::fabs(m[1][1]) + std::fabs(m[2][1]);
		return cx > 1e-9f && cy > 1e-9f;
	}

	// Was the set made for this camera? GTA's cascade thresholds (gFacetCentre.xyz) are the cascades' far
	// ends plus the camera's depth along its forward axis; another viewport's set (a water reflection),
	// or one caught half-way while GTA switches between two, doesn't fit.
	inline bool ShadowSetFitsCamera(const GtaShadowSet& a_s, const double a_cam[3], const float a_fwd[3], double a_tolerance = 0.5)
	{
		const double fl = std::sqrt(double(a_fwd[0]) * a_fwd[0] + double(a_fwd[1]) * a_fwd[1] + double(a_fwd[2]) * a_fwd[2]);
		if (!(fl > 1e-6)) {
			return false;
		}
		const double camDepth = (a_fwd[0] * a_cam[0] + a_fwd[1] * a_cam[1] + a_fwd[2] * a_cam[2]) / fl;
		float        b[4];
		CascadeBounds(a_s, b);
		for (int i = 0; i < 3; ++i) {
			if (!(std::fabs(double(a_s.r[shreg::kFacet][i]) - camDepth - double(b[i])) < a_tolerance)) {
				return false;
			}
		}
		return true;
	}

	// FusionFix's filter settings, with its defaults ("sharp" filter, Definition on) when unknown.
	struct ShadowTuning
	{
		float softness = 1.5f, bias = 5.0f, blend = 0.1f, fov = 1.0f;
		bool  taps16 = true, chss = false;
		float chssMax = 10.0f, chssLight = 1.0f;
		float ndlScale = 1.0f, ndlOffset = 0.0f;
	};

	inline ShadowTuning TuningFrom(const FusionShadowTune& a_t)
	{
		ShadowTuning out;
		if (!a_t.known || !shdetail::Finite(&a_t.r[0][0], int(shreg::kFfCount * 4))) {
			return out;
		}
		const float* c217 = a_t.r[0];
		const float* c218 = a_t.r[1];
		const float* c220 = a_t.r[3];
		const float* c221 = a_t.r[4];
		const float* c223 = a_t.r[6];
		if (std::fabs(c218[0]) > 0.0f && std::fabs(c218[0]) < 100.0f) {
			out.softness = std::fabs(c218[0]);
			out.bias = std::fabs(c218[1]);
			out.chss = c218[2] != 0.0f;
			out.blend = std::fabs(c218[3]);
		}
		if (c223[1] > 0.0f && c223[1] < 10.0f) {
			out.fov = c223[1];
		}
		out.taps16 = c221[0] != 0.0f;
		if (std::fabs(c217[0]) > 0.0f) {
			out.chssMax = std::fabs(c217[0]);
			out.chssLight = std::fabs(c217[1]);
		}
		if (c220[2] > 0.0f && c220[2] < 4.0f && std::fabs(c220[3]) < 2.0f) {
			out.ndlScale = c220[2];
			out.ndlOffset = c220[3];
		}
		return out;
	}

	// The shader constants for this frame: GTA's set made camera-relative (in double: the matrix maps
	// absolute world metres), a_forward the camera's forward axis (unit, GTA world axes).
	inline ShadowParams MakeShadowParams(const GtaShadowSet& a_s, const ShadowTuning& a_t, const double a_cam[3], const float a_forward[3], bool a_ours)
	{
		ShadowParams p;
		const double fl = std::sqrt(double(a_forward[0]) * a_forward[0] + double(a_forward[1]) * a_forward[1] + double(a_forward[2]) * a_forward[2]);
		const double f[3] = { a_forward[0] / fl, a_forward[1] / fl, a_forward[2] / fl };
		const double camDepth = f[0] * a_cam[0] + f[1] * a_cam[1] + f[2] * a_cam[2];
		for (int i = 0; i < 3; ++i) {
			p.fwd[i] = float(f[i]);
			p.split[i] = float(double(a_s.r[shreg::kFacet][i]) - camDepth);
		}
		p.fwd[3] = 1.0f;
		p.split[3] = a_s.r[shreg::kParam18][3];
		const float(*m)[4] = a_s.r + shreg::kMatrix;
		float* const cols[3] = { p.mx, p.my, p.mz };
		const int    comp[3] = { 0, 1, 3 };  // the sun pass uses the matrix's x, y and w columns
		for (int c = 0; c < 3; ++c) {
			const int j = comp[c];
			double    off = double(m[3][j]);
			for (int r = 0; r < 3; ++r) {
				cols[c][r] = m[r][j];
				off += a_cam[r] * double(m[r][j]);
			}
			cols[c][3] = float(off);
		}
		for (int k = 0; k < kCascades; ++k) {
			CascadeTransform(a_s, k, p.casc[k]);
		}
		CascadeBounds(a_s, p.bounds);
		p.filter[0] = a_s.r[shreg::kParam18][2];
		p.filter[1] = a_t.softness;
		p.filter[2] = a_t.bias;
		p.filter[3] = a_t.blend;
		p.misc[0] = a_t.fov;
		p.misc[1] = a_t.taps16 ? 1.0f : 0.0f;
		p.misc[2] = a_t.chss ? 1.0f : 0.0f;
		p.misc[3] = p.casc[0][0] / p.casc[0][1];
		p.chss[0] = a_t.chssMax;
		p.chss[1] = a_t.chssLight;
		p.chss[2] = a_ours ? 1.0f : 0.0f;
		p.ndl[0] = a_t.ndlScale;
		p.ndl[1] = a_t.ndlOffset;
		return p;
	}

	// ---- mirrored in the pixel shaders (SunShadow) ------------------------------------------------
	struct ShadowCoord
	{
		int   cascade = 0;
		float uv[2]{};      // atlas uv of the receiver (normal offset applied)
		float zr = 0.0f;    // the value it is compared with: lit if texel >= zr
		float clampU[2]{};  // the taps' u stays in here (the cascade's quarter)
		float radius[2]{};  // filter radius in uv (before the softness)
		float fade = 0.0f;  // 1: shadows faded out at this distance
	};

	// A camera-relative point a_rel with normal a_n (zero: none) -> where GTA's sun pass looks it up.
	inline ShadowCoord ShadowLookup(const ShadowParams& a_p, const float a_rel[3], const float a_n[3])
	{
		ShadowCoord c;
		const float d = a_p.fwd[0] * a_rel[0] + a_p.fwd[1] * a_rel[1] + a_p.fwd[2] * a_rel[2];
		const float f[4] = { 1.0f, d >= a_p.split[0] ? 1.0f : 0.0f, d >= a_p.split[1] ? 1.0f : 0.0f, d >= a_p.split[2] ? 1.0f : 0.0f };
		const float oh[4] = { std::fabs(f[0] - f[1]), std::fabs(f[1] - f[2]), std::fabs(f[2] - f[3]), std::fabs(f[3]) };
		c.cascade = int(f[1] + f[2] + f[3]);
		float so[4] = {};
		for (int k = 0; k < kCascades; ++k) {
			for (int i = 0; i < 4; ++i) {
				so[i] += oh[k] * a_p.casc[k][i];
			}
		}
		const float p4[4] = { a_rel[0], a_rel[1], a_rel[2], 1.0f };
		float       sc[3] = { shdetail::Dot4(p4, a_p.mx), shdetail::Dot4(p4, a_p.my), shdetail::Dot4(p4, a_p.mz) };
		const float nsc[3] = { a_n[0] * a_p.mx[0] + a_n[1] * a_p.mx[1] + a_n[2] * a_p.mx[2], a_n[0] * a_p.my[0] + a_n[1] * a_p.my[1] + a_n[2] * a_p.my[2],
			a_n[0] * a_p.mz[0] + a_n[1] * a_p.mz[1] + a_n[2] * a_p.mz[2] };
		const float* b = a_p.bounds;
		const float  farB = shdetail::Dot4(oh, b);
		const float  bn[4] = { b[1], b[2], b[3], b[3] };
		const float  nextB = shdetail::Dot4(oh, bn);
		const float  widths[4] = { b[0], b[1] - b[0], b[2] - b[1], b[3] - b[2] };
		const float  width = shdetail::Dot4(oh, widths);
		const float  blendW = std::max(width * a_p.filter[3], 1e-4f);
		const float  t0 = std::clamp((d - (farB - width)) / width, 0.0f, 1.0f);
		const float  t1 = std::clamp((d - (farB - blendW)) / blendW, 0.0f, 1.0f);
		float        r20x = farB + (nextB - farB) * t0;
		float        r20y = 1.0f + (nextB / farB - 1.0f) * t1;
		r20x *= a_p.filter[0];
		r20y *= a_p.filter[0];
		const float bias = r20x * a_p.filter[2] * a_p.misc[0];
		for (int i = 0; i < 3; ++i) {
			sc[i] += nsc[i] * bias;
		}
		c.zr = kZrBias + kZrScale * sc[2];
		c.uv[0] = sc[0] * so[0] + so[2];
		c.uv[1] = sc[1] * so[1] + so[3];
		const float left[4] = { -1.0f, 0.25f, 0.5f, 0.75f }, right[4] = { 0.25f, 0.5f, 0.75f, 2.0f };
		c.clampU[0] = shdetail::Dot4(oh, left);
		c.clampU[1] = shdetail::Dot4(oh, right) - a_p.filter[0] * 0.25f;
		c.radius[0] = r20y * a_p.misc[3];
		c.radius[1] = r20y;
		const float dist = std::sqrt(a_rel[0] * a_rel[0] + a_rel[1] * a_rel[1] + a_rel[2] * a_rel[2]);
		const float fd = std::clamp(dist / std::max(a_p.split[3], 1e-3f), 0.0f, 1.0f);
		c.fade = fd * fd;
		return c;
	}

	// ---- our atlas: Minecraft's blocks drawn into GTA's cascade layout ------------------------------
	// Cascade a_k's quarter of the atlas in u (where its taps stay), as GTA clamps them.
	inline void CascadeU(int a_k, float& a_lo, float& a_hi)
	{
		static const float lo[4] = { 0.0f, 0.25f, 0.5f, 0.75f }, hi[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
		a_lo = lo[a_k & 3];
		a_hi = hi[a_k & 3];
	}

	// Camera-relative position -> cascade a_k of our atlas, as clip space rows (x = dot(float4(rel, 1),
	// a_out[0]), y = ... a_out[1]) plus the light depth sc.z (a_out[2]); D3D9's half-pixel offset is
	// folded in, so the pixel a point lands on is the texel the lookup samples for it. a_w x a_h is the
	// whole atlas.
	inline void CasterRows(const ShadowParams& a_p, int a_k, std::uint32_t a_w, std::uint32_t a_h, float a_out[3][4])
	{
		const float* t = a_p.casc[a_k & 3];
		for (int i = 0; i < 4; ++i) {
			a_out[0][i] = 2.0f * t[0] * a_p.mx[i];  // x = 2u - 1
			a_out[1][i] = -2.0f * t[1] * a_p.my[i];  // y = 1 - 2v
			a_out[2][i] = a_p.mz[i];
		}
		a_out[0][3] += 2.0f * t[2] - 1.0f - 1.0f / float(std::max(a_w, 1u));
		a_out[1][3] += 1.0f - 2.0f * t[3] + 1.0f / float(std::max(a_h, 1u));
	}

	// Where a point drawn with CasterRows lands, as atlas uv (the inverse of the viewport transform and
	// half-pixel offset; for tests).
	inline void CasterToUv(const float a_rows[3][4], const float a_rel[3], std::uint32_t a_w, std::uint32_t a_h, float a_uv[2])
	{
		const float p4[4] = { a_rel[0], a_rel[1], a_rel[2], 1.0f };
		const float x = shdetail::Dot4(p4, a_rows[0]), y = shdetail::Dot4(p4, a_rows[1]);
		// D3D9: pixel x covers screen [x - 0.5, x + 0.5] around integer centres; texel i's centre is u = (i + 0.5) / w.
		const float px = (x + 1.0f) * 0.5f * float(a_w);  // screen position, D3D9 convention
		const float py = (1.0f - y) * 0.5f * float(a_h);
		a_uv[0] = (px + 0.5f) / float(a_w);
		a_uv[1] = (py + 0.5f) / float(a_h);
	}

	// Does a sphere (camera-relative centre, radius m) reach into cascade a_k's quarter of the atlas?
	inline bool SphereInCascade(const ShadowParams& a_p, int a_k, const float a_c[3], float a_r)
	{
		const float* t = a_p.casc[a_k & 3];
		const float  p4[4] = { a_c[0], a_c[1], a_c[2], 1.0f };
		const float  u = shdetail::Dot4(p4, a_p.mx) * t[0] + t[2];
		const float  v = shdetail::Dot4(p4, a_p.my) * t[1] + t[3];
		const float  ru = a_r * std::fabs(t[0]) * std::sqrt(a_p.mx[0] * a_p.mx[0] + a_p.mx[1] * a_p.mx[1] + a_p.mx[2] * a_p.mx[2]);
		const float  rv = a_r * std::fabs(t[1]) * std::sqrt(a_p.my[0] * a_p.my[0] + a_p.my[1] * a_p.my[1] + a_p.my[2] * a_p.my[2]);
		float        lo, hi;
		CascadeU(a_k, lo, hi);
		return u + ru >= lo && u - ru <= hi && v + rv >= 0.0f && v - rv <= 1.0f;
	}

	// ---- GTA's world in the blocks' shadow ------------------------------------------------------------
	// How much a GTA pixel darkens when the blocks shade it: GTA lit it with ambient + sun * N.L * g (g
	// its own shadow), the blocks leave ambient + sun * N.L * c (c <= g). The ratio, per colour channel,
	// in HDR; fog (which the shadow doesn't reach) pulls it back towards 1; GTA's tone mapping makes the
	// picture ~ HDR ^ gamma (deSatContrastGamma.z), so the frame is multiplied by ratio ^ gamma.
	// a_strength 1 = as GTA would have shaded it.
	inline void DarkenFactor(const float a_amb[3], const float a_sun[3], float a_ndl, float a_g, float a_c, float a_fog, float a_gamma, float a_strength,
		float a_out[3])
	{
		for (int i = 0; i < 3; ++i) {
			const float lit = a_amb[i] + a_sun[i] * a_ndl * a_g;
			const float shaded = a_amb[i] + a_sun[i] * a_ndl * a_c;
			float       r = lit > 1e-4f ? shaded / lit : 1.0f;
			r = std::clamp(r, 0.0f, 1.0f);
			r = r + (1.0f - r) * std::clamp(a_fog, 0.0f, 1.0f);
			r = std::pow(r, std::max(a_gamma, 0.1f));
			a_out[i] = 1.0f + (r - 1.0f) * std::clamp(a_strength, 0.0f, 1.0f);
		}
	}

	// The camera-relative ray of a screen position at view depth 1, from a camera-relative clip matrix
	// (row vectors, w = view depth): ray = ndc.x * a_out[0] + ndc.y * a_out[1] + a_out[2] (xyz). It
	// solves (ray.C0, ray.C1, ray.C3) = (ndc.x, ndc.y, 1) for the clip matrix's columns C. False if the
	// matrix can't be inverted.
	inline bool RayBasis(const float a_clip[4][4], float a_out[3][4])
	{
		double m[3][3];
		const int cols[3] = { 0, 1, 3 };
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				m[r][c] = a_clip[c][cols[r]];  // row r of M = column cols[r] of the clip matrix
			}
		}
		const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		if (!std::isfinite(det) || std::fabs(det) < 1e-12) {
			return false;
		}
		double inv[3][3];
		inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
		inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
		inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
		inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
		inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
		inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
		inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
		inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
		inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
		// ray = inv * (ndc.x, ndc.y, 1): a_out[j] = column j of inv
		for (int j = 0; j < 3; ++j) {
			for (int i = 0; i < 3; ++i) {
				a_out[j][i] = float(inv[i][j]);
			}
			a_out[j][3] = 0.0f;
		}
		return true;
	}

	// GTA's N.L as its sun pass uses it.
	inline float GtaNdl(const ShadowParams& a_p, const float a_n[3], const float a_toSun[3])
	{
		const float d = a_n[0] * a_toSun[0] + a_n[1] * a_toSun[1] + a_n[2] * a_toSun[2];
		return std::clamp(d * a_p.ndl[0] + a_p.ndl[1], 0.0f, 1.0f);
	}
}
