// GTA IV's mission scripts and Minecraft mode (ScriptScenes, MissionPedsSafe; drive/SceneLogic.h).
//
// * On-foot scenes: a script that takes the player over (switches his control off, shows its own camera,
//   gives him tasks, runs a minigame) gets him: GTA IV drives the player meanwhile (HostDrive's
//   Why::kScript: no puppet, GTA's camera, GTA's pad, the Minecraft body on Niko), and Minecraft takes him
//   back once the script has let go (SceneLogic.h).
// * What the player watches: GTA's cutscenes and the scripts' cameras pause Minecraft (kSkyScene: its mobs,
//   their blasts and its sounds stand still, GtaMenuPause.java), phone calls duck its sounds (kSkyPhoneCall).
// * Mission characters: peds a mission script owns (IS_PED_A_MISSION_PED) and the vehicles they sit in go to
//   Minecraft as kActorMission (its mobs leave them alone), and no mob's hit or blast reaches them (Combat).
//
// Game thread only (natives).
#pragma once

#include <cstdint>

namespace lc::Missions
{
	struct Frame
	{
		int   player = 0, ped = 0;
		bool  exists = false, loading = false, paused = false, dead = false, inCar = false, cutscene = false;
		bool  puppeting = false;     // before this frame's decision
		bool  minecraftMode = true;  // not Niko mode (HostDrive's toggle)
		bool  mcInWorld = false;
		int   ownScriptCam = 0;      // our own CameraMode=scripted camera (not a mission's)
		float dt = 0.0f;
	};

	struct State
	{
		bool        scripted = false;   // GTA drives the player for a mission script (HostDrive Why::kScript)
		const char* why = nullptr;      // what holds it (for the log)
		bool        scene = false;      // a cutscene or a script's camera is shown: Minecraft pauses (kSkyScene)
		bool        phoneCall = false;  // a phone call: Minecraft's sounds duck (kSkyPhoneCall)
	};

	// Game::Tick, before HostDrive::Tick.
	State Tick(const Frame& a_frame);
	// The last Tick's verdict.
	const State& Current();
	// Game::OnIngameStartup.
	void OnIngameStartup();
	// A ped a mission script owns (IS_PED_A_MISSION_PED; MissionPedsSafe=1, else always false). Memoised
	// per frame.
	bool IsMissionPed(int a_ped);
	// A vehicle a mission character sits in (driver or passenger), or a mission script's own vehicle
	// with someone in it (IS_CAR_A_MISSION_CAR). Memoised per frame.
	bool IsMissionVehicle(int a_vehicle);
	// Combat, before it sets off one of Minecraft's mobs' blasts at a_x a_y a_z: mission characters
	// within a_reach metres are explosion-proof for a moment (their own proofs come back after).
	void ShieldFromMobBlast(float a_x, float a_y, float a_z, float a_reach);
}
