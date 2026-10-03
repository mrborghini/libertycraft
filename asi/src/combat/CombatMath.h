// Combat rules that don't need the game (SDK-free, unit-tested on Linux by tests/combat_test.cpp):
// actor ids (peds and vehicle pieces), Minecraft <-> GTA IV damage scaling, which GTA IV weapon types
// become which Minecraft hurt, health fractions, the knockback direction, vehicle damage, and the
// pacing of hurt events.
#pragma once

#include "libertycraft_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace lc::combat
{
	namespace proto = ::libertycraft::proto;

	// ---- actor ids ----------------------------------------------------------------------------
	// ActorRecord::formId = 0x4C000000 | the ped's script handle. The handle is what the ped pool's
	// GetIndex returns and every ped native takes: (pool slot << 8) | the slot's generation byte, so
	// it is stable while the ped exists and never names a later ped that reuses the slot. The 'L' tag
	// keeps the id non-zero (Minecraft reads 0 as "no attacker") and recognisable in logs.
	inline constexpr std::uint32_t kActorIdTag = 0x4C000000u;
	inline constexpr std::uint32_t kActorIdHandleMask = 0x00FFFFFFu;

	inline std::uint32_t ActorIdFromHandle(std::uint32_t a_handle) { return kActorIdTag | (a_handle & kActorIdHandleMask); }

	inline bool HandleFromActorId(std::uint32_t a_id, std::uint32_t& a_handle)
	{
		if ((a_id & ~kActorIdHandleMask) != kActorIdTag) {
			return false;
		}
		a_handle = a_id & kActorIdHandleMask;
		return true;
	}

	// Vehicles go out as a row of up to proto::kActorVehicleSegments records along their axis (see
	// proto::kActorVehicle): formId = 'V' tag | (vehicle script handle << 4) | piece. Vehicle handles
	// are (pool slot << 8) | generation, far below 2^20.
	inline constexpr std::uint32_t kVehicleIdHandleMask = 0x000FFFFFu;
	inline constexpr std::uint32_t kVehicleIdPieceMask = 0xFu;

	inline std::uint32_t VehicleActorId(std::uint32_t a_handle, std::uint32_t a_piece)
	{
		return proto::kActorVehicleTag | ((a_handle & kVehicleIdHandleMask) << 4) | (a_piece & kVehicleIdPieceMask);
	}

	inline bool VehicleFromActorId(std::uint32_t a_id, std::uint32_t& a_handle, std::uint32_t& a_piece)
	{
		if ((a_id & 0xFF000000u) != proto::kActorVehicleTag) {
			return false;
		}
		a_handle = (a_id >> 4) & kVehicleIdHandleMask;
		a_piece = a_id & kVehicleIdPieceMask;
		return true;
	}

	// How far (m) a vehicle's stand-in boxes may stick out of its body, seen from above (more only for
	// a long vehicle turned well away from the world axes, when the boxes run out: VehicleSegments).
	inline constexpr float kVehicleBoxSlack = 0.05f;
	// How deep (m) the notches between two boxes along a side may reach into the body.
	inline constexpr float kVehicleNotch = 0.2f;

	// A vehicle of a_length x a_width metres as a row of upright boxes square to the world axes (all
	// Minecraft entity boxes are): the vehicle's axis makes an angle with the world axes, and a_e is
	// |cos| + |sin| of it (1 when it lies along an axis, sqrt 2 at 45 degrees). A square box of side s
	// then reaches s/2 * a_e out from its centre along the vehicle's axis and across it, so boxes of
	// side (width + 2 slack) / a_e, centred on the axis no further than (length - width) / 2 from the
	// middle, stay within the outline grown by the slack, whichever way the vehicle faces. They are
	// spaced closely enough (at most half a box) that the notches between them along the sides stay
	// within kVehicleNotch; when a_maxCount boxes can't do that, the slack grows instead. Turned away
	// from the axes, the row leaves the corners open, so then four small boxes fill them (a_corners).
	// Piece i is centred offset[i] metres ahead of the middle and side[i] to its right (0 is the front
	// piece), size[i] wide and height[i] tall (VehiclePieces; VehicleSegments leaves 0).
	struct VehicleLayout
	{
		std::uint32_t count = 1;
		float         offset[proto::kActorVehicleSegments]{};
		float         side[proto::kActorVehicleSegments]{};
		float         size[proto::kActorVehicleSegments]{};
		float         height[proto::kActorVehicleSegments]{};
		float         boxWidth = 0.0f;  // the row's boxes
		float         slack = 0.0f;     // what the row's boxes may stick out by
		std::uint32_t bodyCount = 1;    // VehiclePieces: the body row (and its corners) come first, then the cabin row
	};

	inline constexpr float kVehicleCornerInset = 0.45f;  // corner boxes' centres from the two faces

	inline VehicleLayout VehicleSegments(float a_length, float a_width, float a_e = 1.0f, float a_slack = kVehicleBoxSlack,
		bool a_corners = true, std::uint32_t a_maxCount = proto::kActorVehicleSegments)
	{
		VehicleLayout l;
		const float length = std::max(a_length, 0.5f), width = std::max(std::min(a_width, length), 0.5f);
		const float e = std::clamp(a_e, 1.0f, 1.41422f);
		const float reach = std::max(0.0f, (length - width) * 0.5f);  // the end pieces' centres
		const bool  corners = a_corners && e > 1.05f && length > 2.0f * kVehicleCornerInset + 0.2f && width > 2.0f * kVehicleCornerInset + 0.2f;
		const int   maxRow = std::max(1, static_cast<int>(std::min(a_maxCount, proto::kActorVehicleSegments)) - (corners ? 4 : 0));
		// Between two boxes the outline's side shows a notch (d sin 2a) / 2 - slack deep for spacing d
		// at angle a (sin 2a = e^2 - 1): keep it within kVehicleNotch.
		float slack = a_slack;
		float spacing = std::min((width + 2.0f * slack) / e * 0.5f, 2.0f * (kVehicleNotch + slack) / std::max(e * e - 1.0f, 1e-3f));
		int   n = reach > 1e-3f ? 1 + static_cast<int>(std::ceil(2.0f * reach / spacing - 1e-3f)) : 1;
		if (n > maxRow) {
			// Out of boxes: spread them out and let them stick out a little more.
			n = maxRow;
			spacing = n > 1 ? 2.0f * reach / static_cast<float>(n - 1) : 0.0f;
			slack = std::max(slack, spacing * (e * e - 1.0f) * 0.5f - kVehicleNotch);
		}
		l.slack = slack;
		l.boxWidth = (width + 2.0f * slack) / e;
		l.count = static_cast<std::uint32_t>(std::max(n, 1));
		for (std::uint32_t i = 0; i < l.count; ++i) {
			l.offset[i] = l.count == 1 ? 0.0f : reach - 2.0f * reach * static_cast<float>(i) / static_cast<float>(l.count - 1);
			l.size[i] = l.boxWidth;
		}
		if (corners) {
			const float cornerSize = 2.0f * (kVehicleCornerInset + slack) / e;
			for (int k = 0; k < 4; ++k, ++l.count) {
				l.offset[l.count] = (k < 2 ? 1.0f : -1.0f) * (length * 0.5f - kVehicleCornerInset);
				l.side[l.count] = (k % 2 ? 1.0f : -1.0f) * (width * 0.5f - kVehicleCornerInset);
				l.size[l.count] = cornerSize;
			}
		}
		l.bodyCount = l.count;
		return l;
	}

	// ---- a vehicle's shape ------------------------------------------------------------------------------
	// In its own frame (x right, y forward, z up, metres from its origin), as measured in game with
	// line probes against its collision (Combat.cpp ProbeShape), or guessed from its model box
	// (GuessShape): the body's ends and half width without the mirrors, the greenhouse's half width,
	// and the top's height along it, in slices from the tail to the nose.
	inline constexpr int kShapeSlices = 40;

	struct VehicleShape
	{
		float bottom = 0.0f, top = 0.0f;  // z: the wheels' bottoms, the highest point
		float tail = 0.0f, nose = 0.0f;   // y
		float centreX = 0.0f;
		float halfWidth = 0.5f;
		float cabinHalfWidth = 0.0f;      // 0: no cabin row (bikes, boats: one row as tall as the vehicle)
		int   slices = 1;
		float tops[kShapeSlices]{};       // z of the top over slice i (y from tail + i d to tail + (i + 1) d)

		float Length() const { return nose - tail; }
		float Middle() const { return (nose + tail) * 0.5f; }
		float SliceLength() const { return Length() / static_cast<float>(std::max(slices, 1)); }
	};

	// The lowest, highest or mean top over the slices [a_from, a_to] reaches (y, the vehicle's frame);
	// a_none if that misses the body.
	enum class TopOf : int
	{
		kLowest,
		kHighest,
		kMean,
	};

	inline float ShapeTop(const VehicleShape& a_s, float a_from, float a_to, TopOf a_of, float a_none)
	{
		const float d = a_s.SliceLength();
		if (d <= 1e-4f || a_to < a_from) {
			return a_none;
		}
		const int first = std::max(0, static_cast<int>(std::floor((a_from - a_s.tail) / d + 1e-4f)));
		const int last = std::min(a_s.slices - 1, static_cast<int>(std::ceil((a_to - a_s.tail) / d - 1e-4f)) - 1);
		if (first > last) {
			return a_none;
		}
		float v = a_s.tops[first], sum = v;
		for (int i = first + 1; i <= last; ++i) {
			v = a_of == TopOf::kLowest ? std::min(v, a_s.tops[i]) : std::max(v, a_s.tops[i]);
			sum += a_s.tops[i];
		}
		return a_of == TopOf::kMean ? sum / static_cast<float>(last - first + 1) : v;
	}

	// A typical car's shape in a model box [a_lo, a_hi]: mirrors 8 cm out on each side; a saloon's
	// profile from the front: bonnet (a quarter of the length, at two thirds of the height), windscreen,
	// roof, rear window, boot (at seven tenths). For a car never measured.
	inline VehicleShape GuessShape(const float a_lo[3], const float a_hi[3], bool a_car)
	{
		VehicleShape s;
		s.bottom = a_lo[2];
		s.top = a_hi[2];
		s.tail = a_lo[1];
		s.nose = a_hi[1];
		s.centreX = (a_lo[0] + a_hi[0]) * 0.5f;
		const float w = a_hi[0] - a_lo[0], h = a_hi[2] - a_lo[2];
		s.slices = kShapeSlices;
		if (!a_car) {
			s.halfWidth = w * 0.5f;
			for (int i = 0; i < s.slices; ++i) {
				s.tops[i] = a_hi[2];
			}
			return s;
		}
		s.halfWidth = std::max(w * 0.5f - 0.08f, w * 0.4f);
		s.cabinHalfWidth = s.halfWidth * 0.78f;
		// From the tail: boot, rear window, roof, windscreen, bonnet (fractions of the length, of the height).
		static constexpr float kAt[] = { 0.0f, 0.18f, 0.28f, 0.60f, 0.72f, 1.0f };
		static constexpr float kTop[] = { 0.70f, 0.70f, 1.0f, 1.0f, 0.66f, 0.66f };
		for (int i = 0; i < s.slices; ++i) {
			const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(s.slices);
			int k = 0;
			while (k < 4 && u > kAt[k + 1]) {
				++k;
			}
			const float t = (u - kAt[k]) / std::max(kAt[k + 1] - kAt[k], 1e-4f);
			s.tops[i] = a_lo[2] + h * (kTop[k] + (kTop[k + 1] - kTop[k]) * std::clamp(t, 0.0f, 1.0f));
		}
		return s;
	}

	// The bumpers round off: within this of either end the profile doesn't lower a piece.
	inline constexpr float kShapeEndTrim = 0.3f;
	// The cabin row is for what rises this far above the body row (its median height).
	inline constexpr float kCabinRise = 0.15f;
	inline constexpr std::uint32_t kMaxCabinPieces = 4;

	// A vehicle's stand-in boxes (see VehicleSegments) for its shape, at |cos| + |sin| a_e of its
	// heading: a row as wide as the body, each box as tall as the body's lowest top along it (the
	// boxes never reach above the body: a hit over the bonnet goes past it), then, where the
	// greenhouse rises above that row, a narrower row within the greenhouse (never over the bonnet or
	// the boot), each box as tall as the greenhouse's mean top along it. Offsets are from the body's
	// middle (VehicleShape::Middle, centreX), heights from its bottom.
	inline VehicleLayout VehiclePieces(const VehicleShape& a_s, float a_e, float a_slack = kVehicleBoxSlack)
	{
		const float length = a_s.Length(), mid = a_s.Middle();
		const bool  cabin = a_s.cabinHalfWidth > 0.2f;
		VehicleLayout l = VehicleSegments(length, 2.0f * a_s.halfWidth, a_e, a_slack, true, proto::kActorVehicleSegments - (cabin ? kMaxCabinPieces : 0u));
		const float e = std::clamp(a_e, 1.0f, 1.41422f);
		const float full = a_s.top - a_s.bottom;
		float       heights[proto::kActorVehicleSegments];
		for (std::uint32_t i = 0; i < l.count; ++i) {
			// The box's reach along the axis, within the body less its rounded ends.
			const float c = mid + l.offset[i], ext = l.size[i] * 0.5f * e;
			const float from = std::max(c - ext, a_s.tail + kShapeEndTrim), to = std::min(c + ext, a_s.nose - kShapeEndTrim);
			const float t = from < to ? ShapeTop(a_s, from, to, TopOf::kLowest, a_s.top) : ShapeTop(a_s, c - ext, c + ext, TopOf::kHighest, a_s.top);
			l.height[i] = std::clamp(t - a_s.bottom, std::min(0.5f, full), full);
			heights[i] = l.height[i];
		}
		if (!cabin) {
			return l;
		}
		// Where the greenhouse rises above the body row's typical height (its median: a box lying
		// wholly under the cabin is taller than the rest).
		std::sort(heights, heights + l.count);
		const float bodyTop = heights[l.count / 2];
		int first = -1, last = -1;
		for (int i = 0; i < a_s.slices; ++i) {
			if (a_s.tops[i] - a_s.bottom >= bodyTop + kCabinRise) {
				first = first < 0 ? i : first;
				last = i;
			}
		}
		if (first < 0) {
			return l;
		}
		const float d = a_s.SliceLength();
		const float rear = a_s.tail + d * static_cast<float>(first), front = a_s.tail + d * static_cast<float>(last + 1);
		// (No slack: the boxes stay within the greenhouse, seen from above.)
		const VehicleLayout c = VehicleSegments(front - rear, 2.0f * a_s.cabinHalfWidth, a_e, 0.0f, false, std::min(kMaxCabinPieces, proto::kActorVehicleSegments - l.count));
		const float cmid = (front + rear) * 0.5f;
		for (std::uint32_t i = 0; i < c.count && l.count < proto::kActorVehicleSegments; ++i, ++l.count) {
			const float at = cmid + c.offset[i], ext = c.size[i] * 0.5f * e;
			l.offset[l.count] = at - mid;
			l.side[l.count] = 0.0f;
			l.size[l.count] = c.size[i];
			l.height[l.count] = std::clamp(ShapeTop(a_s, at - ext, at + ext, TopOf::kMean, a_s.bottom + bodyTop) - a_s.bottom, bodyTop, full);
		}
		return l;
	}

	// ---- where a hit on a vehicle landed --------------------------------------------------------------
	// In the vehicle's own frame (x right, y forward, z up, metres from its origin), against its model
	// box [a_lo, a_hi] (GET_MODEL_DIMENSIONS).
	enum CarFace : int
	{
		kFaceLeft = 0,
		kFaceRight = 1,
		kFaceRear = 2,
		kFaceFront = 3,
		kFaceBottom = 4,
		kFaceTop = 5,
	};

	// Where the ray from a_o along a_d (any length) first enters the box: its parameter and the face.
	// False if it misses or starts inside.
	inline bool RayEntersBox(const float a_o[3], const float a_d[3], const float a_lo[3], const float a_hi[3], float& a_t, int& a_face)
	{
		float tEnter = -1e30f, tExit = 1e30f;
		int   face = -1;
		for (int axis = 0; axis < 3; ++axis) {
			if (std::fabs(a_d[axis]) < 1e-9f) {
				if (a_o[axis] < a_lo[axis] || a_o[axis] > a_hi[axis]) {
					return false;
				}
				continue;
			}
			float t0 = (a_lo[axis] - a_o[axis]) / a_d[axis], t1 = (a_hi[axis] - a_o[axis]) / a_d[axis];
			int   f0 = axis * 2, f1 = axis * 2 + 1;  // entering through lo is the -axis face
			if (t0 > t1) {
				std::swap(t0, t1);
				std::swap(f0, f1);
			}
			if (t0 > tEnter) {
				tEnter = t0;
				face = f0;
			}
			tExit = std::min(tExit, t1);
		}
		if (face < 0 || tEnter > tExit || tEnter < 0.0f) {
			return false;
		}
		a_t = tEnter;
		a_face = face;
		return true;
	}

	// GTA IV's windows (eVehicleWindow, SMASH_CAR_WINDOW) and seats (-1 driver, 0.. passengers).
	enum CarWindow : int
	{
		kWindowNone = -1,  // the body
		kWindowLF = 0,
		kWindowRF = 1,
		kWindowLR = 2,
		kWindowRR = 3,
		kWindscreen = 4,
		kWindscreenRear = 5,
	};
	inline constexpr int kSeatDriver = -1;
	inline constexpr int kSeatNone = -2;

	struct CarStrike
	{
		int window = kWindowNone;
		int seat = kSeatNone;  // who sits behind that glass
	};

	// The glass of a car, from its model box: the windows start at kBeltline of its height (the
	// wheels count: the box reaches the ground); the side windows of the front doors span the
	// middle of its length, the rear doors' the part behind (the front one ends at the A pillar, the
	// rear one at the C pillar). Fractions of length run 0 at the back to 1 at the front.
	inline constexpr float kBeltline = 0.58f;
	inline constexpr float kFrontWindowFrom = 0.47f, kFrontWindowTo = 0.76f;
	inline constexpr float kRearWindowFrom = 0.20f;
	inline constexpr float kWindscreenFrom = 0.62f, kWindscreenTo = 0.78f;  // seen from above
	inline constexpr float kRearScreenFrom = 0.18f, kRearScreenTo = 0.30f;

	inline CarStrike ClassifyCarStrike(int a_face, const float a_p[3], const float a_lo[3], const float a_hi[3])
	{
		CarStrike s;
		const float len = std::max(a_hi[1] - a_lo[1], 0.1f), height = std::max(a_hi[2] - a_lo[2], 0.1f);
		const float along = (a_p[1] - a_lo[1]) / len;     // 0 rear .. 1 front
		const float up = (a_p[2] - a_lo[2]) / height;      // 0 ground .. 1 roof
		const bool  left = a_p[0] < (a_lo[0] + a_hi[0]) * 0.5f;
		const bool  glassHeight = up >= kBeltline;
		switch (a_face) {
		case kFaceLeft:
		case kFaceRight:
			if (glassHeight && along >= kFrontWindowFrom && along <= kFrontWindowTo) {
				s.window = a_face == kFaceLeft ? kWindowLF : kWindowRF;
			} else if (glassHeight && along >= kRearWindowFrom && along < kFrontWindowFrom) {
				s.window = a_face == kFaceLeft ? kWindowLR : kWindowRR;
			}
			break;
		case kFaceFront:
			s.window = glassHeight ? kWindscreen : kWindowNone;
			break;
		case kFaceRear:
			s.window = glassHeight ? kWindscreenRear : kWindowNone;
			break;
		case kFaceTop:
			if (along >= kWindscreenFrom && along <= kWindscreenTo) {
				s.window = kWindscreen;
			} else if (along >= kRearScreenFrom && along <= kRearScreenTo) {
				s.window = kWindscreenRear;
			}
			break;
		default:
			break;
		}
		switch (s.window) {
		case kWindowLF:
			s.seat = kSeatDriver;
			break;
		case kWindowRF:
			s.seat = 0;
			break;
		case kWindowLR:
			s.seat = 1;
			break;
		case kWindowRR:
			s.seat = 2;
			break;
		case kWindscreen:
			s.seat = left ? kSeatDriver : 0;
			break;
		case kWindscreenRear:
			s.seat = left ? 1 : 2;
			break;
		default:
			break;
		}
		return s;
	}

	// The hit from kEvHitPoint (a point on the line it came along, MC, and that line's MC yaw/pitch)
	// in the vehicle's frame: a_right/a_fwd/a_up are its unit axes in GTA space, a_pos its origin.
	// Follows the line from a few metres back into the model box. False if it misses the box (it
	// grazed a stand-in box where the vehicle isn't): a body hit, then.
	inline bool StrikeOnCar(const double a_mcPoint[3], float a_mcYaw, float a_mcPitch, const float a_pos[3], const float a_right[3], const float a_fwd[3],
		const float a_up[3], const float a_lo[3], const float a_hi[3], int& a_face, float a_local[3])
	{
		// MC yaw/pitch -> a GTA direction (Coords.h: heading = 180 - yaw; MC pitch positive = down).
		const float h = (180.0f - a_mcYaw) * 3.14159265358979f / 180.0f, p = -a_mcPitch * 3.14159265358979f / 180.0f;
		const float dir[3] = { -std::sin(h) * std::cos(p), std::cos(h) * std::cos(p), std::sin(p) };
		const float gp[3] = { static_cast<float>(a_mcPoint[0]), static_cast<float>(-a_mcPoint[2]), static_cast<float>(a_mcPoint[1]) };
		constexpr float kBack = 4.0f;
		float rel[3], o[3], d[3];
		for (int i = 0; i < 3; ++i) {
			rel[i] = gp[i] - dir[i] * kBack - a_pos[i];
		}
		auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
		o[0] = dot(rel, a_right), o[1] = dot(rel, a_fwd), o[2] = dot(rel, a_up);
		d[0] = dot(dir, a_right), d[1] = dot(dir, a_fwd), d[2] = dot(dir, a_up);
		float t = 0.0f;
		if (!RayEntersBox(o, d, a_lo, a_hi, t, a_face)) {
			return false;
		}
		for (int i = 0; i < 3; ++i) {
			a_local[i] = o[i] + d[i] * t;
		}
		return true;
	}

	// ---- GTA IV weapon types (Scripting::eWeapon; CPhysical::m_nLastDamageWeapon) ---------------
	enum Weapon : int
	{
		kWeaponUnarmed = 0,
		kWeaponBat = 1,
		kWeaponPoolCue = 2,
		kWeaponKnife = 3,
		kWeaponGrenade = 4,
		kWeaponMolotov = 5,
		kWeaponRocket = 6,
		kWeaponPistol = 7,
		kWeaponSniperM40A1 = 17,
		kWeaponRocketLauncher = 18,
		kWeaponFlameThrower = 19,
		kWeaponMinigun = 20,
		kWeaponRammedByCar = 49,
		kWeaponRunOverByCar = 50,
		kWeaponExplosion = 51,
		kWeaponUziDriveby = 52,
		kWeaponDrowning = 53,
		kWeaponFall = 54,
		kWeaponAnyMelee = 56,
	};

	enum class HurtClass
	{
		kMelee,       // proto::kHurtMelee
		kProjectile,  // proto::kHurtProjectile
		kOther,       // proto::kHurtOther: explosions, fire, vehicles, anything unknown
		kIgnore,      // Minecraft simulates this itself: falls, drowning
	};

	// Falls and drowning are dropped: Minecraft owns the player's physics, air and fall damage
	// (and the puppeted ped is frozen, so GTA IV shouldn't produce them anyway).
	inline HurtClass ClassifyWeapon(int a_weapon)
	{
		if ((a_weapon >= kWeaponUnarmed && a_weapon <= kWeaponKnife) || a_weapon == kWeaponAnyMelee) {
			return HurtClass::kMelee;
		}
		if ((a_weapon >= kWeaponPistol && a_weapon <= kWeaponSniperM40A1) || a_weapon == kWeaponMinigun || a_weapon == kWeaponUziDriveby) {
			return HurtClass::kProjectile;
		}
		if (a_weapon == kWeaponFall || a_weapon == kWeaponDrowning) {
			return HurtClass::kIgnore;
		}
		return HurtClass::kOther;
	}

	inline proto::HurtKind HurtKindOf(HurtClass a_class)
	{
		switch (a_class) {
		case HurtClass::kMelee:
			return proto::kHurtMelee;
		case HurtClass::kProjectile:
			return proto::kHurtProjectile;
		default:
			return proto::kHurtOther;
		}
	}

	// ---- damage scaling -------------------------------------------------------------------------
	// Minecraft hit on a ped: MC damage (half-hearts, after MC's own modifiers) * PedDamageScale = GTA
	// health points. Anything that did damage takes at least 1.
	inline std::uint32_t PedDamageFromMc(float a_mcDamage, float a_pedDamageScale)
	{
		if (!(a_mcDamage > 0.0f) || !(a_pedDamageScale > 0.0f)) {
			return 0;
		}
		const float hp = std::min(a_mcDamage * a_pedDamageScale, 100000.0f);
		return std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(std::lround(hp)));
	}

	// HostCombat.HOST_TO_MC_DAMAGE on the Java side: Minecraft damage = (InputEvent a / 100) / 5.
	inline constexpr float kJavaHostToMcDamage = 5.0f;

	// GTA damage to the player -> Minecraft damage (half-hearts): GTA health / PlayerDamageScale.
	inline float McDamageFromGta(float a_gtaDamage, float a_playerDamageScale)
	{
		return a_gtaDamage > 0.0f && a_playerDamageScale > 0.0f ? a_gtaDamage / a_playerDamageScale : 0.0f;
	}

	// The "host damage" kInHurt carries (InputEvent a = this * 100) so that Minecraft ends up
	// taking a_mcDamage.
	inline float HostDamageForMc(float a_mcDamage) { return a_mcDamage * kJavaHostToMcDamage; }

	// ---- health ---------------------------------------------------------------------------------
	// GTA IV peds die at or below a_deathHealth (the player and ambient peds: 100 of 200).
	inline float HealthFraction(float a_health, float a_maxHealth, float a_deathHealth)
	{
		const float span = a_maxHealth - a_deathHealth;
		if (!(span > 0.0f)) {
			return a_health > a_deathHealth ? 1.0f : 0.0f;
		}
		return std::clamp((a_health - a_deathHealth) / span, 0.0f, 1.0f);
	}

	// Health and armour the player lost since the last refill (gains don't count).
	inline float Deficit(float a_baseHealth, float a_health, float a_baseArmour, float a_armour)
	{
		return std::max(0.0f, a_baseHealth - a_health) + std::max(0.0f, a_baseArmour - a_armour);
	}

	// ---- knockback --------------------------------------------------------------------------------
	// kEvHitActor b/c: the push direction in MC (x, z). GTA (x, y) = (x, -z), normalised; false if
	// there is no direction.
	inline bool PushDirToGta(float a_mcX, float a_mcZ, float& a_gtaX, float& a_gtaY)
	{
		const float len = std::sqrt(a_mcX * a_mcX + a_mcZ * a_mcZ);
		if (!(len > 1e-4f)) {
			a_gtaX = a_gtaY = 0.0f;
			return false;
		}
		a_gtaX = a_mcX / len;
		a_gtaY = -a_mcZ / len;
		return true;
	}

	// APPLY_FORCE_TO_PED's direction, if it is taken in the ped's own frame (x right, y forward): a
	// GTA world direction (a_gx, a_gy) for a ped facing a_headingDeg (GTA heading: 0 = north,
	// counter-clockwise; forward = (-sin h, cos h), right = (cos h, sin h)).
	inline void WorldToPedLocal(float a_gx, float a_gy, float a_headingDeg, float& a_lx, float& a_ly)
	{
		const float h = a_headingDeg * 3.14159265358979f / 180.0f;
		const float s = std::sin(h), c = std::cos(h);
		a_lx = a_gx * c + a_gy * s;   // . right
		a_ly = -a_gx * s + a_gy * c;  // . forward
	}

	// Angle (degrees, 0 to 180) between two horizontal directions; 180 if either is zero.
	inline float AngleBetween(float a_x0, float a_y0, float a_x1, float a_y1)
	{
		const float l0 = std::sqrt(a_x0 * a_x0 + a_y0 * a_y0), l1 = std::sqrt(a_x1 * a_x1 + a_y1 * a_y1);
		if (!(l0 > 1e-6f) || !(l1 > 1e-6f)) {
			return 180.0f;
		}
		const float c = std::clamp((a_x0 * a_x1 + a_y0 * a_y1) / (l0 * l1), -1.0f, 1.0f);
		return std::acos(c) * 180.0f / 3.14159265358979f;
	}

	// How long a hit knocks a ped over: Minecraft's base knockback (0.4) ~0.8 s, a sprint hit or a
	// Knockback enchantment longer, a critical at least 1.2 s. 0: no knockback, no ragdoll.
	inline int RagdollMs(float a_strength, bool a_critical)
	{
		if (!(a_strength > 0.0f) && !a_critical) {
			return 0;
		}
		int ms = static_cast<int>(std::lround(std::clamp(a_strength, 0.0f, 2.0f) * 2000.0f));
		if (a_critical) {
			ms = std::max(ms, 1200);
		}
		return std::clamp(ms, 500, 3000);
	}

	// ---- vehicles -------------------------------------------------------------------------------
	// Minecraft damage on a vehicle -> GTA IV body/engine health points (cars have 1000 of each; the
	// engine burns below 0 and the car blows up soon after).
	inline float VehicleDamageFromMc(float a_mcDamage, float a_scale)
	{
		if (!(a_mcDamage > 0.0f) || !(a_scale > 0.0f)) {
			return 0.0f;
		}
		return std::min(a_mcDamage * a_scale, 100000.0f);
	}

	// ---- crimes (GtaCrimes) ----------------------------------------------------------------------
	// Minecraft's hits never go through GTA IV's own damage path with the player as the attacker, so
	// its crime system never sees them. The rules it would apply, roughly: hurting a cop is always a
	// crime (1 star, killing one 2); an assault the police see is 1 star; a killing seen by the
	// police is 2, seen only by bystanders (they phone it in) 1. Returns the wanted level the player
	// should have now (never lower than a_wanted).
	inline constexpr float kCopSightMetres = 40.0f;
	inline constexpr float kWitnessMetres = 25.0f;

	inline unsigned WantedAfterAttack(unsigned a_wanted, bool a_victimCop, bool a_killed, unsigned a_copsNear, unsigned a_witnessesNear)
	{
		unsigned w = 0;
		if (a_victimCop) {
			w = a_killed ? 2u : 1u;
		} else if (a_killed) {
			w = a_copsNear ? 2u : a_witnessesNear ? 1u : 0u;
		} else if (a_copsNear) {
			w = 1u;
		}
		return std::max(a_wanted, w);
	}

	// How a ped Minecraft hurt (and that survived) reacts: cops and gang members fight back, of the
	// rest about one in three does and the others run (a_seed: anything stable per ped).
	enum class Reaction
	{
		kFight,
		kFlee,
	};

	inline Reaction ReactionOf(bool a_cop, bool a_tough, std::uint32_t a_seed)
	{
		if (a_cop || a_tough) {
			return Reaction::kFight;
		}
		return (a_seed * 2654435761u >> 16) % 3u == 0u ? Reaction::kFight : Reaction::kFlee;
	}

	// ---- explosions -----------------------------------------------------------------------------
	// Minecraft explosion radius (blocks; TNT 4, creeper 3, charged creeper 6) -> ADD_EXPLOSION radius (m).
	inline float ExplosionRadius(float a_mcRadius, float a_scale)
	{
		if (!(a_mcRadius > 0.0f) || !(a_scale > 0.0f)) {
			return 0.0f;
		}
		return std::clamp(a_mcRadius * a_scale, 0.5f, 30.0f);
	}

	// Camera shake of a blast at a_distance metres: full within the radius, fading out at 6 radii.
	inline float ExplosionShake(float a_radius, float a_distance)
	{
		if (!(a_radius > 0.0f)) {
			return 0.0f;
		}
		const float fadeEnd = a_radius * 6.0f;
		return std::clamp(1.0f - (a_distance - a_radius) / (fadeEnd - a_radius), 0.0f, 1.0f);
	}

	// ---- where a hurt came from (proto::kHurtHasDirection) -----------------------------------------
	// The HurtFlags for a hit that came from GTA direction (a_dx, a_dy) (from the player toward the
	// source): the MC yaw of that direction in bits 16 to 24. 0 if there is no direction.
	inline std::uint32_t HurtDirectionFlags(float a_dx, float a_dy)
	{
		if (!(a_dx * a_dx + a_dy * a_dy > 1e-6f)) {
			return 0;
		}
		// MC (x, z) = GTA (x, -y); MC yaw faces (-sin yaw, cos yaw).
		const float yaw = std::atan2(-a_dx, -a_dy) * 180.0f / 3.14159265358979f;
		const int   deg = (static_cast<int>(std::lround(yaw)) % 360 + 360) % 360;
		return proto::kHurtHasDirection | (static_cast<std::uint32_t>(deg) << proto::kHurtDirectionShift);
	}

	// The MC yaw (0 to 359) toward the source in a_flags (HurtDirectionFlags), or -1.
	inline int HurtSourceYaw(std::uint32_t a_flags)
	{
		return (a_flags & proto::kHurtHasDirection) ? static_cast<int>((a_flags >> proto::kHurtDirectionShift) & 0x1FFu) : -1;
	}

	// Minecraft's raised shield (proto::kMcBlocking) blocks what comes from within a_halfArc degrees of
	// where the player looks: a_lookYaw, MC degrees, against the MC yaw toward the source.
	inline bool ShieldCovers(float a_lookYaw, float a_sourceYaw, float a_halfArc = 90.0f)
	{
		float d = std::fmod(a_sourceYaw - a_lookYaw, 360.0f);
		d = d < -180.0f ? d + 360.0f : d > 180.0f ? d - 360.0f : d;
		return std::fabs(d) < a_halfArc;
	}

	// ---- Minecraft's vitals on GTA's HUD (proto::kMcVitalsValid) --------------------------------------
	struct McVitals
	{
		bool  valid = false;
		float health = 0.0f, maxHealth = 0.0f, absorption = 0.0f;
		int   armour = 0;
	};

	inline McVitals DecodeVitals(std::uint32_t a_healthWord, std::uint32_t a_armourWord)
	{
		McVitals v;
		v.valid = (a_armourWord & proto::kMcVitalsValid) != 0 && (a_healthWord >> 16) != 0;
		if (v.valid) {
			v.health = static_cast<float>(a_healthWord & 0xFFFFu) / 100.0f;
			v.maxHealth = static_cast<float>(a_healthWord >> 16) / 100.0f;
			v.armour = static_cast<int>(a_armourWord & 0xFFu);
			v.absorption = static_cast<float>((a_armourWord >> 8) & 0xFFu);
		}
		return v;
	}

	// The health GTA IV's HUD is shown for the puppeted player (whose real health stays at the buffer):
	// with the max health it divides by at 200, the arcs read (health - 100) / 100, so Minecraft's
	// health fraction a_fraction is 100 + 100 x a_fraction (100: dead, empty).
	inline float HudHealth(float a_fraction)
	{
		return 100.0f + 100.0f * std::clamp(a_fraction, 0.0f, 1.0f);
	}

	// GTA armour (0 to a_maxArmour) for a_points of Minecraft armour (0 to 20, a full diamond set).
	inline int MirroredArmour(int a_points, int a_maxArmour)
	{
		return std::clamp(static_cast<int>(std::lround(static_cast<double>(std::clamp(a_points, 0, 20)) * a_maxArmour / 20.0)), 0, std::max(a_maxArmour, 0));
	}

	// ---- pacing GTA damage for Minecraft --------------------------------------------------------
	// Minecraft ignores damage for 10 ticks (0.5 s) after a hurt unless it's bigger than that hurt,
	// so GTA's rapid hits (a burst of fire, a fire's per-frame damage) would mostly be lost. The
	// first hit after a quiet spell goes out at once; whatever lands during the next kWindow seconds
	// is summed and goes out when it ends. The batch keeps the kind and attacker of its biggest hit.
	class HurtPacer
	{
	public:
		static constexpr float kWindow = 0.55f;
		static constexpr float kMinGtaDamage = 0.5f;  // don't send crumbs; they wait for more

		struct Batch
		{
			float           damage = 0.0f;  // GTA health points
			proto::HurtKind kind = proto::kHurtOther;
			std::uint32_t   attacker = 0;
			std::uint32_t   flags = 0;  // proto::HurtFlags of the biggest hit (its direction)
			std::uint32_t   hits = 0;
			float           biggest = 0.0f;
		};

		void Add(proto::HurtKind a_kind, float a_damage, std::uint32_t a_attacker, std::uint32_t a_flags = 0)
		{
			if (!(a_damage > 0.0f)) {
				return;
			}
			pending_.damage += a_damage;
			++pending_.hits;
			if (a_damage > pending_.biggest) {
				pending_.biggest = a_damage;
				pending_.kind = a_kind;
				pending_.attacker = a_attacker ? a_attacker : pending_.attacker;
				pending_.flags = a_flags;
			} else if (!pending_.attacker) {
				pending_.attacker = a_attacker;
			}
		}

		// Once per frame after the frame's Add()s. True: send a_out now.
		bool Tick(float a_dt, Batch& a_out)
		{
			sinceSend_ += std::max(a_dt, 0.0f);
			if (pending_.damage < kMinGtaDamage || sinceSend_ < kWindow) {
				return false;
			}
			a_out = pending_;
			pending_ = Batch{};
			sinceSend_ = 0.0f;
			return true;
		}

		void Reset()
		{
			pending_ = Batch{};
			sinceSend_ = kWindow;
		}

		[[nodiscard]] float Pending() const { return pending_.damage; }

	private:
		Batch pending_{};
		float sinceSend_ = kWindow;  // the first hit goes out at once
	};
}
