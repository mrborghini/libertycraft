// Pure, SDK-free and D3D-free helpers of the block renderer: camera matrices, frustum culling,
// Minecraft's lighting terms, atlas mipmaps, quad compression of section meshes and the per-frame
// geometry of Minecraft's world things (selection outline, cracks, dropped items/blocks, arrows).
// Header-only so tests/render_test.cpp exercises it on Linux.
//
// Matrix convention: row vectors, clip = (x, y, z, 1) * M, like RAGE and D3DX. M[r][c].
// Space convention: "camera-relative" = GTA world axes (x east, y north, z up, metres) with the
// camera at the origin. Section/mesh vertices are Minecraft blocks relative to a Minecraft
// origin; the vertex shader turns them into camera-relative GTA positions:
//   rel = offset + (x, -z, y)     offset = McToGta(origin) - camera
#pragma once

#include "libertycraft_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lc::render
{
	namespace proto = ::libertycraft::proto;
	using Vertex = proto::RenVertex;

	// RenVertex::flags bits (the protocol's, plus the ones SkyCraft's renderer adds for its own
	// per-frame geometry).
	inline constexpr std::uint32_t kFlagCutout = 1;
	inline constexpr std::uint32_t kFlagTranslucent = 2;
	inline constexpr std::uint32_t kFlagUntextured = 4;            // colour only (outline)
	inline constexpr std::uint32_t kFlagNoMip = 8;                 // always the full-size texture
	inline constexpr std::uint32_t kFlagNormalFromFaces = 7u << 4;  // shaded by the triangle's own normal
	inline constexpr std::uint32_t kFullSkyLight = 15u << 8;

	struct Mat4
	{
		float m[4][4];
	};

	inline Mat4 Identity()
	{
		Mat4 r{};
		for (int i = 0; i < 4; ++i) {
			r.m[i][i] = 1.0f;
		}
		return r;
	}

	inline Mat4 Mul(const Mat4& a, const Mat4& b)
	{
		Mat4 r{};
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				double s = 0.0;
				for (int k = 0; k < 4; ++k) {
					s += double(a.m[i][k]) * double(b.m[k][j]);
				}
				r.m[i][j] = float(s);
			}
		}
		return r;
	}

	// (x, y, z, 1) * M
	inline void Transform(const Mat4& a_m, const float a_p[3], float a_out[4])
	{
		for (int c = 0; c < 4; ++c) {
			a_out[c] = a_p[0] * a_m.m[0][c] + a_p[1] * a_m.m[1][c] + a_p[2] * a_m.m[2][c] + a_m.m[3][c];
		}
	}

	// The game's view matrix (world -> camera, row vectors) without its translation, times its
	// projection: camera-relative world positions -> clip space.
	inline Mat4 CameraRelativeClip(const Mat4& a_view, const Mat4& a_proj)
	{
		Mat4 v = a_view;
		v.m[3][0] = v.m[3][1] = v.m[3][2] = 0.0f;
		v.m[3][3] = 1.0f;
		return Mul(v, a_proj);
	}

	// A D3D-style (left-handed view space, z in [0, w]) camera-relative clip matrix from a camera
	// basis in world axes. a_fovY in radians, vertical.
	inline Mat4 ClipFromBasis(const float a_right[3], const float a_forward[3], const float a_up[3], float a_fovY, float a_aspect, float a_near, float a_far)
	{
		Mat4 view{};  // world -> (right, up, forward)
		for (int r = 0; r < 3; ++r) {
			view.m[r][0] = a_right[r];
			view.m[r][1] = a_up[r];
			view.m[r][2] = a_forward[r];
		}
		view.m[3][3] = 1.0f;
		const float ys = 1.0f / std::tan(a_fovY * 0.5f);
		const float xs = ys / (a_aspect > 0.0f ? a_aspect : 1.0f);
		Mat4        proj{};
		proj.m[0][0] = xs;
		proj.m[1][1] = ys;
		proj.m[2][2] = a_far / (a_far - a_near);
		proj.m[2][3] = 1.0f;
		proj.m[3][2] = -a_near * a_far / (a_far - a_near);
		return Mul(view, proj);
	}

	// Frustum side planes (left, right, bottom, top) and "in front of the camera" from a
	// camera-relative clip matrix, normalised: dot(p, n) + d >= -radius means maybe visible.
	struct Frustum
	{
		float plane[5][4];

		static Frustum FromClip(const Mat4& a_m)
		{
			Frustum f{};
			auto col = [&](int c, float o[4]) {
				for (int r = 0; r < 4; ++r) {
					o[r] = a_m.m[r][c];
				}
			};
			float c0[4], c1[4], c3[4];
			col(0, c0);
			col(1, c1);
			col(3, c3);
			for (int r = 0; r < 4; ++r) {
				f.plane[0][r] = c3[r] + c0[r];
				f.plane[1][r] = c3[r] - c0[r];
				f.plane[2][r] = c3[r] + c1[r];
				f.plane[3][r] = c3[r] - c1[r];
				f.plane[4][r] = c3[r];  // w > 0
			}
			for (auto& p : f.plane) {
				const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
				if (len > 1e-12f) {
					for (float& v : p) {
						v /= len;
					}
				}
			}
			return f;
		}

		bool SphereVisible(const float a_c[3], float a_radius) const
		{
			for (const auto& p : plane) {
				if (p[0] * a_c[0] + p[1] * a_c[1] + p[2] * a_c[2] + p[3] < -a_radius) {
					return false;
				}
			}
			return true;
		}
	};

	// ---- Minecraft's lighting terms (mirrored in the pixel shader) ----------------------------
	// Light level (0..1) -> brightness, Minecraft's falloff curve.
	inline float LightCurve(float a_l) { return a_l / (4.0f - 3.0f * a_l); }

	// Minecraft's directional face shade by RenVertex normal index (Direction ordinal + 1):
	// 1 down 0.5, 2 up 1.0, 3/4 north/south 0.8, 5/6 west/east 0.6; 0 (no normal) 1.0.
	inline float FaceShade(int a_normalIndex)
	{
		static constexpr float kShade[8] = { 1.0f, 0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f, 1.0f };
		return kShade[a_normalIndex & 7];
	}

	// How much of the sky light reaches the ground at a game hour (0..24): 1 by day, ~0.2 at
	// night (moonlight), smooth through dawn (5-7) and dusk (19-21). Minecraft's sky darkening.
	inline float DayFactor(float a_hour)
	{
		float h = std::fmod(a_hour, 24.0f);
		if (h < 0.0f) {
			h += 24.0f;
		}
		constexpr float kNight = 0.2f;
		auto smooth = [](float t) {
			t = std::clamp(t, 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		};
		float day;
		if (h < 5.0f || h >= 21.0f) {
			day = 0.0f;
		} else if (h < 7.0f) {
			day = smooth((h - 5.0f) / 2.0f);
		} else if (h < 19.0f) {
			day = 1.0f;
		} else {
			day = 1.0f - smooth((h - 19.0f) / 2.0f);
		}
		return kNight + (1.0f - kNight) * day;
	}

	// ---- section keys ------------------------------------------------------------------------
	inline std::uint64_t SectionKey(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
	{
		return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) |
		       (std::uint32_t(a_z) & 0x1FFFFF);
	}

	// ---- atlas mipmaps -----------------------------------------------------------------------
	// Number of mip levels for a texture: the full chain, capped (16-pixel sprites stay inside
	// their own atlas cell down to 1 pixel with 5 levels).
	inline std::uint32_t MipLevels(std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_cap)
	{
		std::uint32_t n = 1;
		while (n < a_cap && (a_w >> n) >= 1 && (a_h >> n) >= 1) {
			++n;
		}
		return n;
	}

	// One 2x2 box-filter step over 4-byte pixels whose 4th byte is alpha (RGBA or BGRA: the colour
	// channels are averaged alike). Colour is alpha-weighted so transparent texels (often black)
	// don't darken the edges of cutout sprites. a_dst is (a_w/2) x (a_h/2), tightly packed.
	inline void Downsample(const std::uint8_t* a_src, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_srcPitch, std::uint8_t* a_dst,
		std::uint32_t a_dstPitch)
	{
		const std::uint32_t w = std::max(1u, a_w / 2), h = std::max(1u, a_h / 2);
		for (std::uint32_t y = 0; y < h; ++y) {
			const std::uint32_t y0 = std::min(a_h - 1, y * 2), y1 = std::min(a_h - 1, y * 2 + 1);
			for (std::uint32_t x = 0; x < w; ++x) {
				const std::uint32_t x0 = std::min(a_w - 1, x * 2), x1 = std::min(a_w - 1, x * 2 + 1);
				const std::uint8_t* p[4] = { a_src + y0 * a_srcPitch + x0 * 4, a_src + y0 * a_srcPitch + x1 * 4, a_src + y1 * a_srcPitch + x0 * 4,
					a_src + y1 * a_srcPitch + x1 * 4 };
				std::uint32_t a = 0;
				std::uint32_t c[3] = { 0, 0, 0 }, cu[3] = { 0, 0, 0 };
				for (const auto* q : p) {
					a += q[3];
					for (int k = 0; k < 3; ++k) {
						c[k] += std::uint32_t(q[k]) * q[3];
						cu[k] += q[k];
					}
				}
				std::uint8_t* d = a_dst + y * a_dstPitch + x * 4;
				for (int k = 0; k < 3; ++k) {
					d[k] = static_cast<std::uint8_t>(a ? (c[k] + a / 2) / a : (cu[k] + 2) / 4);
				}
				d[3] = static_cast<std::uint8_t>((a + 2) / 4);
			}
		}
	}

	// How many mip levels of an atlas rectangle can be rebuilt from the rectangle alone: level L
	// needs x, y, w, h to be multiples of 2^L (sprites sit on cells of their own size).
	inline std::uint32_t RegionMipLevels(std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_levels)
	{
		std::uint32_t n = 1;
		while (n < a_levels) {
			const std::uint32_t mask = (1u << n) - 1;
			if ((a_x | a_y | a_w | a_h) & mask) {
				break;
			}
			++n;
		}
		return n;
	}

	// ---- section meshes ----------------------------------------------------------------------
	// Minecraft's quads arrive as triangle lists in the order (0 1 2) (0 2 3). If every group of
	// six vertices follows it, the mesh can be stored as 4 vertices per quad with a shared index
	// buffer (a third less memory, which counts in a 32-bit game).
	inline bool IsQuadList(const Vertex* a_v, std::uint32_t a_count)
	{
		if (a_count == 0 || a_count % 6) {
			return false;
		}
		for (std::uint32_t q = 0; q < a_count; q += 6) {
			if (std::memcmp(&a_v[q], &a_v[q + 3], sizeof(Vertex)) || std::memcmp(&a_v[q + 2], &a_v[q + 4], sizeof(Vertex))) {
				return false;
			}
		}
		return true;
	}

	// Opaque/cutout triangles first, translucent after; quads compressed to 4 vertices when the
	// whole mesh is a quad list. Returns the vertex counts of both parts in a_out.
	struct SectionMesh
	{
		std::vector<Vertex> vertices;
		std::uint32_t       opaque = 0;       // vertices (or quad corners) of the opaque part
		std::uint32_t       translucent = 0;  // the same, after it
		bool                quads = false;    // 4 vertices per quad (draw with the quad index buffer)
	};

	inline void BuildSectionMesh(const Vertex* a_src, std::uint32_t a_count, SectionMesh& a_out)
	{
		a_count -= a_count % 3;
		a_out.vertices.clear();
		a_out.quads = IsQuadList(a_src, a_count);
		const std::uint32_t stride = a_out.quads ? 6 : 3;
		a_out.vertices.reserve(a_out.quads ? a_count / 6 * 4 : a_count);
		for (int pass = 0; pass < 2; ++pass) {
			for (std::uint32_t t = 0; t < a_count; t += stride) {
				const bool translucent = (a_src[t].flags & kFlagTranslucent) != 0;
				if (translucent != (pass == 1)) {
					continue;
				}
				if (a_out.quads) {
					a_out.vertices.push_back(a_src[t]);
					a_out.vertices.push_back(a_src[t + 1]);
					a_out.vertices.push_back(a_src[t + 2]);
					a_out.vertices.push_back(a_src[t + 5]);
				} else {
					a_out.vertices.insert(a_out.vertices.end(), a_src + t, a_src + t + 3);
				}
			}
			if (pass == 0) {
				a_out.opaque = static_cast<std::uint32_t>(a_out.vertices.size());
			}
		}
		a_out.translucent = static_cast<std::uint32_t>(a_out.vertices.size()) - a_out.opaque;
	}

	// ---- per-frame geometry of Minecraft's world things (port of SkyCraft's BuildEntities) ----
	// Positions are Minecraft blocks relative to an integer Minecraft origin near the camera.
	class EntityBuilder
	{
	public:
		std::vector<Vertex> solid;     // dropped items and blocks, arrows
		std::vector<Vertex> cracks;    // block-breaking cracks (blended, after the translucent blocks)
		std::vector<Vertex> outline;   // the selection box, as a line list

		void Clear()
		{
			solid.clear();
			cracks.clear();
			outline.clear();
		}

		void Build(const proto::WorldEntities& a_e, const double a_o[3])
		{
			const std::uint32_t count = std::min(a_e.count, proto::kMaxWorldEntities);
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& e = a_e.entities[i];
				const float px = float(e.x - a_o[0]), py = float(e.y - a_o[1]), pz = float(e.z - a_o[2]);
				switch (e.kind) {
				case proto::kWeBlock:
					{
						// A dropped block: a small cube spinning about its centre, like in Minecraft.
						const float s = e.scale;
						const float mn[3] = { px - s * 0.5f, py - s * 0.5f, pz - s * 0.5f };
						const float sz[3] = { s, s, s };
						Box(solid, mn, sz, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], e.tint, kFlagCutout | kFlagNoMip, true);
						break;
					}
				case proto::kWeCrack:
					{
						const float mn[3] = { px, py, pz };
						Box(cracks, mn, e.ext, 0.0f, e.uv[0], e.uv[0], e.uv[0], 0, kFlagTranslucent | kFlagNoMip, false);
						break;
					}
				case proto::kWeArrow:
				case proto::kWeTrident:
					{
						// Minecraft arrows face (sin yaw, sin pitch, cos yaw).
						const float yaw = e.yaw * kPi / 180.0f, pitch = e.pitch * kPi / 180.0f;
						const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
						Arrow(px, py, pz, d, e.uv[0], e.uv[1], e.kind == proto::kWeTrident);
						break;
					}
				case proto::kWeItem:
					{
						// A flat sprite turning about the vertical, like a dropped item.
						const float spin = e.yaw * kPi / 180.0f, half = e.scale * 0.5f;
						const float rx = std::cos(spin) * half, rz = std::sin(spin) * half;
						const float p[4][3] = {
							{ px - rx, py + half, pz - rz },
							{ px + rx, py + half, pz + rz },
							{ px + rx, py - half, pz + rz },
							{ px - rx, py - half, pz - rz },
						};
						Quad(solid, p, e.uv[0], 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
						break;
					}
				default:
					break;  // kWeShadow: not geometry
				}
			}
			if (a_e.hasSelection) {
				Outline(a_e.selMin, a_e.selMax, a_o);
			}
		}

		// The 12 edges of the targeted block's box, 45% black like Minecraft's outline.
		void Outline(const float a_min[3], const float a_max[3], const double a_o[3])
		{
			constexpr float g = 0.002f;
			const float     lo[3] = { float(a_min[0] - a_o[0]) - g, float(a_min[1] - a_o[1]) - g, float(a_min[2] - a_o[2]) - g };
			const float     hi[3] = { float(a_max[0] - a_o[0]) + g, float(a_max[1] - a_o[1]) + g, float(a_max[2] - a_o[2]) + g };
			static constexpr int kEdges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
			constexpr std::uint32_t kOutline = 0x73000000u;  // RGBA8: black, alpha 0x73 (45%)
			for (const auto& edge : kEdges) {
				for (int k : edge) {
					outline.push_back({ (k & 1) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1], (k & 4) ? hi[2] : lo[2], 0, 0, kOutline, kFullSkyLight, kFlagUntextured });
				}
			}
		}

		static constexpr float kPi = 3.14159265f;

	private:
		static void Quad(std::vector<Vertex>& a_out, const float a_p[4][3], const float a_uv[4], std::uint32_t a_color, std::uint32_t a_flags)
		{
			const float uv[4][2] = { { a_uv[0], a_uv[1] }, { a_uv[2], a_uv[1] }, { a_uv[2], a_uv[3] }, { a_uv[0], a_uv[3] } };
			for (int k : { 0, 1, 2, 0, 2, 3 }) {
				a_out.push_back({ a_p[k][0], a_p[k][1], a_p[k][2], uv[k][0], uv[k][1], a_color, kFullSkyLight, a_flags });
			}
		}

		// A brightness times a tint (RGBA8, r in the low byte), as a vertex colour.
		static std::uint32_t Shade(float a_shade, std::uint32_t a_tint)
		{
			float r = a_shade, g = a_shade, b = a_shade;
			if (a_tint) {
				r *= float(a_tint & 0xFF) / 255.0f;
				g *= float((a_tint >> 8) & 0xFF) / 255.0f;
				b *= float((a_tint >> 16) & 0xFF) / 255.0f;
			}
			return 0xFF000000u | (std::uint32_t(b * 255.0f) << 16) | (std::uint32_t(g * 255.0f) << 8) | std::uint32_t(r * 255.0f);
		}

		// An axis-aligned box (rotated a_yaw about its vertical centre line), sides a_side, top
		// a_top, bottom a_bottom. Faces wind counter-clockwise seen from outside.
		static void Box(std::vector<Vertex>& a_out, const float a_min[3], const float a_size[3], float a_yaw, const float a_side[4], const float a_top[4],
			const float a_bottom[4], std::uint32_t a_topTint, std::uint32_t a_flags, bool a_shaded)
		{
			const float cx = a_min[0] + a_size[0] * 0.5f, cz = a_min[2] + a_size[2] * 0.5f;
			const float c = std::cos(a_yaw), s = std::sin(a_yaw);
			auto        corner = [&](int a_i, float a_o[3]) {
                const float lx = ((a_i & 1) ? 0.5f : -0.5f) * a_size[0], lz = ((a_i & 4) ? 0.5f : -0.5f) * a_size[2];
                a_o[0] = cx + lx * c - lz * s;
                a_o[1] = a_min[1] + ((a_i & 2) ? a_size[1] : 0.0f);
                a_o[2] = cz + lx * s + lz * c;
			};
			// corner bits: 1 = +x, 2 = +y, 4 = +z; each face TL, TR, BR, BL seen from outside
			static constexpr int kFaces[6][4] = {
				{ 6, 7, 5, 4 },  // south (+z)
				{ 3, 2, 0, 1 },  // north (-z)
				{ 7, 3, 1, 5 },  // east (+x)
				{ 2, 6, 4, 0 },  // west (-x)
				{ 2, 3, 7, 6 },  // top
				{ 4, 5, 1, 0 },  // bottom
			};
			for (int f = 0; f < 6; ++f) {
				float p[4][3];
				for (int k = 0; k < 4; ++k) {
					corner(kFaces[f][k], p[k]);
				}
				const float*        uv = f == 4 ? a_top : f == 5 ? a_bottom : a_side;
				const std::uint32_t color = a_shaded ? Shade(1.0f, f == 4 ? a_topTint : 0) : 0xFFFFFFFFu;
				Quad(a_out, p, uv, color, a_flags | kFlagNormalFromFaces);
			}
		}

		// Minecraft's arrow (or a trident) at a position, flying along d (unit, Minecraft axes).
		void Arrow(float px, float py, float pz, const float d[3], const float* a_uvSide, const float* a_uvBack, bool a_trident)
		{
			constexpr float kArrowScale = 0.55f;  // Minecraft's arrow is chunky next to GTA's people
			float           s[3] = { d[2], 0.0f, -d[0] };
			float           sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
			if (sl < 1e-3f) {
				s[0] = 1.0f, s[2] = 0.0f, sl = 1.0f;
			}
			s[0] /= sl, s[2] /= sl;
			const float     u[3] = { s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0] };
			constexpr float r = 0.70710678f;
			const float     fins[2][3] = { { (u[0] + s[0]) * r, (u[1] + s[1]) * r, (u[2] + s[2]) * r }, { (u[0] - s[0]) * r, (u[1] - s[1]) * r, (u[2] - s[2]) * r } };
			auto            at = [&](float a_along, const float* a_q, float a_side, const float* a_q2, float a_side2, float a_o[3]) {
                const float pos[3] = { px, py, pz };
                for (int k = 0; k < 3; ++k) {
                    a_o[k] = pos[k] + d[k] * a_along + a_q[k] * a_side + (a_q2 ? a_q2[k] * a_side2 : 0.0f);
                }
			};
			if (!a_trident) {
				// Minecraft's ArrowModel (1/16 block units, scaled 0.9): two fins 16 long, 4 wide,
				// from x -12 (fletching) to +4 (head), and a 4x4 back plate at x -11.
				constexpr float k = 0.9f / 16.0f * kArrowScale;
				for (const auto& q : fins) {
					float p[4][3];
					at(-12 * k, q, -2 * k, nullptr, 0, p[0]);
					at(4 * k, q, -2 * k, nullptr, 0, p[1]);
					at(4 * k, q, 2 * k, nullptr, 0, p[2]);
					at(-12 * k, q, 2 * k, nullptr, 0, p[3]);
					Quad(solid, p, a_uvSide, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
				}
				float p[4][3];
				at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, p[0]);
				at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, p[1]);
				at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, p[2]);
				at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, p[3]);
				Quad(solid, p, a_uvBack, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
			} else {
				// Tridents: the item icon, whose diagonal runs handle (bottom-left) to tip (top-right).
				constexpr float h = 0.9f;
				for (const auto& q : fins) {
					float p[4][3];
					at(0, q, h, nullptr, 0, p[0]);
					at(h, q, 0, nullptr, 0, p[1]);
					at(0, q, -h, nullptr, 0, p[2]);
					at(-h, q, 0, nullptr, 0, p[3]);
					Quad(solid, p, a_uvSide, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
				}
			}
		}
	};

	// ---- overlay -----------------------------------------------------------------------------
	// Where Minecraft's crosshair (15 GUI pixels) and the attack indicator under it (16 x 16 from
	// 9 GUI pixels below the centre) are, in back buffer pixels: {x0, y0, x1, y1}; empty if none.
	inline void CrosshairRect(int a_guiScale, float a_scaleX, float a_bbW, float a_bbH, float a_out[4])
	{
		if (a_guiScale <= 0) {
			a_out[0] = a_out[1] = a_out[2] = a_out[3] = 0.0f;
			return;
		}
		const float g = float(a_guiScale) * a_scaleX;
		const float cx = a_bbW * 0.5f, cy = a_bbH * 0.5f;
		a_out[0] = cx - 12.0f * g;
		a_out[1] = cy - 12.0f * g;
		a_out[2] = cx + 12.0f * g;
		a_out[3] = cy + 28.0f * g;
	}
}
