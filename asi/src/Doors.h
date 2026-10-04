// GTA IV's doors for the Minecraft player.
//
// While Minecraft drives Niko he is frozen with collision off, so nothing he does pushes a door
// and GTA's doors would never open for him (collision leaves doors out, so he could walk through
// them while they stayed shut). This pushes them the way Niko's body would: each frame the
// player (a 0.35 m circle, seen from above) is checked against every door leaf near them
// (object pool, collision/Objects.h's door shape test). Where the leaf cuts into the player it is
// turned about its hinge just far enough to clear them (PushRotation below), on the side it was
// on before they touched it: a door swings away from whoever walks into it, gradually, as far as
// they walk it open, and faster when they run. Once nothing pushes it, GTA's own hinge spring
// swings it back shut.
//
// How the turn is applied is found out in game, per session (the first that moves a door wins):
//  1. the door's angular velocity (SET_OBJECT_INITIAL_ROTATION_VELOCITY), GTA's physics;
//  2. a push on the leaf (APPLY_FORCE_TO_OBJECT at the contact point), GTA's physics;
//  3. GTA's door state, locked at the matching open ratio every frame
//     (SET_STATE_OF_CLOSEST_DOOR_OF_TYPE), unlocked again the moment the push ends.
// A door GTA itself keeps locked (a mission door) is left alone.
//
// Doors are remembered by pool handle and found again every frame; a door whose object is gone
// (streamed out, deleted, recreated) is forgotten, and no native ever runs on one.
// Unity-built into dllmain.cpp (needs IV-SDK); the geometry below is SDK-free (tests/collision_test.cpp).
#pragma once

#include <cmath>

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

	// ---- push geometry (plan view, radians, relative to the hinge) --------------------------------

	inline float WrapPi(float a_r)
	{
		constexpr float kPi = 3.14159265f;
		while (a_r > kPi) {
			a_r -= 2.0f * kPi;
		}
		while (a_r <= -kPi) {
			a_r += 2.0f * kPi;
		}
		return a_r;
	}

	// The angle (radians) the leaf must keep from the direction of a circle of radius a_r at
	// distance a_d from the hinge so it doesn't cut into it: asin(r/d) along the leaf, the angle at
	// which the tip just touches beyond it. Negative: no contact possible (at the hinge, or beyond
	// the tip's reach).
	inline float ClearAngle(float a_d, float a_len, float a_r)
	{
		if (a_d <= a_r || a_d >= a_len + a_r) {
			return -1.0f;
		}
		if (a_d * a_d <= a_len * a_len + a_r * a_r) {
			return std::asin(a_r / a_d);
		}
		const float c = (a_d * a_d + a_len * a_len - a_r * a_r) / (2.0f * a_d * a_len);
		return std::acos(c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c));
	}

	// Leaf at angle a_alpha, player (radius a_r) at (a_px, a_py) from the hinge, leaf a_len long.
	// a_side: which side of the player the leaf is on (+1 counter-clockwise of them, -1 clockwise),
	// remembered from before they touched it. Returns the turn (radians, + counter-clockwise) that
	// takes the leaf just clear of the player on that side, 0 if it doesn't cut into them.
	inline float PushRotation(float a_alpha, float a_px, float a_py, float a_len, float a_r, float a_side)
	{
		const float d = std::sqrt(a_px * a_px + a_py * a_py);
		const float a = ClearAngle(d, a_len, a_r);
		if (a < 0.0f) {
			return 0.0f;
		}
		const float sep = WrapPi(a_alpha - std::atan2(a_py, a_px));
		if (std::fabs(sep) >= a) {
			return 0.0f;  // clear of them (on either side)
		}
		return a_side * a - sep;
	}

	// Which side of the player the leaf is on (for PushRotation), while it doesn't touch them.
	inline float LeafSide(float a_alpha, float a_px, float a_py) { return WrapPi(a_alpha - std::atan2(a_py, a_px)) >= 0.0f ? 1.0f : -1.0f; }
}
