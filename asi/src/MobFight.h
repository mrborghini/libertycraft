// GTA IV's peds and Minecraft's mobs (PedsFightMobs).
//
// Minecraft's hostile mobs hunt GTA's peds on foot as they hunt the player (fabric combat/PedTargets.java);
// their blows, arrows and blasts reach the ped through its stand-in like the player's do, flagged
// proto::kHitByMob, and Combat applies them as usual but hands the ped's reaction here instead of
// treating the hit as the player's crime.
//
// Minecraft also tells us every 5 ticks where the hostile mobs near the player are and which ped each
// one is after (proto::kEvMob, combat/HostMobs.java). Every 0.25 s (game thread):
//  - the ped a mob is after, and police within 25 m of such a mob (3 at most), deal with it: armed
//    peds and police (a cop without a gun is given a pistol) shoot at the mob's chest, re-aimed every
//    0.8 s as it moves (TASK_SHOOT_AT_COORD; DebugMobShoot=1: TASK_AIM_GUN_AT_COORD and
//    FIRE_PED_WEAPON); the rest run from it (TASK_SMART_FLEE_POINT) while it is within 15 m;
//  - a ped a mob hurt reacts the same way (a gun: it shoots back; none: it runs).
// GTA IV's bullets hit the mobs: GTA's bullet trace (NpcBlocks' hook) asks ClipShot first, which ends
// the shot at the first mob box on its way (in front of Minecraft's blocks); if GTA finds nothing in
// front of the mob either, the mob took it, and Tick sends proto::kInMobHit with the shooter (the
// nearest ped to the muzzle, or the player) and the weapon's damage / kMobDamageScale (a pistol's 25:
// 5 Minecraft damage, so four or five shots kill a zombie). Minecraft hurts the mob as from the
// shooter's stand-in, so it turns on the shooter.
#pragma once

#include "LinkCore.h"

namespace lc::Combat
{
	struct Frame;
}

namespace lc::MobFight
{
	namespace proto = ::libertycraft::proto;

	// Game thread: a kEvMob event from Minecraft.
	void OnMob(const proto::McEvent& a_ev);

	// Game thread: a Minecraft mob just hurt (or killed) a_ped; a_vehicle: the ped sits at the wheel of
	// it (0: on foot). a_pushX/Y: the way the hit pushed the ped (GTA, unit; 0 0 if unknown): the mob is
	// the other way.
	void Attacked(int a_ped, bool a_killed, int a_vehicle, float a_pushX, float a_pushY);

	// Any thread (GTA's bullet trace): the first mob box the shot a_from -> a_to passes through; a_to is
	// moved to where it enters it. 0: none (a_to unchanged).
	int ClipShot(const float a_from[3], float a_to[3]);
	// Any thread: GTA traced the shot ClipShot ended at mob a_mob; a_hits: what GTA hit in front of it
	// (0: the mob took the bullet).
	void ShotDone(int a_mob, const float a_from[3], const float a_at[3], int a_hits);

	// Game thread, once per frame (Combat::Tick): reactions, bullet hits, the DebugMobFight test hook.
	void Tick(const Combat::Frame& a_frame);

	// A new game session or a load: forget every mob and ped.
	void Reset();
}
