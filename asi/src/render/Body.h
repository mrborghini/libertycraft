// The player's Minecraft body on Niko's skeleton: while GTA IV animates Niko itself (getting into,
// driving and getting out of a vehicle, cutscenes, Niko mode if wanted) his bones pose the
// Minecraft body and Niko is hidden. Pure maths, SDK-free and D3D-free (tests/body_test.cpp).
//
// Minecraft sends its player's body standing still (kRenRagdoll): blocks relative to the feet,
// facing +Z with +X the player's left and +Y up, split into Minecraft's six parts (RenBatch flags
// bits 8-11, proto::RagdollPart; held items ride their arm). Each part stays a rigid piece hung at
// its joint, and the whole figure keeps Minecraft's own proportions: every part is scaled by the
// same factor, Niko's height over the Minecraft player's (kNikoFit, times MinecraftBodyScale), so
// it looks like a Minecraft player of his size, never stretched to his bones (his legs are longer,
// his head half the size). GTA's bones turn and place the parts:
//  - the body (torso) leans and turns with GTA's hips -> neck line and its shoulder line, from
//    Minecraft's hips, which sit lower than Niko's (his legs are longer),
//  - seated (a car seat, a sofa: SeatedAmount) the figure is smaller (kSeatFit), as the Minecraft
//    torso and head are taller than Niko's, so its head stays under the roof and its hips on the seat,
//  - the whole figure moves up or down so that its lower sole meets GTA's lower sole, but never so
//    high that the top of its head is over Niko's (kHeadRoom; then it sinks into the seat),
//  - each leg points from its (Minecraft) hip at GTA's sole, so the feet stay where his are,
//  - each arm points from its (Minecraft) shoulder at GTA's hand, so hands meet a steering wheel,
//  - the head turns with GTA's head bone (its axes named by a HeadMap: kNikoHead, or CalibrateHead).
//
// Spaces: "standing" = the kRenRagdoll vertices (blocks, x left, y up, z forward, feet at 0).
// "GTA" = GTA world axes (x east, y north, z up, metres) relative to an origin the caller picks
// near the ped (the bones are read relative to it). PoseMesh writes Minecraft coordinates
// relative to that same origin (mc = (x, z, -y)), ready for the block shader.
#pragma once

