// The room GTA IV renders from, kept with Minecraft's camera at interiors.
//
// GTA IV draws an interior's rooms (and the outside only through their portals: doors, windows) from
// the room its game viewport's portal tracker is in, and the outside world from outside. The tracker
// follows the viewport's camera through the portals it crosses. Minecraft's camera isn't GTA's: in
// third person it follows the player through the wall beside a door, so the tracker stayed in the
// stairwell with the camera out in the street, and GTA drew only the stairwell: the building hung in
// a void. Each frame (Game::Tick) the camera's place is checked against the tracker's room and the
// tracker is put right when they disagree (ViewportRoom.cpp).
//
// Game thread (processScriptsEvent: natives).
#pragma once

namespace lc::ViewportRoom
{
	// Game::Tick, every frame: a_cam the camera's position this frame (GTA metres), a_ped the player's
	// ped. a_ours: Minecraft's camera (puppet mode); else GTA's own (checked for a moment after puppet
	// mode ends: its camera jumps back to Niko). a_detached: the camera isn't at the player's eye
	// (third person), so it can be somewhere the portals didn't take it.
	void Tick(const float a_cam[3], int a_ped, bool a_ours, bool a_detached, float a_dt);
	// Game::OnIngameStartup.
	void OnIngameStartup();
}
