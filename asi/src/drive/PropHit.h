// The Minecraft player against GTA IV's street furniture and walls at speed: pure maths, no IV-SDK.
// PropSmash.cpp feeds it; asi/tests/drive_test.cpp tests it.
//
// A mover (Minecraft's player sprinting or in elytra flight, or the mount it rides: kEvMover) is an
// upright box (MC space: x, y up, z) of a width and a height, moving along a velocity. A prop is the
// oriented boxes Collision probed it as (collision/Objects.h OBox: a rectangle in the object's heading
// frame on MC x/z, and a height range). The host looks a little ahead along the mover's path: a prop it
// will run into fast enough leaves Minecraft's collision first (so Minecraft doesn't stop the mover at
// it), and is knocked over when the box reaches it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace lc::drive::prop
{
	// collision/Objects.h OBox, field for field.
	struct Box
	{
		float o[2], u[2], v[2];
		float a0, a1, b0, b1, y0, y1;
	};

	struct Mover
	{
		float bottom[3]{};   // the box's bottom centre (MC)
		float vel[3]{};      // m/s (MC)
		float halfWidth = 0.3f, height = 1.8f;
	};

	// Does the mover's box, its bottom centre at a_at, overlap a_b grown by a_margin? (The box's
	// footprint as a circle through its corners would reach too far diagonally: as the inscribed one, a
	// little grown.)
	inline bool Overlaps(const Box& a_b, const float a_at[3], float a_halfWidth, float a_height, float a_margin)
	{
		if (a_at[1] + a_height < a_b.y0 - a_margin || a_at[1] > a_b.y1 + a_margin) {
			return false;
		}
		const float dx = a_at[0] - a_b.o[0], dz = a_at[2] - a_b.o[1];
		const float a = dx * a_b.u[0] + dz * a_b.u[1], b = dx * a_b.v[0] + dz * a_b.v[1];
		const float ca = std::clamp(a, a_b.a0, a_b.a1), cb = std::clamp(b, a_b.b0, a_b.b1);
		const float r = a_halfWidth * 1.1f + a_margin;
		return (a - ca) * (a - ca) + (b - cb) * (b - cb) <= r * r;
	}

	// The first time (s, from now) within a_horizon the mover runs into any of a_boxes, or -1. Steps of
	// at most 0.1 m along its path.
	inline float FirstContact(const Mover& a_m, const Box* a_boxes, std::size_t a_count, float a_horizon, float a_margin)
	{
		const float speed = std::sqrt(a_m.vel[0] * a_m.vel[0] + a_m.vel[1] * a_m.vel[1] + a_m.vel[2] * a_m.vel[2]);
		if (!(speed > 1e-3f) || a_count == 0) {
			for (std::size_t i = 0; i < a_count; ++i) {
				if (Overlaps(a_boxes[i], a_m.bottom, a_m.halfWidth, a_m.height, a_margin)) {
					return 0.0f;
				}
			}
			return -1.0f;
		}
		const int steps = std::clamp(static_cast<int>(std::ceil(speed * a_horizon / 0.1f)), 1, 400);
		for (int s = 0; s <= steps; ++s) {
			const float t = a_horizon * static_cast<float>(s) / static_cast<float>(steps);
			const float at[3] = { a_m.bottom[0] + a_m.vel[0] * t, a_m.bottom[1] + a_m.vel[1] * t, a_m.bottom[2] + a_m.vel[2] * t };
			for (std::size_t i = 0; i < a_count; ++i) {
				if (Overlaps(a_boxes[i], at, a_m.halfWidth, a_m.height, a_margin)) {
					return t;
				}
			}
		}
		return -1.0f;
	}

	// How fast a mover must be going to knock a prop over (m/s): light things (bins, cones, small signs,
	// under kLightMass kg) give way to a sprint (5.6 m/s), poles and the like (traffic lights, lamp posts,
	// big signs) to elytra flight or a galloping horse (a horse at full gallop: about 14 m/s), not to a
	// sprint or a walking horse; past kMaxMass kg nothing does (a bus shelter, a kiosk).
	inline constexpr float kLightMass = 120.0f, kMaxMass = 4000.0f;
	inline constexpr float kLightSpeed = 5.0f, kHeavySpeed = 9.0f;
	inline float BreakSpeed(float a_propMass)
	{
		if (!(a_propMass > 0.0f) || a_propMass > kMaxMass) {
			return 1e9f;
		}
		return a_propMass < kLightMass ? kLightSpeed : kHeavySpeed;
	}

	// How much of its speed the mover keeps going through (a share of its momentum goes into the prop):
	// a player (about 80 kg) loses about half to a traffic light, a horse (about 600 kg with its rider)
	// little; never less than kMinKeep through something that broke.
	inline constexpr float kPlayerMass = 80.0f, kMountMass = 600.0f, kMinKeep = 0.2f;
	inline float Keep(float a_moverMass, float a_propMass)
	{
		const float m = std::max(a_moverMass, 1.0f), p = std::clamp(a_propMass, 0.0f, kMaxMass);
		return std::clamp(m / (m + 0.25f * p), kMinKeep, 1.0f);
	}

	// What the prop is sent off at (m/s, along the mover's way): the momentum it took.
	inline float PropSpeed(float a_moverMass, float a_propMass, float a_speed, float a_keep)
	{
		return std::clamp(a_moverMass * a_speed * (1.0f - a_keep) / std::max(a_propMass, 1.0f), 1.0f, 12.0f);
	}

	// ---- elytra crashes (kEvImpact) -----------------------------------------------------------------
	// A crash this hard (m/s, Minecraft's numbers: the speed a wall took away, or into the ground)
	// knocks the player over. Minecraft's own flying-into-a-wall damage starts at 6 m/s.
	inline constexpr float kCrashWall = 8.0f, kCrashGround = 10.0f;

	struct Crash
	{
		bool  knock = false;
		float dir[2]{};        // GTA x/y (unit): which way the ragdoll goes
		float throwSpeed = 0.0f;  // m/s
		int   ms = 0;
	};

	// a_wall: into a wall (bounce back off it) or onto the ground (tumble on along the flight).
	// a_dirMc: the horizontal flight direction (MC x/z); a_horizontal: horizontal speed before (m/s).
	inline Crash CrashOf(bool a_wall, float a_speed, const float a_dirMc[2], float a_horizontal)
	{
		Crash c;
		if (!(a_speed >= (a_wall ? kCrashWall : kCrashGround))) {
			return c;
		}
		c.knock = true;
		const float gx = a_dirMc[0], gy = -a_dirMc[1];  // MC (x, z) -> GTA (x, -z)
		const float len = std::hypot(gx, gy);
		const float s = a_wall ? -1.0f : 1.0f;
		c.dir[0] = len > 1e-3f ? s * gx / len : 0.0f;
		c.dir[1] = len > 1e-3f ? s * gy / len : 0.0f;
		c.throwSpeed = a_wall ? std::clamp(0.12f * a_speed, 1.0f, 4.0f) : std::clamp(0.6f * a_horizontal, 2.0f, 20.0f);
		c.ms = static_cast<int>(std::clamp(1500.0f + 100.0f * a_speed, 1500.0f, 5000.0f));
		return c;
	}
}
