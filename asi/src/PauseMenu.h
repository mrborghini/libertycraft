// GTA IV's pause menu and LibertyCraft (PauseMenu.cpp).
//
// Test hooks: DebugPauseMenu opens and closes GTA's pause menu with real Esc key events (the same
// path as the keyboard), from a thread of its own (the game's script tick doesn't run on every
// paused frame), and logs the frontend's state (its screen, the "frontend" texture dictionary the
// menu draws its map with) and the address space around each open and close. DebugDriveWander
// lets GTA's AI drive the player's car through the city (heavy streaming, like a player's drive).
#pragma once

namespace lc::PauseMenu
{
	struct Frame
	{
		int   ped = 0;
		bool  exists = false;
		bool  inCar = false;
		bool  paused = false;
		bool  loading = false;
		float dt = 0.0f;
	};

	// Game thread (Game::Tick), every frame, also while GTA's menu is open.
	void Tick(const Frame& a_f);
}
