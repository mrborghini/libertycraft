// Minecraft's fire, lava, magma and water for GTA IV's peds and vehicles (port of SkyCraft's hazards).
//
// Minecraft sends every light-emitting block of each section (kRenLights) with what standing in it
// does (BlockHazard: fire, soul fire and lit campfires; lava; magma). The render ring is drained on
// the render thread (render/World.cpp), which hands those messages here; the game thread applies
// them at the start of its next Tick (hazard/HazardGrid.h, unit-tested).
//
// Every 0.1 s (game thread, from Game::Tick), for peds on foot within 80 m of the player (Niko
// too, unless Minecraft owns him: puppet mode, or knocked over): a ped whose body touches fire or
// lava is set alight (START_CHAR_FIRE: GTA's burning ped runs about screaming and takes GTA's fire
// damage) and hurt like Minecraft hurts its own (fire 2, lava 8, magma 1 Minecraft damage a second,
// x PedDamageScale); out of it, it burns on for 4 s (lava 8 s) and is put out, unless it has died.
// Magma only hurts.
//
// Every frame, vehicles within 80 m whose underside (their model box, sampled 3 x 4) touches
// Minecraft's blocks (traffic, parked cars, and the player's own while GTA drives):
//  - HazardsBurnVehicles: fire takes 250 engine health a second (below 0 GTA's engine fire starts
//    and, if it keeps burning, its explosion), magma 50; lava sets the vehicle alight at once
//    (START_CAR_FIRE, engine below 0): wrecked in seconds.
//  - LiquidsSlowVehicles: in water or lava (kRenLiquids) the vehicle struggles, more the deeper it
//    sits (hazard/HazardGrid.h Drag: only part of the engine's push takes, drag, a top speed it's
//    eased down to; about 8 m/s in 0.2 m of water, 3 in 0.5, a crawl in 0.9), through its physics
//    collider's velocity (smooth under GTA's own throttle). Over a metre of water the engine runs on
//    for 3 s, as in GTA's own water (hazard::StallStep; GTA's own drowning measured with the
//    DebugHazardInject=harbour / shore test hook), then stalls and is held off while it's that deep;
//    out of it, it starts again (GTA's own drowned car stays dead). Water never damages a vehicle;
//    only fire, lava and magma do.
//
// LiquidsSlowPeds: every 0.1 s peds on foot in Minecraft water wade slower, the deeper the slower
// (SET_CHAR_MOVE_ANIM_SPEED_MULTIPLIER, put back when they're out).
//
// MinecraftWaterIsGtaWater (experimental, off): GTA IV 1.0.8.0's water level query (0x9AB6C0,
// trampolined after a byte check) answers Minecraft's water surface where Minecraft has water.
// Measured (DebugHazards logs its callers): the camera, the player's position and some effects ask
// it; GTA's ped swimming and vehicle buoyancy don't, so peds don't swim in Minecraft water through
// it (they wade, above). The scripts' GET_WATER_HEIGHT and GET_WATER_HEIGHT_NO_WAVES keep GTA's own
// water (collision/Water.h reads it for Minecraft).
#pragma once

#include <cstdint>

namespace lc::Combat
{
	struct Frame;
}

namespace lc::Hazards
{
	// Render thread: a kRenLights message (RenLights + RenLight[count]), or Minecraft cleared its
	// world (kRenClearAll).
	void OnLights(const std::uint8_t* a_data, std::uint32_t a_bytes);
	// Render thread: a kRenLiquids message (RenLiquids + RenLiquid[count]).
	void OnLiquids(const std::uint8_t* a_data, std::uint32_t a_bytes);
	void Clear();

	// Game thread, once per frame (Game::Tick, after Combat).
	void Tick(const Combat::Frame& a_frame);
}
