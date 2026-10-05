// The player's Minecraft body on Niko's skeleton (MinecraftBody=1): while GTA IV animates Niko
// itself (getting into, driving and getting out of vehicles, cutscenes, Niko mode if
// MinecraftBodyNikoMode=1), Niko is hidden and Minecraft's standing body (kRenRagdoll) is drawn
// with each part on his bones (render/Body.h has the maths, render/World.cpp draws it).
//
// HostDrive::Tick decides when (it knows why GTA drives) and hides the ped; Render::Capture
// (drawingEvent, when this frame's animation is final) reads the bones and puts the posed parts
// into the frame snapshot. Game thread only.
#pragma once

#include "DriveLogic.h"
#include "render/Frame.h"

namespace lc::NikoBody
{
	// HostDrive::Tick: the ped whose animation the body follows (0: no body this frame). a_why is
	// why GTA drives; a_player the player's ped.
	int Target(int a_player, drive::Why a_why, bool a_hostDrives, bool a_mcInWorld);
	// HostDrive::Tick: the player ped while he dies (GTA's wasted flow: the death, the fade, until he is
	// back at the hospital) or is being arrested (busted), when the body should be on him then
	// (MinecraftBodyDeath; in Niko mode only with MinecraftBodyNikoMode); else 0. GTA no longer
	// "plays" a dying player (GET_PLAYER_CHAR has no ped for him), so the ped is found here.
	int DeathScenePed(bool a_minecraftMode);
	// The body is on him for a death scene this frame (GTA counts its wasted flow as no player in play,
	// which the renderer takes for a loading screen: it draws the blocks and the body anyway).
	bool DeathSceneShown();
	// Hides (or shows again) a ped under the body, the configured way (MinecraftBodyHide).
	void Hide(int a_ped, bool a_hide);
	// Render::Capture: the target's bones this frame -> a_f.bodyParts / bodyOrigin / kFrameBody.
	// False: no body this frame (none wanted, or the bones couldn't be read).
	bool Capture(render::FrameSnapshot& a_f);
	// Game::OnIngameStartup: a save is loading (the peds go away).
	void OnIngameStartup();
	// Game::Camera, while GTA drives the player: the DebugBodyView test hook's camera (the final
	// cam's 4 x 4 rows right, forward, up, position). True: it wrote one.
	bool DebugCamera(float* a_m);
}
