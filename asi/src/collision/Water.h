// GTA IV's water (the rivers, the harbour, the sea) around the player as protocol WaterGrid, so
// Minecraft swims, floats and drowns in it (HostWater.java). SDK-dependent: included by
// Collision.cpp, called from Collision::Update (game thread) every third harvest frame.
#pragma once

#include "Sdk.h"
#include "Coords.h"
#include "Link.h"
#include "collision/Rays.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lc::col
{
	struct WaterStats
	{
		std::uint32_t writes = 0, wetCells = 0, suppressed = 0;
	};
	inline WaterStats waterStats;

	// 16x16 block columns centred on a_centerMc: GET_WATER_HEIGHT_NO_WAVES at each column's centre
	// (false where no water quad covers it). Water above the player's feet that the player is
	// sealed off from (a tunnel under the river: something's underside between the feet and the
	// surface) is not reported, or Minecraft would drown the player in the tunnel.
	inline void WriteWater(const McVec& a_centerMc)
	{
		constexpr int     kSize = static_cast<int>(proto::kWaterGridSize);
		proto::WaterGrid  grid{};
		grid.originX = static_cast<std::int32_t>(std::floor(a_centerMc.x)) - kSize / 2;
		grid.originZ = static_cast<std::int32_t>(std::floor(a_centerMc.z)) - kSize / 2;
		grid.worldId = 0;
		const float probeZ = static_cast<float>(a_centerMc.y) + 2.0f;  // GTA z == MC y
		float       highest = -1e30f;
		int         wet = 0;
		for (int dz = 0; dz < kSize; ++dz) {
			for (int dx = 0; dx < kSize; ++dx) {
				const double mx = grid.originX + dx + 0.5, mz = grid.originZ + dz + 0.5;
				float        h = 0.0f;
				const bool   water = ::Scripting::GET_WATER_HEIGHT_NO_WAVES(static_cast<float>(mx), static_cast<float>(-mz), probeZ, &h);
				const bool   ok = water && std::isfinite(h) && h > -1000.0f && h < 2000.0f;
				grid.surface[dz * kSize + dx] = ok ? h : proto::kNoWater;
				if (ok) {
					highest = std::max(highest, h);
					++wet;
				}
			}
		}
		if (wet && highest > static_cast<float>(a_centerMc.y) + 0.3f) {
			// Water above the feet: only if nothing covers the player below its surface.
			const float from[3] = { static_cast<float>(a_centerMc.x), static_cast<float>(a_centerMc.y) + 0.5f, static_cast<float>(a_centerMc.z) };
			const float to[3] = { from[0], highest, from[2] };
			Hit         h{};
			if (to[1] > from[1] && CastMc(from, to, h)) {
				for (auto& s : grid.surface) {
					s = proto::kNoWater;
				}
				++waterStats.suppressed;
				wet = 0;
			}
		}
		waterStats.wetCells += static_cast<std::uint32_t>(wet);
		++waterStats.writes;
		Link::Get().WriteWaterGrid(grid);
	}
}
