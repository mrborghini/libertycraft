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
// Columns more than 7 regions away are forgotten (every 2 s once over 225 are held) and Minecraft
// is told so (kColForget, through the worker after the column's queued regions), so it can drop
// them too instead of keeping every region of a long drive.
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

#include <cstddef>
#include <cstdint>
#include <functional>

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

		// Street furniture (collision/Objects.h), for PropSmash. Game thread only.
		struct ObjectView
		{
			const void*   key;       // the CObject
			std::int32_t  model;
			std::uint32_t hash;      // its model's hash (logging)
			const void*   boxes;     // col::OBox[count]: its probed shape, MC space
			std::size_t   count;
			bool          passable;  // left out of Minecraft's collision (SetObjectPassable)
		};
		// Each tracked object Minecraft collides with (solid, probed, keeping still).
		void ForEachSolidObject(const std::function<void(const ObjectView&)>& a_visit) const;
		// Leaves an object out of the collision Minecraft gets (or puts it back), at once: the regions it
		// touches are sent again. It goes back by itself once it has moved (knocked over) and keeps still.
		bool SetObjectPassable(const void* a_key, bool a_passable);

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
