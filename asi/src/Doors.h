// GTA IV's doors for the Minecraft player.
//
// While Minecraft drives Niko he is frozen with collision off, so nothing he does pushes a door
// and GTA's doors would never open for him (collision leaves doors out, so he could walk through
// them while they stayed shut). Each frame this finds the doors near the player (object pool,
// collision/Objects.h's door shape test) and swings a door open, away from the player, when they
// walk into it: within ~1.2 m of its leaf and moving towards it, or standing in the doorway.
// Opening holds the door open through GTA's door state (SET_STATE_OF_CLOSEST_DOOR_OF_TYPE, locked
// at a fully open ratio); once the player is well clear it is shut and handed back to GTA
// (unlocked, as it was). A door GTA itself keeps locked shut (a mission door) is left alone.
// If the state native doesn't move a door, it is pushed open with an impulse instead.
// Unity-built into dllmain.cpp (needs IV-SDK).
#pragma once

namespace lc::Doors
{
	struct Frame
	{
		int   ped = 0;              // the player's ped (0: none)
		bool  loading = false;      // loading / faded out: no natives, forget everything
		bool  puppeting = false;    // Minecraft drives the player: doors open for them
		float feet[3]{};            // the player's feet, GTA space
		float dt = 0.0f;
	};

	// Game thread, once a frame (Game::Tick).
	void Tick(const Frame& a_frame);
}
