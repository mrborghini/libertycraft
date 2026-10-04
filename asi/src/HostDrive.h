// Vehicles and Niko mode: when GTA IV itself drives the player instead of Minecraft.
//
// ToggleKey (Backslash) switches Minecraft mode <-> Niko mode (plain GTA IV: no puppet, no input
// for Minecraft, GTA's HUD; Minecraft's player follows Niko). VehicleKey (F), taken from Minecraft
// while puppeting, gives Niko back to GTA IV and presses GTA's own enter-vehicle control, so the game
// picks the door or carjacks like normal. In a vehicle GTA IV drives (its own F gets out) and
// Minecraft's player rides a mount at the seat (kSkyInVehicle). The decisions are DriveLogic.h's.
//
// Threads: Tick on the game thread (processScriptsEvent, natives allowed), OnKey on the window
// thread, Pad in processPadEvent.
#pragma once

#include "Coords.h"

#include <cstdint>

class CPad;  // IV-SDK

namespace lc::HostDrive
{
	struct Frame
	{
		int   player = 0, ped = 0;
		bool  exists = false, loading = false, paused = false, dead = false;
		bool  inCar = false, cutscene = false;
		bool  scripted = false;   // a mission script has the player on foot (Missions.h): GTA drives him
		bool  puppeting = false;  // before this frame's puppet decision
		bool  mcInWorld = false;
		bool  haveMc = false;      // this frame's McState was read (mcCreative is current)
		bool  mcCreative = false;  // the Minecraft player is in creative or spectator (kMcCreative)
		bool  resyncing = false;  // the teleport handshake runs (Minecraft hasn't arrived where the game put the player)
		float dt = 0.0f;
		float heading = 0.0f;  // the ped's heading (GTA degrees)
	};

	struct Result
	{
		const char* blocker = nullptr;  // GTA drives: no puppet (nullptr: Minecraft may)
		bool        hostDrives = false;
		bool        inVehicle = false;
		bool        resync = false;      // GTA just let go: teleport handshake first
		GtaVec      seatFeet{};          // inVehicle: where the riding Minecraft player's feet go
		float       heading = 0.0f;      // GTA degrees: the vehicle's (inVehicle) or the ped's
	};

	// Game::Tick, before the teleport handshake and the puppet decision.
	Result Tick(const Frame& a_frame);
	// Game::OnIngameStartup.
	void OnIngameStartup();
	// The window procedure, for every key message. True: consumed (the toggle key always; the
	// vehicle key while puppeting, so Minecraft never sees it).
	bool OnKey(std::uint32_t a_dik, bool a_down, bool a_repeat);
	// processPadEvent with the local player's pad, after the game's pad update: presses GTA's
	// enter / exit controls while an action asks for it. (GTA hands the player's ped its pad only
	// while player control is on: during puppet mode this isn't called.)
	void Pad(CPad* a_pad);
	// Test hooks: move Niko to the nearest road (out of an interior) at the next Tick while puppeting.
	void DebugRequestRoad(const char* a_who);
	// Knocks the player over (RagdollOnVehicleHit; Combat calls it when one of GTA's explosions hurt
	// the puppeted player): GTA takes Niko, ragdolls him along (a_gx, a_gy) with a_force for
	// a_ragdollMs and keeps him until he is back on his feet. Game thread.
	void KnockDown(float a_gx, float a_gy, float a_force, int a_ragdollMs, const char* a_what);
	// Game::Tick after its puppet decision: leaving puppet mode shows Niko again; the ped HostDrive
	// hides (under the Minecraft body, in a vehicle) goes straight back into hiding, so no frame shows him.
	void AfterPuppetDecision();
	// Knocked over in Minecraft mode, until Niko is back up: Minecraft still owns the player's health
	// (Game hands Combat this as part of puppet mode).
	bool KnockedOver();
	// Minecraft mode, GTA animates Niko for a vehicle (the vehicle key's walk to a door, getting in,
	// seated, bailing out, getting out and back up): Minecraft owns the player's health there too
	// (Combat). False in Niko mode.
	bool VehicleInMinecraftMode();
	// Game::Tick while puppeting, with the feet Minecraft wants: the DebugWalkThroughCar test hook
	// may move them. Returns how to place the ped: 0 as configured, 1 the native, 2 the direct move.
	int DebugPuppetTarget(GtaVec& a_feet);
}
