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

	// How far (m) a vehicle's stand-in boxes may stick out of its real outline, seen from above.
	inline constexpr float kVehicleBoxSlack = 0.2f;

	// A vehicle of a_length x a_width metres as a row of upright boxes square to the world axes (all
	// Minecraft entity boxes are): the vehicle's axis makes an angle with the world axes, and a_e is
	// |cos| + |sin| of it (1 when it lies along an axis, sqrt 2 at 45 degrees). A square box of side s
	// then reaches s/2 * a_e out from its centre along the vehicle's axis and across it, so boxes of
	// side (width + 2 slack) / a_e, centred on the axis no further than (length - width) / 2 from the
	// middle, stay within the outline grown by a_slack, whichever way the vehicle faces. They are
	// spaced closely enough (at most half a box) that the gaps between them along the sides stay
	// within 0.2 m. Turned away from the axes, the row leaves the corners open, so then four small
	// boxes fill them. Piece i is centred a_offset[i] metres ahead of the middle and a_side[i] to its
	// right (0 is the front piece), a_size[i] wide.
	struct VehicleLayout
	{
		std::uint32_t count = 1;
		float         offset[proto::kActorVehicleSegments]{};
		float         side[proto::kActorVehicleSegments]{};
		float         size[proto::kActorVehicleSegments]{};
		float         boxWidth = 0.0f;  // the row's boxes
	};

	inline constexpr float kVehicleCornerInset = 0.45f;  // corner boxes' centres from the two faces

	inline VehicleLayout VehicleSegments(float a_length, float a_width, float a_e = 1.0f, float a_slack = kVehicleBoxSlack)
	{
		VehicleLayout l;
		const float length = std::max(a_length, 0.5f), width = std::max(std::min(a_width, length), 0.5f);
		const float e = std::clamp(a_e, 1.0f, 1.41422f);
		l.boxWidth = (width + 2.0f * a_slack) / e;
		const float reach = std::max(0.0f, (length - width) * 0.5f);  // the end pieces' centres
		// Between two boxes the outline's side shows a notch (d sin 2a) / 2 - slack deep for spacing d
		// at angle a (sin 2a = e^2 - 1): keep it within 0.2 m.
		const float spacing = std::min(l.boxWidth * 0.5f, 2.0f * (0.2f + a_slack) / std::max(e * e - 1.0f, 1e-3f));
		const int   n = reach > 1e-3f ? 1 + static_cast<int>(std::ceil(2.0f * reach / spacing - 1e-3f)) : 1;
		const bool corners = e > 1.05f && length > 2.0f * kVehicleCornerInset + 0.2f && width > 2.0f * kVehicleCornerInset + 0.2f;
		const int  maxRow = static_cast<int>(proto::kActorVehicleSegments) - (corners ? 4 : 0);
		l.count = static_cast<std::uint32_t>(std::clamp(n, 1, maxRow));
		for (std::uint32_t i = 0; i < l.count; ++i) {
			l.offset[i] = l.count == 1 ? 0.0f : reach - 2.0f * reach * static_cast<float>(i) / static_cast<float>(l.count - 1);
			l.size[i] = l.boxWidth;
		}
		if (corners) {
			const float cornerSize = 2.0f * (kVehicleCornerInset + a_slack) / e;
			for (int k = 0; k < 4; ++k, ++l.count) {
				l.offset[l.count] = (k < 2 ? 1.0f : -1.0f) * (length * 0.5f - kVehicleCornerInset);
				l.side[l.count] = (k % 2 ? 1.0f : -1.0f) * (width * 0.5f - kVehicleCornerInset);
				l.size[l.count] = cornerSize;
			}
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
			std::uint32_t   hits = 0;
			float           biggest = 0.0f;
		};

		void Add(proto::HurtKind a_kind, float a_damage, std::uint32_t a_attacker)
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
