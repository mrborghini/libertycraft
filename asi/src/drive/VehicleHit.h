// Vehicles running into the puppeted player (RagdollOnVehicleHit): pure geometry, no IV-SDK.
// HostDrive.cpp feeds it every vehicle near the player each frame; asi/tests/drive_test.cpp tests it.
//
// A vehicle is a box in its own frame (IV-SDK's CMatrix rows: right = x, forward = y, up = z): the
// model box for cars, bikes and boats, the fuselage for helicopters (measured, HostDrive). The player
// is a column around GTA's ped position (the root, about 1 m above the feet). What counts is the speed
// of the vehicle's point where the player is (its pose this frame and last: its motion, its turning,
// up and down), so a helicopter's swinging tail or one coming down on the player's head hits like a
// car's bumper. A spinning rotor (a disc around its hub, about one of the vehicle's axes) strikes the
// player where his column crosses it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lc::drive::hit
{
	// As eVehicleType (CVehicle::m_nVehicleType).
	enum class Kind : std::uint8_t
	{
		kCar,
		kBike,
		kBoat,
		kTrain,
		kHeli,
		kPlane
	};

	inline const char* KindName(Kind a_k)
	{
		switch (a_k) {
		case Kind::kCar:
			return "car";
		case Kind::kBike:
			return "bike";
		case Kind::kBoat:
			return "boat";
		case Kind::kTrain:
			return "train";
		case Kind::kHeli:
			return "helicopter";
		case Kind::kPlane:
			return "plane";
		}
		return "vehicle";
	}

	// Helicopters, planes and trains: heavier, and slower still hurts.
	inline bool Heavy(Kind a_k) { return a_k == Kind::kHeli || a_k == Kind::kPlane || a_k == Kind::kTrain; }

	struct Pose
	{
		float pos[3]{};
		float right[3]{ 1.0f, 0.0f, 0.0f }, fwd[3]{ 0.0f, 1.0f, 0.0f }, up[3]{ 0.0f, 0.0f, 1.0f };
	};

	inline void ToLocal(const Pose& a_p, const float a_w[3], float a_out[3])
	{
		const float d[3] = { a_w[0] - a_p.pos[0], a_w[1] - a_p.pos[1], a_w[2] - a_p.pos[2] };
		a_out[0] = d[0] * a_p.right[0] + d[1] * a_p.right[1] + d[2] * a_p.right[2];
		a_out[1] = d[0] * a_p.fwd[0] + d[1] * a_p.fwd[1] + d[2] * a_p.fwd[2];
		a_out[2] = d[0] * a_p.up[0] + d[1] * a_p.up[1] + d[2] * a_p.up[2];
	}

	inline void DirToWorld(const Pose& a_p, const float a_l[3], float a_out[3])
	{
		for (int k = 0; k < 3; ++k) {
			a_out[k] = a_p.right[k] * a_l[0] + a_p.fwd[k] * a_l[1] + a_p.up[k] * a_l[2];
		}
	}

	inline void ToWorld(const Pose& a_p, const float a_l[3], float a_out[3])
	{
		DirToWorld(a_p, a_l, a_out);
		for (int k = 0; k < 3; ++k) {
			a_out[k] += a_p.pos[k];
		}
	}

	struct Tuning
	{
		float margin = 0.35f;     // the player's radius around the box
		float below = 1.2f;       // the root this far below the box's bottom still touches (the head)
		float above = 0.3f;       // ... this far above its top (higher: standing on it)
		float speed = 3.0f;       // m/s at the contact: slower pushes, it doesn't knock over
		float heavySpeed = 2.0f;  // helicopters, planes, trains
		float maxSpeed = 90.0f;   // faster: GTA moved it (a teleport), not a hit
		float feet = 1.0f, head = 0.8f;  // the player's column: below and above the root
		float bodyRadius = 0.3f;         // rotor strikes: how close to the disc counts
	};

	struct BodyHit
	{
		bool  hit = false;
		bool  touching = false;  // the player is at the box (hit or not)
		float speed = 0.0f;      // m/s of the vehicle's point at the player
		float vel[3]{};          // its velocity (world)
		float local[3]{};        // the player's root in the vehicle's frame
	};

	// The vehicle's body this frame: is the player at its box (inflated by his size), and is the box's
	// point there moving into him at a_minSpeed or faster? a_prev is its pose a_dt ago.
	inline BodyHit Body(const Pose& a_now, const Pose& a_prev, float a_dt, const float a_lo[3], const float a_hi[3], const float a_root[3], float a_minSpeed,
		const Tuning& a_t = {})
	{
		BodyHit r;
		ToLocal(a_now, a_root, r.local);
		const float* l = r.local;
		if (l[0] < a_lo[0] - a_t.margin || l[0] > a_hi[0] + a_t.margin || l[1] < a_lo[1] - a_t.margin || l[1] > a_hi[1] + a_t.margin || l[2] < a_lo[2] - a_t.below ||
			l[2] > a_hi[2] + a_t.above) {
			return r;
		}
		r.touching = true;
		if (!(a_dt > 1e-4f)) {
			return r;
		}
		// The vehicle's point where the player is now: where it was last frame.
		float was[3];
		ToWorld(a_prev, l, was);
		for (int k = 0; k < 3; ++k) {
			r.vel[k] = (a_root[k] - was[k]) / a_dt;
		}
		r.speed = std::sqrt(r.vel[0] * r.vel[0] + r.vel[1] * r.vel[1] + r.vel[2] * r.vel[2]);
		if (!std::isfinite(r.speed) || r.speed < a_minSpeed || r.speed > a_t.maxSpeed) {
			return r;
		}
		// Into him: toward him from the box's middle, or from the box's nearest point (a swinging tail,
		// a body coming down on him). Not a vehicle pulling away (or him walking into one).
		const float mid[3] = { (a_lo[0] + a_hi[0]) * 0.5f, (a_lo[1] + a_hi[1]) * 0.5f, (a_lo[2] + a_hi[2]) * 0.5f };
		float       fromMid[3] = { l[0] - mid[0], l[1] - mid[1], l[2] - mid[2] }, fromMidW[3];
		DirToWorld(a_now, fromMid, fromMidW);
		const float nearest[3] = { std::clamp(l[0], a_lo[0], a_hi[0]), std::clamp(l[1], a_lo[1], a_hi[1]), std::clamp(l[2], a_lo[2], a_hi[2]) };
		float       fromNear[3] = { l[0] - nearest[0], l[1] - nearest[1], l[2] - nearest[2] }, fromNearW[3];
		DirToWorld(a_now, fromNear, fromNearW);
		// (each by more than a glance: 0.1 of the speed along the unit direction)
		const float midLen = std::hypot(fromMidW[0], fromMidW[1]);
		const float nearLen = std::sqrt(fromNearW[0] * fromNearW[0] + fromNearW[1] * fromNearW[1] + fromNearW[2] * fromNearW[2]);
		const float horiz = r.vel[0] * fromMidW[0] + r.vel[1] * fromMidW[1];  // (cars: as before, along the ground)
		const float face = r.vel[0] * fromNearW[0] + r.vel[1] * fromNearW[1] + r.vel[2] * fromNearW[2];
		const float down = fromNearW[2] < -0.05f ? -r.vel[2] : 0.0f;  // the player under it, it coming down
		r.hit = horiz > 0.1f * r.speed * midLen || (nearLen > 1e-3f && face > 0.1f * r.speed * nearLen) || down > a_minSpeed * 0.5f;
		return r;
	}

	// A rotor: a disc of a_radius around its hub (the vehicle's frame), turning about one of the
	// vehicle's axes (0 x: a tail rotor; 2 z: a main rotor). Inside a_inner it's the mast (the body).
	struct Rotor
	{
		float hub[3]{};
		int   axis = 2;
		float radius = 0.0f, inner = 0.4f;
	};

	struct RotorHit
	{
		bool  hit = false;
		float rho = 0.0f;    // the strike's distance from the hub (m)
		float local[3]{};    // where (the vehicle's frame)
		float fling[3]{};    // which way the player goes (world, unit): out from the hub, a little up
	};

	// Does the player's column (a_root's feet to head, world up) cross the spinning disc?
	inline RotorHit Strike(const Pose& a_now, const Rotor& a_r, const float a_root[3], const Tuning& a_t = {})
	{
		RotorHit r;
		if (a_r.radius <= 0.0f) {
			return r;
		}
		const float feetW[3] = { a_root[0], a_root[1], a_root[2] - a_t.feet }, headW[3] = { a_root[0], a_root[1], a_root[2] + a_t.head };
		float       a[3], b[3];
		ToLocal(a_now, feetW, a);
		ToLocal(a_now, headW, b);
		for (int k = 0; k < 3; ++k) {
			a[k] -= a_r.hub[k];
			b[k] -= a_r.hub[k];
		}
		const int   ax = std::clamp(a_r.axis, 0, 2);
		const float s0 = a[ax], s1 = b[ax];
		float       t = 0.0f;
		if (s0 * s1 <= 0.0f) {
			t = std::fabs(s0 - s1) > 1e-6f ? s0 / (s0 - s1) : 0.0f;
		} else {
			t = std::fabs(s0) < std::fabs(s1) ? 0.0f : 1.0f;
			if (std::fabs(t == 0.0f ? s0 : s1) > a_t.bodyRadius) {
				return r;  // the whole column is above or below the disc
			}
		}
		float q[3];
		for (int k = 0; k < 3; ++k) {
			q[k] = a[k] + (b[k] - a[k]) * t;
		}
		q[ax] = 0.0f;
		r.rho = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
		if (r.rho < a_r.inner || r.rho > a_r.radius + a_t.bodyRadius) {
			return r;
		}
		r.hit = true;
		for (int k = 0; k < 3; ++k) {
			r.local[k] = q[k] + a_r.hub[k];
		}
		r.local[ax] = a_r.hub[ax] + (a[ax] + (b[ax] - a[ax]) * t);
		const float out[3] = { q[0] / r.rho, q[1] / r.rho, q[2] / r.rho };
		float       w[3];
		DirToWorld(a_now, out, w);
		const float h = std::hypot(w[0], w[1]);
		if (h > 0.1f) {
			r.fling[0] = w[0] / h * 0.96f, r.fling[1] = w[1] / h * 0.96f, r.fling[2] = 0.28f;
		} else {
			r.fling[0] = 0.0f, r.fling[1] = 0.0f, r.fling[2] = 1.0f;  // (a disc on its side, the player under its edge)
		}
		return r;
	}

	// How hard a hit at a_speed (m/s) is: GTA damage, the push's force and the ragdoll's length.
	struct Blow
	{
		float gtaDamage = 0.0f, force = 0.0f;
		int   ms = 0;
	};

	// The push (APPLY_FORCE_TO_PED, world axes) sends the knocked-over player off at about 1.8 m/s per
	// unit of force (measured 0.1 s after it: 12 -> 21 m/s, 19 -> 34 m/s, 25 -> 52 m/s). The old forces
	// (2 per m/s of the vehicle) threw him 40 to 90 m.
	inline constexpr float kForceToSpeed = 1.8f;
	// GTA's own run-over, measured (DebugVehicleHit=ped): a pedestrian standing still, hit by a car at
	// 10.1 m/s, went off at 8.8 m/s and came to rest 10.9 m away. The player goes off at this share of
	// the vehicle's speed, at least kMinThrowSpeed, and never faster than kMaxThrowSpeed (any knockdown).
	inline constexpr float kThrowShare = 0.87f;
	inline constexpr float kMinThrowSpeed = 3.5f, kMaxThrowSpeed = 20.0f;
	// The push goes up by this share of its force.
	inline constexpr float kBodyUp = 0.3f;

	// The force for a throw at a_speed m/s (capped).
	inline float ForceForThrow(float a_speed) { return std::clamp(a_speed, 0.0f, kMaxThrowSpeed) / kForceToSpeed; }

	inline Blow BodyBlow(float a_speed, Kind a_k)
	{
		Blow b;
		const float heavy = Heavy(a_k) ? 1.5f : 1.0f;
		b.force = ForceForThrow(std::max(kThrowShare * a_speed * (Heavy(a_k) ? 1.15f : 1.0f), kMinThrowSpeed));
		b.ms = static_cast<int>(std::clamp(1500.0f + 150.0f * a_speed * heavy, 1500.0f, 5000.0f));
		b.gtaDamage = std::clamp(2.5f * a_speed * heavy, 8.0f, 150.0f);
		return b;
	}

	// A spinning rotor: nearly lethal, and a hard fling (a tail rotor is smaller and slower: less).
	inline Blow RotorBlow(bool a_main)
	{
		Blow b;
		b.gtaDamage = a_main ? 180.0f : 120.0f;
		b.force = ForceForThrow(a_main ? 16.0f : 12.0f);
		b.ms = 5000;
		return b;
	}
}
