// Falls while GTA IV moves the player on foot in Minecraft mode: pure logic, no IV-SDK.
// HostDrive.cpp feeds it; asi/tests/drive_test.cpp tests it.
//
// Knocked over, bailing out of a car, getting back up: GTA's physics moves Niko then, and a high fall's
// landing kills him in one frame, through Combat's health buffer (GTA's wasted flow while Minecraft's
// player was unhurt). So while he falls he is protected (HostDrive makes him invincible), and the drop is
// measured for Minecraft, which applies its own fall damage to its player (proto::kHurtFall): what falls
// do in Minecraft mode anyway, with Feather Falling, Protection, creative and totems.
#pragma once

#include <cmath>

namespace lc::drive
{
	class FallWatch
	{
	public:
		static constexpr float kFallSpeed = 3.0f;      // m/s down: a fall (not a stagger or a slide down a slope)
		static constexpr float kStillSpeed = 1.0f;     // m/s up or down: slower than this counts as come to rest
		static constexpr float kSettleSeconds = 0.15f; // ... this long: landed
		static constexpr float kGuardSeconds = 0.5f;   // protected this long after landing (the landing's own frames)
		static constexpr float kMinReport = 1.0f;      // metres: shorter drops aren't sent
		static constexpr float kWarp = 10.0f;          // metres in one frame: the game moved him, not a fall
		static constexpr float kGravity = 9.8f;        // m/s^2 (GTA IV's)

		struct Out
		{
			bool  protect = false;  // GTA must not hurt him this frame (he falls, or just landed)
			bool  started = false;  // a fall began this frame
			float landed = 0.0f;    // > 0: a fall ended this frame, this many metres from its top
		};

		// a_watch: GTA moves him on foot and Minecraft owns his health. a_z: his height (GTA metres).
		Out Tick(bool a_watch, float a_dt, float a_z, bool a_inWater)
		{
			Out out;
			if (!a_watch) {
				*this = FallWatch{};
				return out;
			}
			if (!have_ || std::fabs(a_z - lastZ_) > kWarp) {
				// The first frame, or a warp (up or down): a fresh start where he is.
				have_ = true;
				lastZ_ = a_z;
				top_ = a_z;
				falling_ = false;
				still_ = 0.0f;
				out.protect = guard_ > 0.0f;
				return out;
			}
			if (!(a_dt > 1e-4f)) {
				out.protect = falling_ || guard_ > 0.0f;  // (paused: nothing moves)
				return out;
			}
			const float vz = (a_z - lastZ_) / a_dt;
			lastZ_ = a_z;
			still_ = std::fabs(vz) < kStillSpeed ? still_ + a_dt : 0.0f;
			if (!falling_) {
				if (vz <= -kFallSpeed) {
					// Falling freely, he reached this speed from rest (or the top of a throw) v^2 / 2g higher up.
					falling_ = true;
					out.started = true;
					guard_ = 0.0f;
					const float from = a_z + vz * vz / (2.0f * kGravity);
					top_ = from > top_ ? from : top_;
				} else if (vz > 0.0f) {
					top_ = a_z > top_ ? a_z : top_;  // thrown up: the fall counts from the top
				} else {
					top_ = a_z;  // standing, or sliding down slower than a fall
				}
			}
			if (falling_ && still_ >= kSettleSeconds) {
				falling_ = false;
				const float drop = top_ - a_z;
				if (!a_inWater && drop >= kMinReport) {
					out.landed = drop;
				}
				top_ = a_z;
				guard_ = kGuardSeconds;
			} else if (!falling_ && guard_ > 0.0f) {
				guard_ -= a_dt;
			}
			out.protect = falling_ || guard_ > 0.0f;
			return out;
		}

		[[nodiscard]] bool  falling() const { return falling_; }
		[[nodiscard]] float top() const { return top_; }

	private:
		bool  have_ = false;
		bool  falling_ = false;
		float lastZ_ = 0.0f;
		float top_ = 0.0f;     // the highest point since he last stood or slid down
		float still_ = 0.0f;   // seconds at rest (vertically)
		float guard_ = 0.0f;   // seconds of protection left after a landing
	};
}
