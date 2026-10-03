// Collision for Minecraft, v1: a heightfield of GTA IV's ground.
//
// Port of SkyCraft's region scheduler: 8x8x8-block regions within 5 regions horizontally, 3 below
// and 2 above the player, nearest first, at most ~2.5 ms of probing a frame, near regions
// re-harvested every second, everything tagged with the collision epoch (kColClear{epoch} on a
// world change or a Minecraft reconnect). For each region's 8x8 column footprint the 9x9 grid of
// block corners is probed with GET_GROUND_Z_FOR_3D_COORD (game thread, natives); a worker thread
// turns the heights into kColTris (two outward-wound kTriTerrain|kTriDiggable triangles per
// block column) and kColRegion (1/8-block voxels at or below the surface) and writes them to the
// collision ring, waiting while it's full, exactly like SkyCraft.
//
// TODO(v2): anything above the ground (roofs over the player, bridges, interiors, props) needs
// real collision geometry (phBound / CWorld::ProcessLineOfSight). Replace HarvestColumn() (the
// only part that talks to the game); the region scheduling, voxelising and ring writing stay.
#pragma once

#include "Coords.h"

#include <cstdint>

namespace lc
{
	class Collision
	{
	public:
		static constexpr int kRegionSize = 8;  // blocks per region edge (must match Java)
		static constexpr int kCorners = kRegionSize + 1;

		// Ground heights (MC y) at the block corners of one region column:
		// h[j * kCorners + i] at MC (rx*8 + i, rz*8 + j).
		struct ColumnHeights
		{
			float h[kCorners * kCorners];
		};

		static Collision& Get();
		void Start();  // worker thread
		// World change / reconnect: drops everything harvested, queues kColClear{epoch}.
		void Reset(std::uint32_t a_epoch);
		// Game thread, once a frame: harvest what's due around a_centerMc. a_feetGtaZ is the
		// player's feet height (GTA z), for ProbeFrom=feet.
		void Update(const McVec& a_centerMc, float a_feetGtaZ);

		struct Counters
		{
			std::uint32_t regions, tris, blocks, columns, columnFailures, collisionRequests, ringWaits, dropped, clears;
			std::uint32_t queued;
			std::uint64_t ringPending;
		};
		Counters TakeCounters();

	private:
		// The game-specific part (replace for v2). False: the column isn't loaded yet.
		bool HarvestColumn(int a_rx, int a_rz, float a_feetGtaZ, ColumnHeights& a_out);
		void WorkerLoop();
	};
}