#include "libertycraft_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lc::render::body
{
	namespace proto = ::libertycraft::proto;

	struct V3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};
	inline V3    operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V3    operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V3    operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline V3    Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	inline float Length(V3 a) { return std::sqrt(Dot(a, a)); }
	inline bool  Finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
	inline V3    Normalize(V3 a, V3 a_fallback)
	{
		const float l = Length(a);
		return l > 1e-6f && std::isfinite(l) ? a * (1.0f / l) : a_fallback;
	}

	// A rotation as the images of the standing body's x (left), y (up) and z (forward) axes.
	struct M3
	{
		V3 c[3]{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	};
	inline V3 Apply(const M3& a_m, V3 a_p) { return a_m.c[0] * a_p.x + a_m.c[1] * a_p.y + a_m.c[2] * a_p.z; }

	// a_r after a_m.
	inline M3 Then(const M3& a_r, const M3& a_m) { return { { Apply(a_r, a_m.c[0]), Apply(a_r, a_m.c[1]), Apply(a_r, a_m.c[2]) } }; }

	// The shortest turn taking unit a_from to unit a_to (Rodrigues). Pointing opposite ways: half a
	// turn about a_axis (made square to a_from).
	inline M3 Arc(V3 a_from, V3 a_to, V3 a_axis)
	{
		const V3    v = Cross(a_from, a_to);
		const float c = Dot(a_from, a_to);
		M3          r;
		if (c < -0.9999f) {
			const V3 k = Normalize(a_axis - a_from * Dot(a_axis, a_from), Normalize(Cross(a_from, V3{ 0, 0, 1 }), V3{ 1, 0, 0 }));
			for (int i = 0; i < 3; ++i) {
				const V3 e{ i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f };
				r.c[i] = k * (2.0f * Dot(k, e)) - e;  // 2kk^T - I
			}
			return r;
		}
		const float f = 1.0f / (1.0f + c);
		for (int i = 0; i < 3; ++i) {
			const V3 e{ i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f };
			r.c[i] = e * c + Cross(v, e) + v * (Dot(v, e) * f);
		}
		return r;
	}

	// An upright frame: y along a_up exactly, x (left) as close to a_left as it can be, z forward.
	inline M3 Frame(V3 a_left, V3 a_up, V3 a_fallbackLeft = { 1, 0, 0 })
	{
		const V3 up = Normalize(a_up, V3{ 0, 0, 1 });
		V3       left = a_left - up * Dot(a_left, up);
		if (Length(left) < 1e-4f) {
			left = a_fallbackLeft - up * Dot(a_fallbackLeft, up);
		}
		left = Normalize(left, Normalize(Cross(up, V3{ 0, 1, 0 }), V3{ 1, 0, 0 }));
		return { { left, up, Cross(left, up) } };  // x cross y = z (Minecraft's axes are right-handed)
	}

	// ---- Minecraft's player model, standing (AvatarRenderer scales it by 0.9375, and the model's
	// y = 0, the neck, lands 1.501 blocks up; HumanoidModel/PlayerModel pivots) -------------------
	inline constexpr float kModelScale = 0.9375f;
	inline constexpr float kPx = kModelScale / 16.0f;                    // one model pixel in blocks
	inline constexpr float kNeckY = 1.501f * kModelScale;                // head and body pivot
	inline constexpr float kHipY = (1.501f - 12.0f / 16.0f) * kModelScale;   // leg pivots, the body's bottom
	inline constexpr float kShoulderY = (1.501f - 2.0f / 16.0f) * kModelScale;  // arm pivots
	inline constexpr float kShoulderX = 5.0f * kPx;
	inline constexpr float kLegX = 1.9f * kPx;
	inline constexpr float kLegLength = 12.0f * kPx;  // leg pivot to sole
	inline constexpr float kArmReach = 10.0f * kPx;   // arm pivot to the end of the arm
	inline constexpr float kArmCentre = 1.0f * kPx;   // the arm's centre line is a pixel outside its pivot
	inline constexpr float kArmSway = 0.1f;           // HumanoidModel's idle arm sway at ageInTicks 0 (radians, outwards)

	inline V3 HipCentre() { return { 0.0f, kHipY, 0.0f }; }
	inline V3 NeckJoint() { return { 0.0f, kNeckY, 0.0f }; }
	inline V3 ShoulderJoint(bool a_left) { return { a_left ? kShoulderX : -kShoulderX, kShoulderY, 0.0f }; }
	inline V3 HipJoint(bool a_left) { return { a_left ? kLegX : -kLegX, kHipY, 0.0f }; }
	inline V3 LegEnd() { return { 0.0f, -kLegLength, 0.0f }; }  // from its pivot
	// The end of the arm (the hand) from its pivot, as the standing body hangs it.
	inline V3 ArmEnd(bool a_left)
	{
		const float c = std::cos(kArmSway), s = std::sin(kArmSway);
		const float out = kArmCentre * c + kArmReach * s;
		return { a_left ? out : -out, -(kArmReach * c - kArmCentre * s), 0.0f };
	}

	// ---- GTA's skeleton ----------------------------------------------------------------------------
	inline constexpr float kAnkleHeight = 0.11f;  // GTA's ankle (foot bone) above its sole, metres (measured standing: 0.10 to 0.12)
	inline constexpr float kMaxLift = 0.6f;       // the sole match moves the figure at most this far
	inline constexpr float kSkullTop = 0.2f;  // GTA's head bone (the skull's base) to the top of the head, metres
	inline constexpr float kHeadPx = 8.0f;    // Minecraft's head: 8 model pixels
	inline constexpr float kHeadRoom = 0.03f;  // the top of the Minecraft head at most this far over Niko's (metres)
	// The Minecraft player's height (soles to the top of the head, 32 model pixels) and Niko's,
	// measured standing (the first pose in the log: ankles 0.80 m under the hips, 0.11 m over the
	// soles; the head bone 0.66 m over the hips, kSkullTop under the top): 1.77 m.
	inline constexpr float kModelHeight = kNeckY + kHeadPx * kPx;
	inline constexpr float kNikoHeight = 1.77f;
	inline constexpr float kNikoFit = kNikoHeight / kModelHeight;  // about 0.94
	// Seated (a car seat, a chair) the Minecraft torso and head are taller than Niko's: from his hips
	// to the top of his head he is 0.86 m (measured), Minecraft's 20 model pixels over its hips,
	// 1.17 m. Seated the whole figure is this size instead, so its head stays under the roof and its
	// hips on the seat (sinking it into the seat instead put its legs under the car's floor).
	inline constexpr float kNikoSeatedTop = 0.86f;
	inline constexpr float kSeatFit = (kNikoSeatedTop + 0.03f) / (20.0f * kPx);  // about 0.76

	// Joint positions (GTA axes, relative to the caller's origin) and the head bone's axes.
	struct Skeleton
	{
		V3   hipL, hipR;            // thighs
		V3   kneeL, kneeR;          // calves (with knees)
		V3   ankleL, ankleR;        // feet
		bool knees = false;
		V3   shoulderL, shoulderR;  // upper arms
		V3   handL, handR;
		V3   neck;
		V3   head;                  // the head bone (the skull's base)
		bool headPos = false;
		V3   headAxes[3];           // the head bone's own x, y, z in GTA axes (unit)
		bool headOk = false;
	};

	// Which head-bone axis is the standing body's x (left), y (up) and z (forward): axis[i] = +-(1 + k).
	struct HeadMap
	{
		int axis[3] = { 0, 0, 0 };

		constexpr HeadMap() = default;
		constexpr HeadMap(int a_left, int a_up, int a_forward) :
			axis{ a_left, a_up, a_forward }
		{}

		bool Valid() const
		{
			int seen = 0;
			for (const int a : axis) {
				const int k = a < 0 ? -a : a;
				if (k < 1 || k > 3 || (seen & (1 << k))) {
					return false;
				}
				seen |= 1 << k;
			}
			return true;
		}

		bool operator==(const HeadMap& a_o) const { return axis[0] == a_o.axis[0] && axis[1] == a_o.axis[1] && axis[2] == a_o.axis[2]; }
	};

	// GTA IV's head bone (measured in game, CalibrateHead agreeing over 30 frames): its x runs up the
	// neck, y forward, z to the left.
	inline constexpr HeadMap kNikoHead{ 3, 1, 2 };

	inline M3 HeadFrame(const V3 a_axes[3], const HeadMap& a_map)
	{
		M3 m;
		for (int i = 0; i < 3; ++i) {
			const int k = (a_map.axis[i] < 0 ? -a_map.axis[i] : a_map.axis[i]) - 1;
			m.c[i] = a_axes[k] * (a_map.axis[i] < 0 ? -1.0f : 1.0f);
		}
		return Frame(m.c[0], m.c[1]);
	}

	// The map that lines the head bone's axes up with a_body's (a frame where the head looks the
	// way the body faces, within acos(a_minDot): 26 degrees; at 0.8 a head turned 60 degrees would
	// match its forward with the body's left). False: no clear match, or not a rotation.
	inline bool CalibrateHead(const V3 a_axes[3], const M3& a_body, HeadMap& a_out, float a_minDot = 0.9f)
	{
		HeadMap m;
		for (int i = 0; i < 3; ++i) {
			int   best = -1;
			float bestDot = 0.0f;
			for (int k = 0; k < 3; ++k) {
				const float d = Dot(a_body.c[i], a_axes[k]);
				if (std::fabs(d) > std::fabs(bestDot)) {
					bestDot = d;
					best = k;
				}
			}
			if (best < 0 || std::fabs(bestDot) < a_minDot) {
				return false;
			}
			m.axis[i] = bestDot > 0.0f ? best + 1 : -(best + 1);
		}
		if (!m.Valid()) {
			return false;
		}
		M3 f;
		for (int i = 0; i < 3; ++i) {
			const int k = (m.axis[i] < 0 ? -m.axis[i] : m.axis[i]) - 1;
			f.c[i] = a_axes[k] * (m.axis[i] < 0 ? -1.0f : 1.0f);
		}
		if (Dot(Cross(f.c[0], f.c[1]), f.c[2]) <= 0.0f) {
			return false;  // a mirror, not a turn
		}
		a_out = m;
		return true;
	}

	// GTA (relative to the origin) = m * (x, y, z, 1) of the standing body.
	struct Part
	{
		float m[3][4]{};
	};

	// The standing body's a_jointStanding to a_jointGta, turned by a_r after scaling by a_scale (the
	// standing body's axes) about the joint.
	inline Part MakePart(const M3& a_r, V3 a_jointStanding, V3 a_jointGta, V3 a_scale = { 1, 1, 1 })
	{
		const M3 m{ { a_r.c[0] * a_scale.x, a_r.c[1] * a_scale.y, a_r.c[2] * a_scale.z } };
		const V3 t = a_jointGta - Apply(m, a_jointStanding);
		const V3 cols[4] = { m.c[0], m.c[1], m.c[2], t };
		Part     p;
		for (int c = 0; c < 4; ++c) {
			p.m[0][c] = cols[c].x;
			p.m[1][c] = cols[c].y;
			p.m[2][c] = cols[c].z;
		}
		return p;
	}

	inline V3 Transform(const Part& a_p, V3 a_v)
	{
		const auto& m = a_p.m;
		return { m[0][0] * a_v.x + m[0][1] * a_v.y + m[0][2] * a_v.z + m[0][3], m[1][0] * a_v.x + m[1][1] * a_v.y + m[1][2] * a_v.z + m[1][3],
			m[2][0] * a_v.x + m[2][1] * a_v.y + m[2][2] * a_v.z + m[2][3] };
	}

	inline Part IdentityPart()
	{
		Part p;
		p.m[0][0] = p.m[1][1] = p.m[2][2] = 1.0f;
		return p;
	}

	struct Pose
	{
		// Per RagdollPart. part[kPartNone] (no batch is sent with it: Minecraft's parts are 1 to 6) is
		// where the held items go in the standing body before their arm's part moves them: the
		// identity (as Minecraft holds them), or collapsed (HideHeld). A batch with no valid part is
		// drawn as the body.
		Part  part[proto::kPartCount];
		float lift = 0.0f;              // metres the figure moved from Niko's hips (up positive)
		float sink = 0.0f;              // of that, how far under the sole match the head room took it (seated)
		float scale = 1.0f;             // every part's size against Minecraft's
		M3    torso;                    // the body's frame (GTA axes)
		bool  headFromBone = false;
		bool  hideBack = false;         // the cape and elytra (kRagdollBack) out of sight: seated they hung through the seat
	};

	// How much a_s sits (0 standing, 1 seated): both thighs (hip -> knee) raised from the pelvis's down
	// line, from 40 degrees (a stride, a crouch) to 70 (seated). A running stride has a thigh behind,
	// so only both raised counts.
	inline float SeatedAmount(const Skeleton& a_s)
	{
		if (!a_s.knees) {
			return 0.0f;
		}
		const V3 hipC = (a_s.hipL + a_s.hipR) * 0.5f;
		const M3 pelvis = Frame(a_s.hipL - a_s.hipR, a_s.neck - hipC);
		const V3 down = pelvis.c[1] * -1.0f;
		float    amount = 1.0f;
		for (int side = 0; side < 2; ++side) {
			const V3    thigh = Normalize(side ? a_s.kneeL - a_s.hipL : a_s.kneeR - a_s.hipR, down);
			const float deg = std::acos(std::clamp(Dot(thigh, down), -1.0f, 1.0f)) * 57.29578f;
			const float t = std::clamp((deg - 40.0f) / 30.0f, 0.0f, 1.0f);
			amount = std::min(amount, t * t * (3.0f - 2.0f * t));
		}
		return amount;
	}

	// The body's size for a_seated (SeatedAmount, smoothed): kNikoFit standing, kSeatFit seated.
	inline float ScaleFor(float a_seated) { return kNikoFit + (kSeatFit - kNikoFit) * std::clamp(a_seated, 0.0f, 1.0f); }

	// Niko's leg, hip to sole (thigh + shin + ankle height; hip to ankle without knees).
	inline float LegLength(const Skeleton& a_s, bool a_left)
	{
		const V3 hip = a_left ? a_s.hipL : a_s.hipR, ankle = a_left ? a_s.ankleL : a_s.ankleR, knee = a_left ? a_s.kneeL : a_s.kneeR;
		return (a_s.knees ? Length(knee - hip) + Length(ankle - knee) : Length(ankle - hip)) + kAnkleHeight;
	}

	// The standing body's parts onto a_s, every part a_scale times Minecraft's size (kNikoFit: Niko's
	// height; ScaleFor). a_map: the head bone's axes (invalid: the head turns with the body). a_seated
	// (SeatedAmount): how much the hips stay where Niko's are instead of the soles meeting his.
	inline Pose Solve(const Skeleton& a_s, const HeadMap& a_map, float a_scale = kNikoFit, float a_seated = 0.0f)
	{
		Pose      out;
		const V3  hipC = (a_s.hipL + a_s.hipR) * 0.5f;
		const V3  up = a_s.neck - hipC;
		const M3  pelvis = Frame(a_s.hipL - a_s.hipR, up);
		const M3  torso = Frame(a_s.shoulderL - a_s.shoulderR, up, pelvis.c[0]);
		const V3  down = pelvis.c[1] * -1.0f;
		out.torso = torso;
		const float k = std::clamp(a_scale, 0.3f, 2.0f);
		const V3    sk{ k, k, k };
		out.scale = k;

		// Head: GTA's head bone (or the body's turn), on Minecraft's neck.
		M3 head = torso;
		if (a_s.headOk && a_map.Valid()) {
			const M3 h = HeadFrame(a_s.headAxes, a_map);
			// A head turned further than a person can turn one: the map is wrong for this skeleton.
			if (Dot(h.c[1], torso.c[1]) > 0.0f && Dot(h.c[2], torso.c[2]) > -0.2f) {
				head = h;
				out.headFromBone = true;
			}
		}

		// Up or down: the lower sole onto GTA's lower sole, Minecraft's (shorter) legs along GTA's hip
		// -> ankle lines; but the top of the head no higher than Niko's.
		float mcSole = 1e9f;
		for (int side = 0; side < 2; ++side) {
			const bool left = side == 1;
			const V3   dir = Normalize((left ? a_s.ankleL : a_s.ankleR) - (left ? a_s.hipL : a_s.hipR), down);
			const V3   joint = hipC + Apply(pelvis, (HipJoint(left) - HipCentre()) * k);
			mcSole = std::min(mcSole, (joint + dir * (kLegLength * k)).z);
		}
		// Seated the hips stay on the seat: the shorter Minecraft legs dangle short of the floor.
		const float gtaSole = std::min(a_s.ankleL.z, a_s.ankleR.z) - kAnkleHeight;
		const float soleLift = (gtaSole - mcSole) * (1.0f - std::clamp(a_seated, 0.0f, 1.0f));
		float       lift = soleLift;
		if (a_s.headPos) {
			const V3    neck = hipC + Apply(torso, (NeckJoint() - HipCentre()) * k);
			const float mcTop = (neck + head.c[1] * (kHeadPx * kPx * k)).z;
			const float nikoTop = (a_s.head + head.c[1] * kSkullTop).z;
			lift = std::min(lift, nikoTop + kHeadRoom - mcTop);
		}
		out.lift = std::clamp(lift, -kMaxLift, kMaxLift);
		out.sink = std::max(0.0f, std::clamp(soleLift, -kMaxLift, kMaxLift) - out.lift);
		const V3 hip = hipC + V3{ 0.0f, 0.0f, out.lift };

		out.part[proto::kPartBody] = MakePart(torso, HipCentre(), hip, sk);

		// Legs: from Minecraft's hips at GTA's soles (the hips' sideways line keeps its roll).
		for (int side = 0; side < 2; ++side) {
			const bool left = side == 1;
			const V3   joint = hip + Apply(pelvis, (HipJoint(left) - HipCentre()) * k);
			const V3   sole = (left ? a_s.ankleL : a_s.ankleR) - V3{ 0.0f, 0.0f, kAnkleHeight };
			V3         to = sole - joint;
			if (Length(to) < 0.05f) {
				to = (left ? a_s.ankleL : a_s.ankleR) - (left ? a_s.hipL : a_s.hipR);
			}
			const M3 leg = Then(Arc(down, Normalize(to, down), pelvis.c[0]), pelvis);
			out.part[left ? proto::kPartLeftLeg : proto::kPartRightLeg] = MakePart(leg, HipJoint(left), joint, sk);
		}

		// Arms: from Minecraft's shoulders at GTA's hands.
		for (int side = 0; side < 2; ++side) {
			const bool left = side == 1;
			const V3   joint = hip + Apply(torso, (ShoulderJoint(left) - HipCentre()) * k);
			const V3   rest = Apply(torso, Normalize(ArmEnd(left), V3{ 0, -1, 0 }));
			const V3   hand = left ? a_s.handL : a_s.handR;
			V3         to = hand - joint;
			if (Length(to) < 0.05f) {
				to = hand - (left ? a_s.shoulderL : a_s.shoulderR);
			}
			const M3 arm = Then(Arc(rest, Normalize(to, rest), torso.c[2]), torso);
			out.part[left ? proto::kPartLeftArm : proto::kPartRightArm] = MakePart(arm, ShoulderJoint(left), joint, sk);
		}

		out.part[proto::kPartHead] = MakePart(head, NeckJoint(), hip + Apply(torso, (NeckJoint() - HipCentre()) * k), sk);
		out.part[proto::kPartNone] = IdentityPart();
		return out;
	}

	// A part shrunk to a point (its joint): its triangles have no area and draw nothing.
	inline void Collapse(Part& a_p)
	{
		for (auto& row : a_p.m) {
			row[0] = row[1] = row[2] = 0.0f;
		}
	}

	// The held items (sword, shield, ...) out of sight: in a vehicle they poked through its roof.
	inline void HideHeld(Pose& a_pose) { Collapse(a_pose.part[proto::kPartNone]); }

	// Each part's box in the standing body (blocks; x left, y up, z forward), with Minecraft's outer
	// skin layer and armour (a pixel all round). Held items aren't in it.
	inline void PartBox(std::uint32_t a_part, V3& a_lo, V3& a_hi)
	{
		const float p = kPx;
		switch (a_part) {
		case proto::kPartHead:
			a_lo = { -5 * p, kNeckY - p, -5 * p }, a_hi = { 5 * p, kNeckY + 9 * p, 5 * p };
			return;
		case proto::kPartRightArm:
			a_lo = { -9 * p, kShoulderY - 11 * p, -3 * p }, a_hi = { -3 * p, kShoulderY + 3 * p, 3 * p };
			return;
		case proto::kPartLeftArm:
			a_lo = { 3 * p, kShoulderY - 11 * p, -3 * p }, a_hi = { 9 * p, kShoulderY + 3 * p, 3 * p };
			return;
		case proto::kPartRightLeg:
			a_lo = { -5 * p, -p, -3 * p }, a_hi = { 1.1f * p, kHipY + p, 3 * p };
			return;
		case proto::kPartLeftLeg:
			a_lo = { -1.1f * p, -p, -3 * p }, a_hi = { 5 * p, kHipY + p, 3 * p };
			return;
		default:  // the body
			a_lo = { -5 * p, kHipY - p, -3 * p }, a_hi = { 5 * p, kNeckY + p, 3 * p };
			return;
		}
	}

	// How far a_point (GTA, relative to the pose's origin) is from a_part's box as posed (metres; 0
	// inside). The box's nearest point is found in the part's own axes (exact for the uniformly scaled
	// parts, close for the legs).
	inline float DistanceToPart(const Pose& a_pose, std::uint32_t a_part, V3 a_point)
	{
		const Part& p = a_pose.part[a_part];
		const V3    c[3] = { { p.m[0][0], p.m[1][0], p.m[2][0] }, { p.m[0][1], p.m[1][1], p.m[2][1] }, { p.m[0][2], p.m[1][2], p.m[2][2] } };
		const V3    d = a_point - V3{ p.m[0][3], p.m[1][3], p.m[2][3] };
		V3          lo, hi;
		PartBox(a_part, lo, hi);
		float l[3];
		for (int k = 0; k < 3; ++k) {
			const float n2 = Dot(c[k], c[k]);
			l[k] = n2 > 1e-12f ? Dot(c[k], d) / n2 : 0.0f;
		}
		const V3 q{ std::clamp(l[0], lo.x, hi.x), std::clamp(l[1], lo.y, hi.y), std::clamp(l[2], lo.z, hi.z) };
		return Length(a_point - Transform(p, q));
	}

	// GTA's camera inside the body (or so close that its near plane cuts it: a_margin metres): the
	// parts it is in are collapsed, so the view isn't a wall of skin texels (a helicopter dropping on
	// the player pushed GTA's camera into the Minecraft body, which is bulkier than Niko). a_all: then
	// the whole body goes, held items too (the parts next to the camera filled the view). Returns a
	// bit per part within a_margin (1 << RagdollPart).
	inline std::uint32_t HideNearCamera(Pose& a_pose, V3 a_camera, float a_margin, bool a_all = false)
	{
		std::uint32_t within = 0;
		for (std::uint32_t part = proto::kPartHead; part < proto::kPartCount; ++part) {
			if (DistanceToPart(a_pose, part, a_camera) < a_margin) {
				within |= 1u << part;
			}
		}
		for (std::uint32_t part = a_all && within ? proto::kPartNone : proto::kPartHead; part < proto::kPartCount; ++part) {
			if ((a_all && within) || (within & (1u << part))) {
				Collapse(a_pose.part[part]);
			}
		}
		return within;
	}

	using Vertex = proto::RenVertex;

	inline std::uint32_t PartOf(std::uint32_t a_batchFlags)
	{
		const std::uint32_t p = (a_batchFlags >> proto::kRagdollPartShift) & 0xF;
		return p < proto::kPartCount ? p : proto::kPartNone;
	}

	// The standing body (kRenRagdoll's batches and vertices) posed by a_pose, into a_out: Minecraft
	// coordinates relative to the skeleton's origin, the same batches. Faces turned by the pose lose
	// their axis normal (lit by their own triangle instead).
	inline void PoseMesh(const std::vector<proto::RenBatch>& a_batches, const std::vector<Vertex>& a_in, const Pose& a_pose, std::vector<Vertex>& a_out)
	{
		a_out.resize(a_in.size());
		for (const auto& b : a_batches) {
			const std::uint32_t part = PartOf(b.flags);
			const Part&         p = a_pose.part[part == proto::kPartNone ? proto::kPartBody : part];
			const bool          held = (b.flags & proto::kRagdollHeld) != 0;
			const bool          gone = a_pose.hideBack && (b.flags & proto::kRagdollBack) != 0;
			const std::size_t   end = std::min<std::size_t>(a_in.size(), std::size_t(b.first) + b.count);
			for (std::size_t i = b.first; i < end; ++i) {
				Vertex   v = a_in[i];
				V3       s{ v.x, v.y, v.z };
				if (held) {
					s = Transform(a_pose.part[proto::kPartNone], s);
				}
				if (gone) {
					s = NeckJoint();  // every vertex on one point: nothing drawn
				}
				const V3 g = Transform(p, s);
				v.x = g.x;
				v.y = g.z;
				v.z = -g.y;
				const std::uint32_t n = (v.flags >> 4) & 7;
				if (n >= 1 && n <= 6) {
					v.flags = (v.flags & ~0x70u) | (7u << 4);
				}
				a_out[i] = v;
			}
		}
	}

	// The standing body's extent per part (diagnostics): min/max of x, y, z.
	struct PartBounds
	{
		float         lo[3]{ 1e9f, 1e9f, 1e9f }, hi[3]{ -1e9f, -1e9f, -1e9f };
		std::uint32_t vertices = 0;
	};

	inline void MeasureParts(const std::vector<proto::RenBatch>& a_batches, const std::vector<Vertex>& a_v, PartBounds (&a_out)[proto::kPartCount])
	{
		for (auto& b : a_out) {
			b = PartBounds{};
		}
		for (const auto& b : a_batches) {
			auto&             pb = a_out[PartOf(b.flags)];
			const std::size_t end = std::min<std::size_t>(a_v.size(), std::size_t(b.first) + b.count);
			for (std::size_t i = b.first; i < end; ++i) {
				const float p[3] = { a_v[i].x, a_v[i].y, a_v[i].z };
				for (int k = 0; k < 3; ++k) {
					pb.lo[k] = std::min(pb.lo[k], p[k]);
					pb.hi[k] = std::max(pb.hi[k], p[k]);
				}
				++pb.vertices;
			}
		}
	}
}
