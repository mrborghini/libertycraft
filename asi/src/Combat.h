// Combat between Minecraft and GTA IV (port of SkyCraft's Combat). Minecraft's side is
// fabric/.../combat/HostCombat.java: it mirrors every actor in our ActorTable as an invisible
// hittable stand-in and sends back what hit them (kEvHitActor), its explosions (kEvExplosion) and
// the player's death (kEvPlayerDied); we send GTA IV's damage to the player as kInHurt.
//
// Each frame (Game::Tick, game thread, natives allowed), with LibertyCraft.ini Combat=1:
//  - ActorTable: GTA IV peds on foot within 80 m of the player (not the player), feet position,
//    yaw, 0.6 x 1.8 blocks, health fraction, hostile/in-combat/dead flags. Written whenever
//    Minecraft is in a world and the game is in play, puppeted or not (in a car too).
//  - kEvHitActor: MC damage * PedDamageScale off the ped's health (DAMAGE_CHAR, falling back to
//    SET_CHAR_HEALTH), plus a ragdoll pushed along Minecraft's knockback (RagdollOnHit).
//  - GtaCrimes=1: GTA's crime system never sees Minecraft's hits (no attacker), so we act for it: the
//    victim fights back or runs (a driver drives off), and police or bystanders seeing it give the
//    player a wanted level (combat/CombatMath.h WantedAfterAttack).
//  - kEvExplosion: ADD_EXPLOSION(ExplosionType) at the blast, radius * ExplosionRadiusScale; GTA's
//    blast knocks peds and cars about. A puppeted player is explosion-proof for a moment around
//    it (Minecraft already hurt its player).
//  - kEvPlayerDied: kills the player ped, so GTA's "wasted" flow runs; Game leaves puppet mode
//    and resyncs Minecraft after the respawn (the game moves the player).
//  - While puppeting: the player ped is vulnerable but sits on a 1000-health buffer (the cap,
//    CPlayerInfo::m_nMaxHealth, raised before every refill) refilled every frame, so GTA IV can't
//    kill it; what it lost (health + armour) goes to Minecraft as kInHurt, divided by
//    PlayerDamageScale and paced to Minecraft's 0.5 s hurt cooldown (falls and drowning are
//    Minecraft's). If the buffer can't be held, the ped turns invincible instead. Leaving puppet
//    mode (for longer than 0.5 s) puts back the max health, health and armour it had before.
// With Combat=0, events are drained and logged only, and Game keeps the puppeted ped invincible.
#pragma once

#include "LinkCore.h"

#include <cstdint>

namespace lc::Combat
{
	namespace proto = ::libertycraft::proto;

	struct Frame
	{
		int                   player = 0;   // Player index (GET_PLAYER_ID)
		int                   ped = 0;      // the player's ped handle, 0 if none
		bool                  exists = false;
		bool                  loading = true;  // screen faded / no ped: the world isn't playable
		bool                  dead = false;
		bool                  puppeting = false;
		bool                  mcInWorld = false;
		const proto::McState* mc = nullptr;  // this frame's McState, null if it couldn't be read
		float                 dt = 0.0f;
	};

	// Once per frame from Game::Tick, after the puppet decision. Returns how many Minecraft events
	// it took from the event ring.
	std::uint32_t Tick(const Frame& a_frame);

	// Combat owns the puppeted ped's health (Combat=1): Game must leave it vulnerable.
	bool OwnsPlayerHealth();

	// A save / new game is loading: every ped handle we hold is about to be invalid.
	void OnIngameStartup();
}
