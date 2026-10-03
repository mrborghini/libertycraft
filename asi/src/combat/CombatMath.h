// Combat rules that don't need the game (SDK-free, unit-tested on Linux by tests/combat_test.cpp):
// actor ids, Minecraft <-> GTA IV damage scaling, which GTA IV weapon types become which Minecraft
// hurt, health fractions, the knockback direction, and the pacing of hurt events.
#pragma once

#include "libertycraft_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
