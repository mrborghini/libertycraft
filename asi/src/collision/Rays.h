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

#include "Sdk.h"
#include "collision/Geometry.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace lc::col
{
	// Static map collision only: no peds, vehicles or (movable) objects such as doors and bins.
	inline constexpr std::uint32_t kLosFlags = STATIC_COLLISION | BUILDINGS;
	// 0: no see-through / shoot-through exemptions (glass and fences count as solid).
	inline constexpr std::uint32_t kLosSeeShoot = 0;

	struct RayCounters
	{
		std::uint32_t rays = 0, hits = 0, inconsistent = 0;
	};
	inline RayCounters rayCounters;  // game thread only

	// Raw probe in GTA space.
	inline bool CastGta(const float a_from[3], const float a_to[3], tLineOfSightResults& a_res)
	{
		CVector from{ a_from[0], a_from[1], a_from[2] };
		CVector to{ a_to[0], a_to[1], a_to[2] };
		std::memset(&a_res, 0, sizeof(a_res));
		++rayCounters.rays;
		return CWorld::ProcessLineOfSight(&from, &to, nullptr, &a_res, kLosFlags, 1, 0, kLosSeeShoot, 4);
	}

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
		const float* p = &res.m_vEndPosition.x;
		const float* n = &res.m_vUnk.x;
		const float  dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
		const float  len = std::sqrt(dx * dx + dy * dy + dz * dz);
		const float  px = p[0] - from[0], py = p[1] - from[1], pz = p[2] - from[2];
		const float  dist = std::sqrt(px * px + py * py + pz * pz);
		if (!std::isfinite(p[0] + p[1] + p[2] + n[0] + n[1] + n[2]) || std::fabs(res.m_fUnk1 * len - dist) > 0.05f + 0.001f * len) {
			++rayCounters.inconsistent;
			const float nan = std::numeric_limits<float>::quiet_NaN();
			a_out.pos[0] = a_out.pos[1] = a_out.pos[2] = nan;
			a_out.n[0] = a_out.n[1] = 0.0f;
			a_out.n[2] = 1.0f;
			return true;
		}
		// gta (x, y, z) -> mc (x, z, -y)
		a_out.pos[0] = p[0];
		a_out.pos[1] = p[2];
		a_out.pos[2] = -p[1];
		a_out.n[0] = n[0];
		a_out.n[1] = n[2];
		a_out.n[2] = -n[1];
		return true;
	}
}
