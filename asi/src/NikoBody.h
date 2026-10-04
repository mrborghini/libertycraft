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
