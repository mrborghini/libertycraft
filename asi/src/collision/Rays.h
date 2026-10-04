// GTA IV line probes for the collision sampler (collision/Geometry.h), in Minecraft space.
// SDK-dependent: included by Collision.cpp (unity-built into dllmain.cpp). Game thread only (the
// physics level isn't safe to query from another thread).
//
// CWorld::ProcessLineOfSight(from, to, null, &results, flags, 1, 0, seeShoot, 4), 1.0.8.0, as
// measured in game (Collision.cpp logs a self-test at the first harvest):
//  * returns true on a hit; tLineOfSightResults +0x10 is the hit position, +0x20 the unit
//    normal of the face (+0x30 a copy), +0x40 the hit's fraction along the segment, +0x44 the
//    distance left to the end;
//  * nUnk1 must be 1 (0 never hits); BUILDINGS (4) is what carries the map's static collision
//    (STATIC_COLLISION alone hits nothing in town);
//  * one-sided: only faces turned towards the ray are reported;
//  * a probe that starts within ~1-2 cm of a face can report that face with a fraction that
//    belongs to something else: such hits are returned with a NaN position (the sampler skips
//    ahead).
#pragma once

#include "Config.h"
#include "Log.h"
#include "Sdk.h"
#include "collision/Geometry.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace lc::col
{
	// Static map collision only: no peds, vehicles or (movable) objects such as doors and bins
	// (those: CastObjectMc, collision/Objects.h).
	inline constexpr std::uint32_t kLosFlags = STATIC_COLLISION | BUILDINGS;
	// 0: no see-through / shoot-through exemptions (glass and fences count as solid).
	inline constexpr std::uint32_t kLosSeeShoot = 0;

	struct RayCounters
	{
		std::uint32_t rays = 0, hits = 0, inconsistent = 0;
	};
	inline RayCounters rayCounters;  // game thread only

	// Raw probe in GTA space.
	inline bool CastGta(const float a_from[3], const float a_to[3], tLineOfSightResults& a_res, std::uint32_t a_flags = kLosFlags)
	{
		CVector from{ a_from[0], a_from[1], a_from[2] };
		CVector to{ a_to[0], a_to[1], a_to[2] };
		std::memset(&a_res, 0, sizeof(a_res));
		++rayCounters.rays;
		return CWorld::ProcessLineOfSight(&from, &to, nullptr, &a_res, a_flags, 1, 0, kLosSeeShoot, 4);
	}

	// GTA IV's materials.dat has this many materials (DEFAULT 0 ... POOLTABLE_POCKET 155).
	inline constexpr std::uint32_t kGtaMaterials = 156;

	// The material of the surface a probe hit: the low byte of the result's dword at +0x48 is its
	// index in common/data/materials/materials.dat (measured with DebugMaterials, 1.0.8.0: 8 TARMAC on
	// the street, 3 CONCRETE and 16 PAVING_SLABS on the pavement, 54 LEAD_ROOFING on a roof, 87 CARPET,
	// 86 LINOLEUM and 20 WOOD_BOARD in Roman's flat). The bytes above it hold other things (0x0001xxxx,
	// 0x0060xxxx, 0x02xxxxxx seen). kNoMaterial when CityMaterials=0 or out of range.
	inline std::uint8_t HitMaterial(const tLineOfSightResults& a_res)
	{
		const std::uint32_t id = a_res.m_nUnkFlags4 & 0xFF;
		return Config::Get().cityMaterials && id < kGtaMaterials ? static_cast<std::uint8_t>(id) : kNoMaterial;
	}

	// A hit (GTA space, from -> to) as a Minecraft-space Hit; junk (see the header) gets a NaN position.
	inline void HitToMc(const float a_from[3], const float a_to[3], const tLineOfSightResults& a_res, Hit& a_out)
	{
		const float* p = &a_res.m_vEndPosition.x;
		const float* n = &a_res.m_vUnk.x;
		const float  dx = a_to[0] - a_from[0], dy = a_to[1] - a_from[1], dz = a_to[2] - a_from[2];
		const float  len = std::sqrt(dx * dx + dy * dy + dz * dz);
		const float  px = p[0] - a_from[0], py = p[1] - a_from[1], pz = p[2] - a_from[2];
		const float  dist = std::sqrt(px * px + py * py + pz * pz);
		if (!std::isfinite(p[0] + p[1] + p[2] + n[0] + n[1] + n[2]) || std::fabs(a_res.m_fUnk1 * len - dist) > 0.05f + 0.001f * len) {
			++rayCounters.inconsistent;
			const float nan = std::numeric_limits<float>::quiet_NaN();
			a_out.pos[0] = a_out.pos[1] = a_out.pos[2] = nan;
			a_out.n[0] = a_out.n[1] = 0.0f;
			a_out.n[2] = 1.0f;
			return;
		}
		// gta (x, y, z) -> mc (x, z, -y)
		a_out.pos[0] = p[0];
		a_out.pos[1] = p[2];
		a_out.pos[2] = -p[1];
		a_out.n[0] = n[0];
		a_out.n[1] = n[2];
		a_out.n[2] = -n[1];
		a_out.mat = HitMaterial(a_res);
	}

	// Object probes (collision/Objects.h), counted apart from the map's.
	struct ObjectRayCounters
	{
		std::uint32_t rays = 0, hits = 0, target = 0, other = 0, unknown = 0;
	};
	inline ObjectRayCounters objectRays;  // game thread only

	// One-sided probe in Minecraft space against OBJECTS only, reporting hits on a_entity alone:
	// anything else in the way (a neighbouring prop, a door) is skipped. A hit whose entity can't
	// be told (no physics instance) counts as the target.
	inline bool CastObjectMc(const float a_from[3], const float a_to[3], const void* a_entity, Hit& a_out)
	{
		float       from[3] = { a_from[0], -a_from[2], a_from[1] };
		const float to[3] = { a_to[0], -a_to[2], a_to[1] };
		for (int pass = 0; pass < 4; ++pass) {
			tLineOfSightResults res;
			++objectRays.rays;
			--rayCounters.rays;  // not a map probe
			if (!CastGta(from, to, res, OBJECTS)) {
				return false;
			}
			++objectRays.hits;
			const rage::phInst* inst = res.m_pInst;
			const void*         ent = inst ? static_cast<const void*>(inst->m_pEntity) : nullptr;
			if (ent == a_entity || !ent) {
				++(ent ? objectRays.target : objectRays.unknown);
				HitToMc(from, to, res, a_out);
				return true;
			}
			++objectRays.other;
			// skip past it
			const float* p = &res.m_vEndPosition.x;
			const float  dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
			const float  len = std::sqrt(dx * dx + dy * dy + dz * dz);
			const float  t = (p[0] - from[0]) * dx + (p[1] - from[1]) * dy + (p[2] - from[2]) * dz;
			if (!std::isfinite(t) || len < 1e-4f) {
				return false;
			}
			const float along = t / len + 0.02f;
			if (along >= len) {
				return false;
			}
			for (int k = 0; k < 3; ++k) {
				from[k] += (k == 0 ? dx : k == 1 ? dy : dz) / len * along;
			}
		}
		return false;
	}

	// DebugMaterials: where in a probe's result is the material of the surface it hit? Histograms of
	// the result's unnamed fields (dwords at 0x04, 0x08, 0x0C, 0x48, 0x4C, 0x50) over many hits, and
	// the first downward hits in full with where they hit, logged every 20000 hits. Game thread.
	struct MaterialProbe
	{
		static constexpr int kFields = 6;
		static constexpr int kOffsets[kFields] = { 0x04, 0x08, 0x0C, 0x48, 0x4C, 0x50 };
		std::uint32_t        values[kFields][48]{};
		std::uint32_t        counts[kFields][48]{};
		std::uint32_t        distinct[kFields]{};
		std::uint32_t        hits = 0, samplesLogged = 0;

		void Sample(const tLineOfSightResults& a_res, const float* a_from, const float* a_to)
		{
			const auto* raw = reinterpret_cast<const std::uint8_t*>(&a_res);
			std::uint32_t v[kFields];
			for (int f = 0; f < kFields; ++f) {
				std::memcpy(&v[f], raw + kOffsets[f], 4);
				int k = 0;
				while (k < 48 && counts[f][k] && values[f][k] != v[f]) {
					++k;
				}
				if (k < 48) {
					if (!counts[f][k]) {
						values[f][k] = v[f];
						++distinct[f];
					}
					++counts[f][k];
				}
			}
			const bool down = a_from[2] > a_to[2] + 1.0f && std::fabs(a_from[0] - a_to[0]) < 0.01f;
			if (down && samplesLogged < 60 && (hits % 97) == 0) {
				++samplesLogged;
				LC_LOG("DebugMaterials: down-probe hit at GTA %.2f %.2f %.2f normal %.2f %.2f %.2f: +04 %08X +08 %08X +0C %08X +48 %08X +4C %08X +50 %08X inst %p",
					a_res.m_vEndPosition.x, a_res.m_vEndPosition.y, a_res.m_vEndPosition.z, a_res.m_vUnk.x, a_res.m_vUnk.y, a_res.m_vUnk.z, v[0], v[1], v[2],
					v[3], v[4], v[5], static_cast<const void*>(a_res.m_pInst));
			}
			if (++hits % 20000 == 0) {
				for (int f = 0; f < kFields; ++f) {
					char line[1024];
					int  n = std::snprintf(line, sizeof line, "DebugMaterials: %u hits, field +%02X: %u distinct values:", hits, kOffsets[f], distinct[f]);
					for (int k = 0; k < 48 && counts[f][k] && n < 900; ++k) {
						n += std::snprintf(line + n, sizeof line - n, " %08X x%u", values[f][k], counts[f][k]);
					}
					LC_LOG("%s", line);
				}
			}
		}
	};
	inline MaterialProbe materialProbe;  // game thread only

	// One-sided probe in Minecraft space (Geometry.h's ray callback).
	inline bool CastMc(const float a_from[3], const float a_to[3], Hit& a_out)
	{
		// mc (x, y, z) -> gta (x, -z, y)
		const float         from[3] = { a_from[0], -a_from[2], a_from[1] };
		const float         to[3] = { a_to[0], -a_to[2], a_to[1] };
		tLineOfSightResults res;
		if (!CastGta(from, to, res)) {
			return false;
		}
		++rayCounters.hits;
		HitToMc(from, to, res, a_out);
		if (Config::Get().debugMaterials) {
			materialProbe.Sample(res, from, to);
		}
		return true;
	}
}
