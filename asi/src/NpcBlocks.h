// Minecraft's blocks are solid for GTA IV's peds and vehicles (port of SkyCraft's NpcBlocks).
//
// Minecraft sends, after each section mesh, which of the section's blocks NPCs collide with
// (proto::kRenSolids). The render ring is drained on the render thread (render/World.cpp), which
// hands those messages here; the game thread applies them at the start of its next Tick.
//
// Each frame (game thread, from Combat::Tick), with LibertyCraft.ini NpcBlocks=1:
//  - peds on foot near the player that overlap a solid block column (seen from above, between a
//    little above their feet and their head) are put back out of it; a ped walking into a wall
//    follows it toward its nearer end instead of walking on the spot. Niko too in Niko mode (not
//    while puppeted: Minecraft owns the player then).
//  - cars and bikes that touch blocks (or would within the next frame) lose the part of their
//    velocity that goes into them, and are moved back out if they got in: a wall of blocks across
//    a road stops traffic.
// Everything is in combat/BlockPush.h (unit-tested); this file only reads and moves the game's
// peds and vehicles.
#pragma once

#include <cstdint>

namespace lc::Combat
{
	struct Frame;
}

namespace lc::NpcBlocks
{
	// Render thread: a kRenSolids message (RenSolids + 512-byte bitset), or Minecraft cleared its
	// world (kRenClearAll).
	void OnSolids(const std::uint8_t* a_data, std::uint32_t a_bytes);
	void Clear();

	// Game thread, once per frame (Combat::Tick).
	void Tick(const Combat::Frame& a_frame);

	// Game thread: a vehicle model's bounding box in the vehicle's own frame (x right, y forward,
	// z up, metres; GET_MODEL_DIMENSIONS, cached per model index). False if it looks implausible.
	bool ModelBox(int a_vehicle, std::int32_t a_modelIndex, float a_min[3], float a_max[3]);
}
