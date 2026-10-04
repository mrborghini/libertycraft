// Collision for Minecraft, v2: GTA IV's static collision sampled with line probes.
//
// Region scheduling is SkyCraft's: 8x8x8-block regions within 5 regions horizontally, 3 below and
// 2 above the player, nearest first, everything tagged with the collision epoch (kColClear{epoch}
// on a game load or a Minecraft reconnect; an interior change re-probes the columns around the
// player instead, Refresh). Per 8x8-block column of regions, the game thread
// probes the world with CWorld::ProcessLineOfSight (collision/Rays.h) within ~2.5 ms a frame,
// resumable across frames (collision/Geometry.h: vertical chains every half block, horizontal
// wall probes per sample edge and floor); a worker thread turns a finished column into kColTris
// (floor, ceiling and wall triangles for the player's smooth collider) and kColRegion (1/8-block
// voxels for mobs and items) per region and writes them to the collision ring, waiting while it
// is full. Far columns get REQUEST_COLLISION_AT_POSN first; a column with no collision at all
// (not streamed in yet) is retried, not sent empty. The 3x3 columns around the player are
// re-checked every second with a few probes and re-probed when they changed (late streaming).
// Also writes the water grid (collision/Water.h).
//
// Street furniture (lamp posts, bins, benches...: GTA objects, not map collision) comes from a
// second pass over the 5x5 columns around the player (collision/Objects.h): every 8 frames the
// object pool is scanned; each object worth colliding with (not a door, not tiny, not attached or
// a vehicle part) is probed once, with OBJECTS-only probes that skip everything but it, once it
// has kept still for two scans, and becomes oriented boxes. A column's objects go into its
// regions' kColTris / kColRegion with the map's; when they change (an object streamed in or out,
// was knocked over, the player stands inside one) the regions they touch are sent again. Objects
// that moved are not solid until they keep still and have been probed again. Object probes share
// the 2.5 ms a frame (0.8 ms of it is theirs while columns are busy) and are logged separately.
#pragma once

#include "Coords.h"

#include <cstdint>

namespace lc
{
	class Collision
	{
	public:
		static constexpr int kRegionSize = 8;  // blocks per region edge (must match Java)

		static Collision& Get();
		void Start();  // worker thread
		// Game load / reconnect: drops everything harvested, queues kColClear{epoch}.
		void Reset(std::uint32_t a_epoch);
		// The player went into or out of an interior (same coordinates): the columns around him (7 x 7)
		// are probed again, nearest first, and their regions are sent again where they changed; until
		// then Minecraft keeps what it has (nothing is cleared, no new epoch: the floor under the player
		// never goes away).
		void Refresh();
		// Game thread, once a frame: probe and queue what's due around a_centerMc (the player's
		// feet, MC space). a_feetGtaZ: the player's feet height in GTA space.
		void Update(const McVec& a_centerMc, float a_feetGtaZ);

		struct Counters
		{
			std::uint32_t regions, tris, blocks, columns, columnFailures, collisionRequests, ringWaits, dropped, clears;
			std::uint32_t queued;
			std::uint64_t ringPending;
		};
		Counters TakeCounters();

	private:
		void WorkerLoop();
	};
}
