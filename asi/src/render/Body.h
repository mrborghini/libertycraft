// The player's Minecraft body on Niko's skeleton: while GTA IV animates Niko itself (getting into,
// driving and getting out of a vehicle, cutscenes, Niko mode if wanted) his bones pose the
// Minecraft body and Niko is hidden. Pure maths, SDK-free and D3D-free (tests/body_test.cpp).
//
// Minecraft sends its player's body standing still (kRenRagdoll): blocks relative to the feet,
// facing +Z with +X the player's left and +Y up, split into Minecraft's six parts (RenBatch flags
// bits 8-11, proto::RagdollPart; held items ride their arm). Each part stays a rigid piece hung at
// its joint, sized to Niko: Minecraft's upper body (torso, arms; 1.17 m from the hips to the top
// of the head, Niko's is about 0.85 m) is scaled by Niko's hips -> neck length over Minecraft's,
// the head so that its top is where Niko's is (Minecraft's head is twice a person's), so the body
// covers Niko and stays inside a car, and the legs keep the torso's thickness but take Niko's leg
// length (thigh + shin + ankle height), so the feet reach the ground. MinecraftBodyScale scales
// all but the legs' length on top. GTA's bones turn and place the parts:
//  - the body (torso) leans and turns with GTA's hips -> neck line and its shoulder line,
//  - each leg points along GTA's hip -> ankle line (the hips' sideways line keeps its roll),
//  - the whole figure moves up or down so that its lower sole meets GTA's lower sole (a bent knee
//    makes the straight Minecraft leg reach further: the figure rises a little),
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
	inline constexpr float kMinScale = 0.6f, kMaxScale = 1.2f;  // the upper body's size against Minecraft's
	inline constexpr float kSkullTop = 0.2f;  // GTA's head bone (the skull's base) to the top of the head, metres
	inline constexpr float kHeadPx = 8.0f;    // Minecraft's head: 8 model pixels
	inline constexpr float kMaxLegStretch = 2.0f;  // legs at most this much longer than the upper body's scale makes them

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

	struct Pose
	{
		Part  part[proto::kPartCount];  // kPartNone: as the body
		float lift = 0.0f;              // metres the sole match moved the figure (up positive)
		float scale = 1.0f;             // the upper body's size against Minecraft's
		float headScale = 1.0f;         // the head's
		float legScale = 1.0f;          // the legs' length against Minecraft's
		M3    torso;                    // the body's frame (GTA axes)
		bool  headFromBone = false;
	};

	// Niko's leg, hip to sole (thigh + shin + ankle height; hip to ankle without knees).
	inline float LegLength(const Skeleton& a_s, bool a_left)
	{
		const V3 hip = a_left ? a_s.hipL : a_s.hipR, ankle = a_left ? a_s.ankleL : a_s.ankleR, knee = a_left ? a_s.kneeL : a_s.kneeR;
		return (a_s.knees ? Length(knee - hip) + Length(ankle - knee) : Length(ankle - hip)) + kAnkleHeight;
	}

	// The standing body's parts onto a_s. a_map: the head bone's axes (invalid: the head turns with
	// the body).
	inline Pose Solve(const Skeleton& a_s, const HeadMap& a_map, float a_userScale = 1.0f)
	{
		Pose      out;
		const V3  hipC = (a_s.hipL + a_s.hipR) * 0.5f;
		const V3  up = a_s.neck - hipC;
		const M3  pelvis = Frame(a_s.hipL - a_s.hipR, up);
		const M3  torso = Frame(a_s.shoulderL - a_s.shoulderR, up, pelvis.c[0]);
		const V3  down = pelvis.c[1] * -1.0f;
		out.torso = torso;
		// Sizes: the upper body by Niko's hips -> neck, the legs by his leg length.
		const float user = std::clamp(a_userScale, 0.5f, 2.0f);
		const float k = std::clamp(Length(up) / (kNeckY - kHipY), kMinScale, kMaxScale) * user;
		const float legs = 0.5f * (LegLength(a_s, true) + LegLength(a_s, false)) / kLegLength;
		const float ls = std::clamp(legs, k, k * kMaxLegStretch);
		const V3    sk{ k, k, k }, sLeg{ k, ls, k };
		out.scale = k;
		out.legScale = ls;

		// Legs: along GTA's hip -> ankle lines, from Minecraft's hips.
		M3 leg[2];
		V3 legJoint[2];
		float mcSole = 1e9f;
		for (int side = 0; side < 2; ++side) {
			const bool left = side == 1;
			const V3   dir = Normalize((left ? a_s.ankleL : a_s.ankleR) - (left ? a_s.hipL : a_s.hipR), down);
			leg[side] = Then(Arc(down, dir, pelvis.c[0]), pelvis);
			legJoint[side] = hipC + Apply(pelvis, (HipJoint(left) - HipCentre()) * k);
			mcSole = std::min(mcSole, (legJoint[side] + Apply(leg[side], LegEnd() * ls)).z);
		}
		// The lower sole onto GTA's lower sole.
		const float gtaSole = std::min(a_s.ankleL.z, a_s.ankleR.z) - kAnkleHeight;
		out.lift = std::clamp(gtaSole - mcSole, -kMaxLift, kMaxLift);
		const V3 lift{ 0.0f, 0.0f, out.lift };
		const V3 hip = hipC + lift;

		out.part[proto::kPartBody] = MakePart(torso, HipCentre(), hip, sk);
		out.part[proto::kPartRightLeg] = MakePart(leg[0], HipJoint(false), legJoint[0] + lift, sLeg);
		out.part[proto::kPartLeftLeg] = MakePart(leg[1], HipJoint(true), legJoint[1] + lift, sLeg);

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

		// Head: GTA's head bone, on Minecraft's neck, its top where Niko's is.
		M3 head = torso;
		if (a_s.headOk && a_map.Valid()) {
			const M3 h = HeadFrame(a_s.headAxes, a_map);
			// A head turned further than a person can turn one: the map is wrong for this skeleton.
			if (Dot(h.c[1], torso.c[1]) > 0.0f && Dot(h.c[2], torso.c[2]) > -0.2f) {
				head = h;
				out.headFromBone = true;
			}
		}
		float kh = k;
		if (a_s.headPos) {
			const float nikoTop = Dot(a_s.head + head.c[1] * kSkullTop - hipC, torso.c[1]);  // above the hips, along the body
			const float mcNeck = k * (kNeckY - kHipY);
			kh = std::clamp((nikoTop - mcNeck) / (kHeadPx * kPx), 0.6f * k, k);
		}
		out.headScale = kh;
		out.part[proto::kPartHead] = MakePart(head, NeckJoint(), hip + Apply(torso, (NeckJoint() - HipCentre()) * k), V3{ kh, kh, kh });
		out.part[proto::kPartNone] = out.part[proto::kPartBody];
		return out;
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
			const Part&               p = a_pose.part[PartOf(b.flags)];
			const std::size_t         end = std::min<std::size_t>(a_in.size(), std::size_t(b.first) + b.count);
			for (std::size_t i = b.first; i < end; ++i) {
				Vertex   v = a_in[i];
				const V3 g = Transform(p, V3{ v.x, v.y, v.z });
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
