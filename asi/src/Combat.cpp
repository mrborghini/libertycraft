// Unity-built into dllmain.cpp (needs IV-SDK). See Combat.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "combat"
#include "Combat.h"

#include "Config.h"
#include "Coords.h"
#include "Game.h"
#include "Link.h"
#include "Log.h"
#include "combat/CombatMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace lc::Combat
{
	namespace
	{
		namespace S = ::Scripting;
		using namespace ::lc::combat;

		constexpr float    kActorRange = 80.0f;            // metres from the player
		constexpr float    kActorWidth = 0.6f;             // blocks
		constexpr float    kActorHeight = 1.8f;
		constexpr float    kDeathHealth = 100.0f;          // GTA IV peds die at <= 100 health (full: 200)
		constexpr unsigned kHealthBuffer = 1000;           // the puppeted player's health while we own it
		constexpr float    kHostileMemory = 30.0f;         // s: who hurt the player counts as hostile this long
		constexpr float    kBlastProofSeconds = 1.0f;      // the puppeted player ignores GTA blasts this long
		constexpr float    kKillRetrySeconds = 1.0f;
		constexpr float    kKnockbackCheckSeconds = 1.0f;  // how long after a shove we measure how far it went
		constexpr float    kSelfTestSeconds = 5.0f;
		constexpr float    kReleaseGraceSeconds = 0.5f;  // puppet mode can drop for a frame (a missed McState read)

		const Config& Cfg() { return Config::Get(); }

		struct Counters
		{
			std::uint32_t frames = 0, tableWrites = 0, actorsSent = 0, actorsMax = 0, events = 0;
			std::uint32_t hits = 0, hitsStale = 0, hitsDamageChar = 0, hitsSetHealth = 0, hitsResisted = 0, hitGtaDamage = 0, kills = 0, ragdolls = 0;
			std::uint32_t explosions = 0, deaths = 0, arrows = 0, unknownEvents = 0;
			std::uint32_t hurtsSent = 0, hurtFrames = 0, hurtDroppedIgnored = 0, hurtDroppedBlast = 0;
			float         hurtGtaDamage = 0.0f;
			double        gatherUs = 0.0;
		} counters;
		std::uint64_t lastStatsMs = 0;

		// ---- the puppeted player's health ------------------------------------------------------------
		struct Owned
		{
			bool     engaged = false;
			int      ped = 0;
			unsigned savedHealth = 0, savedArmour = 0;  // what the ped had before we took over
			float    savedMax = 0.0f;                   // CPed::m_fMaxHealth before, if we raised it
			unsigned savedInfoMax = 0;                  // CPlayerInfo::m_nMaxHealth before (SET_CHAR_HEALTH caps at it)
			bool     raisedMax = false;
			int      player = 0;
			unsigned base = 0, baseArmour = 0;          // refilled to these every frame
			unsigned lastHealth = 0, lastArmour = 0;    // what the refill actually left (losses count from here)
			bool     capReset = false;                  // the game put CPlayerInfo's max health back (logged once)
		} owned;
		HurtPacer pacer;
		float     blastProof = 0.0f;  // > 0: the player is explosion-proof (our own blast) this many seconds
		bool      proofSet = false;
		int       proofPed = 0;       // who got the proof (cleared by the timer, puppeted or not)
		float     releaseGrace = 0.0f;  // puppet mode just ended: keep the health this long before handing it back
		bool      safetyInvincible = false;  // the buffer couldn't be held: the puppeted player is invincible instead
		float     selfTestTimer = kSelfTestSeconds;

		// ---- the Minecraft player died ---------------------------------------------------------------
		bool  killing = false;
		float killTimer = 0.0f;
		int   killStage = 0;

		// ---- bookkeeping -------------------------------------------------------------------------------
		struct Recent
		{
			std::uint32_t handle;
			float         age;
		};
		std::vector<Recent> attackers;  // peds that hurt the player lately (hostile)

		struct Shove
		{
			int   ped;
			float x, y, z;
			float timeLeft;
			float force;
		};
		std::vector<Shove> shoves;

		std::vector<proto::ActorRecord> records;
		bool  tableCleared = true;
		bool  explosionTableLogged = false;
		std::uint64_t lastTypesMs = 0;
		int   damageCharWorks = -1;  // -1 unknown, 0 DAMAGE_CHAR seems to do nothing, 1 it works
		bool  warped = false;
		float warpTimer = 0.0f;
		std::uint32_t typeSeen[32]{};

		std::int64_t Qpc()
		{
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double QpcPerUs()
		{
			static const double f = [] {
				LARGE_INTEGER q;
				::QueryPerformanceFrequency(&q);
				return double(q.QuadPart) / 1e6;
			}();
			return f;
		}

		// GET_PED_TYPE: pedtype.dat's order without its four PLAYER entries (CIVMALE = 0). In Hove Beach
		// the table sees types 0, 1 and 7 (civilians and Russian gangsters), which fits.
		const char* PedTypeName(unsigned a_type)
		{
			static constexpr const char* kNames[] = { "Civilian", "Civilian", "Cop", "Gangster", "Biker", "Biker", "Gangster", "Gangster", "Gangster",
				"Gangster", "Gangster", "Gangster", "Gangster", "Gangster", "Gangster", "Dealer", "Paramedic", "Firefighter", "Criminal", "Bum",
				"Prostitute" };
			return a_type < sizeof(kNames) / sizeof(kNames[0]) ? kNames[a_type] : "Pedestrian";
		}

		constexpr unsigned kPedTypeCop = 2;

		// The script handle of a CPed* (0 if the pointer isn't a live ped in the pool, e.g. a vehicle).
		std::uint32_t HandleOfPedPointer(const void* a_ptr)
		{
			auto* pool = CPools::ms_pPedPool;
			if (!pool || !a_ptr || !pool->m_pObjects || pool->m_nEntrySize == 0) {
				return 0;
			}
			const auto base = reinterpret_cast<std::uintptr_t>(pool->m_pObjects);
			const auto p = reinterpret_cast<std::uintptr_t>(a_ptr);
			if (p < base) {
				return 0;
			}
			const auto off = p - base;
			if (off % pool->m_nEntrySize != 0 || off / pool->m_nEntrySize >= pool->m_nCount) {
				return 0;
			}
			const int slot = static_cast<int>(off / pool->m_nEntrySize);
			if (!pool->IsValid(slot)) {
				return 0;
			}
			return pool->GetIndex(reinterpret_cast<CPed*>(const_cast<void*>(a_ptr)));
		}

		bool RecentAttacker(std::uint32_t a_handle)
		{
			return std::any_of(attackers.begin(), attackers.end(), [&](const Recent& r) { return r.handle == a_handle; });
		}

		void NoteAttacker(std::uint32_t a_handle)
		{
			if (!a_handle) {
				return;
			}
			for (auto& r : attackers) {
				if (r.handle == a_handle) {
					r.age = 0.0f;
					return;
				}
			}
			attackers.push_back({ a_handle, 0.0f });
		}

		void SetBlastProof(int a_ped, bool a_on)
		{
			if (!a_ped || !S::DOES_CHAR_EXIST(a_ped)) {
				proofSet = false;
				proofPed = 0;
				return;
			}
			if (a_on == proofSet) {
				return;
			}
			// (bullet, fire, explosion, collision, melee). Overrides proofs a mission script may have set.
			S::SET_CHAR_PROOFS(a_ped, false, false, a_on, false, false);
			proofSet = a_on;
			proofPed = a_on ? a_ped : 0;
		}

		// ---- explosion info (diagnostics) ----------------------------------------------------------------
		void LogExplosionTable()
		{
			if (explosionTableLogged || !Cfg().diagnostics) {
				return;
			}
			explosionTableLogged = true;
			const tExplosionInfo* info = CExplosion::ms_ExplosionInfo;
			if (!info) {
				return;
			}
			for (int i = 0; i < 25; ++i) {
				const auto& e = info[i];
				LC_LOG("explosion type %2d: type %u weapon %u radius %.1f damage %.0f..%.0f force %.1f ragdoll %.1f fires %u-%u shake %.2f fx %08X",
					i, e.m_nType, e.m_nWeaponType, e.m_fEndRadius, e.m_fDamageAtCentre, e.m_fDamageAtEdge, e.m_fForceFactor, e.m_fRagdollModifier,
					e.m_nNumFiresMin, e.m_nNumFiresMax, e.m_fCameraShake, e.m_nFxHash);
			}
		}

		// ---- actor table ---------------------------------------------------------------------------------
		void ClearActorTable()
		{
			if (!tableCleared) {
				Link::Get().WriteActors(nullptr, 0);
				tableCleared = true;
			}
		}

		void WriteActorTable(const Frame& a_frame)
		{
			const auto t0 = Qpc();
			records.clear();
			auto* pool = CPools::ms_pPedPool;
			CPed* playerPed = FindPlayerPed();
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_frame.ped, &px, &py, &pz);
			unsigned wanted = 0;
			S::STORE_WANTED_LEVEL(a_frame.player, &wanted);
			const float feetDrop = Cfg().rootToFeet;
			for (int i = 0; pool && i < static_cast<int>(pool->m_nCount) && records.size() < proto::kMaxActors; ++i) {
				CPed* p = pool->Get(i);
				if (!p || p == playerPed || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float dx = m.x - px, dy = m.y - py, dz = m.z - pz;
				if (dx * dx + dy * dy + dz * dz > kActorRange * kActorRange) {
					continue;
				}
				const int handle = static_cast<int>(pool->GetIndex(p));
				if (handle == a_frame.ped || !S::DOES_CHAR_EXIST(handle)) {
					continue;
				}
				proto::ActorRecord r{};
				r.formId = ActorIdFromHandle(static_cast<std::uint32_t>(handle));
				r.width = kActorWidth;
				r.height = kActorHeight;
				float x = m.x, y = m.y, z = m.z, heading = 0.0f;
				unsigned type = 0;
				unsigned health = 0;
				const bool dead = S::IS_CHAR_DEAD(handle) || (S::GET_CHAR_HEALTH(handle, &health), health == 0);
				if (dead) {
					r.flags = proto::kActorDead;
					r.healthFrac = 0.0f;
					heading = p->m_fCurrentHeading * kRadToDeg;
				} else {
					if (S::IS_CHAR_IN_ANY_CAR(handle)) {
						continue;  // on foot only: a stand-in in a car seat could be hit through the car
					}
					S::GET_CHAR_COORDINATES(handle, &x, &y, &z);
					S::GET_CHAR_HEADING(handle, &heading);
					const float maxHealth = p->m_fMaxHealth > kDeathHealth ? p->m_fMaxHealth : 200.0f;
					r.healthFrac = HealthFraction(static_cast<float>(health), maxHealth, kDeathHealth);
					S::GET_PED_TYPE(handle, &type);
					const bool inCombat = S::IS_PED_IN_COMBAT(handle);
					const bool cop = type == kPedTypeCop;
					const bool hostile = inCombat && (RecentAttacker(static_cast<std::uint32_t>(handle)) || (cop && wanted > 0));
					r.flags = (inCombat ? proto::kActorInCombat : 0u) | (hostile ? proto::kActorHostile : 0u);
				}
				++typeSeen[std::min<unsigned>(type, 31)];
				const McVec feet = GtaToMc(x, y, z - feetDrop);
				r.x = static_cast<float>(feet.x);
				r.y = static_cast<float>(feet.y);
				r.z = static_cast<float>(feet.z);
				r.yaw = GtaHeadingToMcYaw(heading);
				std::strncpy(r.name, PedTypeName(type), sizeof(r.name) - 1);
				records.push_back(r);
			}
			Link::Get().WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
			tableCleared = false;
			++counters.tableWrites;
			counters.actorsSent += static_cast<std::uint32_t>(records.size());
			counters.actorsMax = std::max<std::uint32_t>(counters.actorsMax, static_cast<std::uint32_t>(records.size()));
			counters.gatherUs += double(Qpc() - t0) / QpcPerUs();
		}

		// ---- Minecraft hit a ped -----------------------------------------------------------------------------
		void ApplyHit(const proto::McEvent& a_ev, const Frame& a_frame)
		{
			std::uint32_t handle = 0;
			const int     ped = HandleFromActorId(a_ev.formId, handle) ? static_cast<int>(handle) : 0;
			unsigned   before = 0, after = 0;
			if (ped && ped != a_frame.ped && S::DOES_CHAR_EXIST(ped) && !S::IS_CHAR_DEAD(ped)) {
				S::GET_CHAR_HEALTH(ped, &before);
			}
			if (before == 0) {  // gone, dead, or dying (0 health, not flagged dead yet)
				++counters.hitsStale;
				LC_LOG_EVERY(1000, "hit on actor %08X ignored: no such living ped any more", a_ev.formId);
				return;
			}
			++counters.hits;
			const auto damage = PedDamageFromMc(a_ev.a, Cfg().pedDamageScale);
			const char* how = "none";
			if (damage > 0) {
				S::DAMAGE_CHAR(ped, damage, false);
				S::GET_CHAR_HEALTH(ped, &after);
				if (after < before) {
					how = "DAMAGE_CHAR";
					++counters.hitsDamageChar;
					if (damageCharWorks != 1) {
						damageCharWorks = 1;
						LC_LOG("DAMAGE_CHAR works: ped health %u -> %u for %u", before, after, damage);
					}
				} else if (damageCharWorks != 1) {
					// DAMAGE_CHAR has never done anything: take the health off ourselves.
					if (damageCharWorks == -1) {
						damageCharWorks = 0;
						LC_LOG("DAMAGE_CHAR(%d, %u) left health at %u; using SET_CHAR_HEALTH instead", ped, damage, after);
					}
					S::SET_CHAR_HEALTH(ped, before > damage ? before - damage : 0);
					S::GET_CHAR_HEALTH(ped, &after);
					how = "SET_CHAR_HEALTH";
					++counters.hitsSetHealth;
				} else {
					how = "resisted";  // DAMAGE_CHAR works, so this ped is protected (invincible / mission)
					++counters.hitsResisted;
				}
				counters.hitGtaDamage += damage;
			}
			const bool killed = S::IS_CHAR_DEAD(ped) || (damage > 0 && after == 0);  // GTA zeroes a ped's health as it dies
			if (killed) {
				++counters.kills;
			}
			const bool crit = (a_ev.flags & proto::kHitCritical) != 0;
			int        ragdollMs = 0;
			float      gx = 0.0f, gy = 0.0f;
			if (Cfg().ragdollOnHit && !killed && PushDirToGta(a_ev.b, a_ev.c, gx, gy)) {
				ragdollMs = RagdollMs(a_ev.d, crit);
				if (ragdollMs > 0) {
					float x = 0, y = 0, z = 0;
					S::GET_CHAR_COORDINATES(ped, &x, &y, &z);
					S::SWITCH_PED_TO_RAGDOLL(ped, ragdollMs, ragdollMs, false, false, false, false);
					// Minecraft's knockback: 0.4 for a plain hit, more for sprint hits / Knockback.
					const float force = 4.0f + 8.0f * std::clamp(a_ev.d, 0.0f, 2.0f);
					S::APPLY_FORCE_TO_PED(ped, 3, gx * force, gy * force, force * 0.3f, 0.0f, 0.0f, 0.0f, 0, 1, 1, 1);
					++counters.ragdolls;
					if (shoves.size() < 16) {
						shoves.push_back({ ped, x, y, z, kKnockbackCheckSeconds, force });
					}
				}
			}
			unsigned type = 0;
			S::GET_PED_TYPE(ped, &type);
			LC_LOG("hit %s %08X for %.2f Minecraft -> %u GTA damage (%s): health %u -> %u%s%s%s%s", PedTypeName(type), a_ev.formId, a_ev.a, damage, how,
				before, after, killed ? ", killed" : "", crit ? ", critical" : "", (a_ev.flags & proto::kHitProjectile) ? ", projectile" : "",
				ragdollMs ? ", ragdoll" : "");
		}

		void CheckShoves(float a_dt)
		{
			for (auto it = shoves.begin(); it != shoves.end();) {
				if ((it->timeLeft -= a_dt) > 0.0f) {
					++it;
					continue;
				}
				if (S::DOES_CHAR_EXIST(it->ped)) {
					float x = 0, y = 0, z = 0;
					S::GET_CHAR_COORDINATES(it->ped, &x, &y, &z);
					const float moved = std::sqrt((x - it->x) * (x - it->x) + (y - it->y) * (y - it->y));
					LC_LOG("knockback: ped %d moved %.2f m in %.1f s (force %.1f, ragdoll %d)", it->ped, moved, kKnockbackCheckSeconds, it->force,
						S::IS_PED_RAGDOLL(it->ped));
				}
				it = shoves.erase(it);
			}
		}

		// ---- Minecraft explosions ----------------------------------------------------------------------------
		void Explode(const proto::McEvent& a_ev, const Frame& a_frame)
		{
			const GtaVec c = McToGta(a_ev.a, a_ev.b, a_ev.c);
			const float  radius = ExplosionRadius(a_ev.d, Cfg().explosionRadiusScale);
			if (radius <= 0.0f) {
				return;
			}
			float px = 0, py = 0, pz = 0;
			if (a_frame.exists) {
				S::GET_CHAR_COORDINATES(a_frame.ped, &px, &py, &pz);
			}
			const float dist = a_frame.exists ? static_cast<float>(std::sqrt((c.x - px) * (c.x - px) + (c.y - py) * (c.y - py) + (c.z - pz) * (c.z - pz))) : 1e9f;
			if (owned.engaged && dist < radius * 3.0f + 5.0f) {
				// Minecraft hurt its player already; GTA's blast mustn't do it again (or ragdoll the puppet).
				blastProof = kBlastProofSeconds;
				SetBlastProof(a_frame.ped, true);
			}
			const float shake = ExplosionShake(radius, dist);
			S::ADD_EXPLOSION(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), Cfg().explosionType, radius, true, false, shake);
			++counters.explosions;
			LC_LOG("Minecraft explosion (radius %.1f blocks) -> ADD_EXPLOSION type %d radius %.1f m at GTA %.1f %.1f %.1f, %.1f m from the player, shake %.2f%s",
				a_ev.d, Cfg().explosionType, radius, c.x, c.y, c.z, dist, shake, proofSet ? " (player explosion-proof)" : "");
		}

		// ---- the player's health while puppeting -----------------------------------------------------------
		// SET_CHAR_HEALTH caps the player at CPlayerInfo::m_nMaxHealth, which the game puts back to 200
		// now and then (after a hit, seen in game): raise it again before every refill.
		void RaiseHealthCap()
		{
			CPlayerInfo* info = CPlayerInfo::GetPlayerInfo(static_cast<std::uint32_t>(owned.player));
			if (info && info->m_nMaxHealth < kHealthBuffer) {
				if (owned.raisedMax && !owned.capReset) {
					owned.capReset = true;
					LC_LOG("the game reset the player's max health to %u; raising it before every refill", info->m_nMaxHealth);
				}
				info->m_nMaxHealth = static_cast<std::uint16_t>(kHealthBuffer);
			}
		}

		void Engage(int a_player, int a_ped)
		{
			owned = Owned{};
			owned.engaged = true;
			owned.ped = a_ped;
			owned.player = a_player;
			S::GET_CHAR_HEALTH(a_ped, &owned.savedHealth);
			S::GET_CHAR_ARMOUR(a_ped, &owned.savedArmour);
			CPed*        p = FindPlayerPed();
			CPlayerInfo* info = CPlayerInfo::GetPlayerInfo(static_cast<std::uint32_t>(a_player));
			owned.savedMax = p ? p->m_fMaxHealth : 0.0f;
			owned.savedInfoMax = info ? info->m_nMaxHealth : 0u;
			// SET_CHAR_MAX_HEALTH alone doesn't lift the cap (seen in game): raise both while we own it.
			RaiseHealthCap();
			S::SET_CHAR_MAX_HEALTH(a_ped, kHealthBuffer);
			owned.raisedMax = true;
			S::SET_CHAR_HEALTH(a_ped, kHealthBuffer);
			unsigned back = 0;
			S::GET_CHAR_HEALTH(a_ped, &back);
			owned.base = back;
			owned.baseArmour = owned.savedArmour;
			owned.lastHealth = back;
			owned.lastArmour = owned.savedArmour;
			S::CLEAR_CHAR_LAST_DAMAGE_ENTITY(a_ped);
			S::CLEAR_CHAR_LAST_WEAPON_DAMAGE(a_ped);
			pacer.Reset();
			selfTestTimer = kSelfTestSeconds;
			LC_LOG("player health handed to Minecraft: GTA health %u (max %.0f, player max %u) armour %u -> buffer %u; GTA damage / %.1f goes to Minecraft",
				owned.savedHealth, owned.savedMax, owned.savedInfoMax, owned.savedArmour, owned.base, Cfg().playerDamageScale);
			if (owned.base <= kDeathHealth + 100.0f) {
				LC_LOG("WARNING: the health buffer is only %u: a big GTA hit could kill the player in one frame", owned.base);
			}
		}

		// The player's maximum health goes back in any case (CPlayerInfo outlives the ped); its health
		// and armour only with a_restore, if it's still alive.
		void Release(bool a_restore, const char* a_why, bool a_pedValid = true)
		{
			if (!owned.engaged) {
				return;
			}
			const int  ped = owned.ped;
			const bool exists = a_pedValid && ped && S::DOES_CHAR_EXIST(ped);
			const bool alive = exists && !S::IS_CHAR_DEAD(ped);
			if (owned.raisedMax) {
				if (CPlayerInfo* info = CPlayerInfo::GetPlayerInfo(static_cast<std::uint32_t>(owned.player)); info && owned.savedInfoMax) {
					info->m_nMaxHealth = static_cast<std::uint16_t>(owned.savedInfoMax);
				}
				if (exists && owned.savedMax > 0.0f) {
					S::SET_CHAR_MAX_HEALTH(ped, static_cast<unsigned>(owned.savedMax));
				}
			}
			if (a_restore && alive) {
				S::SET_CHAR_HEALTH(ped, owned.savedHealth);
				unsigned armour = 0;
				S::GET_CHAR_ARMOUR(ped, &armour);
				if (armour < owned.savedArmour) {
					S::ADD_ARMOUR_TO_CHAR(ped, owned.savedArmour - armour);
				}
			}
			if (safetyInvincible && exists) {
				S::SET_CHAR_INVINCIBLE(ped, false);
			}
			safetyInvincible = false;
			releaseGrace = 0.0f;
			LC_LOG("player health back to GTA IV (%s): %s", a_why, a_restore && alive ? "restored what it had before" : "max health restored, health left as is");
			owned = Owned{};
			pacer.Reset();
		}

		void BridgePlayerDamage(const Frame& a_frame)
		{
			unsigned health = 0, armour = 0;
			S::GET_CHAR_HEALTH(a_frame.ped, &health);
			S::GET_CHAR_ARMOUR(a_frame.ped, &armour);
			// Losses since the last refill's result: a refill that fell short never counts as damage.
			const float deficit = Deficit(static_cast<float>(owned.lastHealth), static_cast<float>(health), static_cast<float>(owned.lastArmour), static_cast<float>(armour));
			if (deficit > 0.0f) {
				CPed*         p = FindPlayerPed();
				const int     weapon = p ? p->m_nLastDamageWeapon : -1;
				const auto    attacker = p ? HandleOfPedPointer(p->m_pLastDamageEntity) : 0u;
				const auto    cls = ClassifyWeapon(weapon);
				++counters.hurtFrames;
				if (blastProof > 0.0f && (weapon == kWeaponExplosion || cls == HurtClass::kOther)) {
					++counters.hurtDroppedBlast;  // our own (Minecraft's) explosion: Minecraft hurt its player itself
				} else if (cls == HurtClass::kIgnore) {
					++counters.hurtDroppedIgnored;
				} else {
					pacer.Add(HurtKindOf(cls), deficit, attacker ? ActorIdFromHandle(attacker) : 0u);
					NoteAttacker(attacker);
				}
				if (Cfg().diagnostics) {
					LC_LOG("player lost %.0f (health %u/%u armour %u/%u), weapon %d, attacker %s%08X", deficit, health, owned.base, armour, owned.baseArmour,
						weapon, attacker ? "ped " : "", attacker);
				}
				S::CLEAR_CHAR_LAST_DAMAGE_ENTITY(a_frame.ped);
				S::CLEAR_CHAR_LAST_WEAPON_DAMAGE(a_frame.ped);
			}
			if (health != owned.base) {
				RaiseHealthCap();
				S::SET_CHAR_HEALTH(a_frame.ped, owned.base);
				S::GET_CHAR_HEALTH(a_frame.ped, &health);
			}
			if (armour < owned.baseArmour) {
				S::ADD_ARMOUR_TO_CHAR(a_frame.ped, owned.baseArmour - armour);
				S::GET_CHAR_ARMOUR(a_frame.ped, &armour);
			}
			owned.lastHealth = health;
			owned.lastArmour = armour;
			// Last line of defence: if the buffer can't be held, the player must not die from GTA damage.
			if (!safetyInvincible && health < static_cast<unsigned>(kDeathHealth) + 100u) {
				safetyInvincible = true;
				S::SET_CHAR_INVINCIBLE(a_frame.ped, true);
				LC_LOG("WARNING: couldn't refill the player's health (%u of %u): invincible until puppet mode ends", health, owned.base);
			}
			HurtPacer::Batch batch;
			if (pacer.Tick(a_frame.dt, batch)) {
				const float mcDamage = McDamageFromGta(batch.damage, Cfg().playerDamageScale);
				Game::ReportHurt(static_cast<std::uint16_t>(batch.kind), HostDamageForMc(mcDamage), batch.attacker, 0);
				++counters.hurtsSent;
				counters.hurtGtaDamage += batch.damage;
				LC_LOG("GTA IV hurt the player: %.0f GTA damage (%u hit%s, %s, attacker %08X) -> %.2f Minecraft damage", batch.damage, batch.hits,
					batch.hits == 1 ? "" : "s", batch.kind == proto::kHurtMelee ? "melee" : batch.kind == proto::kHurtProjectile ? "projectile" : "other",
					batch.attacker, mcDamage);
			}
		}

		// ---- the Minecraft player died: so does the GTA IV one --------------------------------------------
		void KillPlayer(const proto::McEvent& a_ev, const Frame& a_frame)
		{
			++counters.deaths;
			if (!a_frame.exists || a_frame.dead || a_frame.loading || killing) {
				LC_LOG("Minecraft player died (killer %08X); the GTA IV player is %s", a_ev.formId,
					killing ? "already being killed" : a_frame.dead ? "already dead" : "not in play: nothing to do");
				return;
			}
			LC_LOG("Minecraft player died (killer %08X): killing the GTA IV player%s", a_ev.formId, a_frame.puppeting ? " (puppet mode ends)" : "");
			Release(false, "the Minecraft player died");
			killing = true;
			killTimer = 0.0f;
			killStage = 0;
			S::SET_CHAR_INVINCIBLE(a_frame.ped, false);
			S::SET_CHAR_HEALTH(a_frame.ped, 0);
		}

		void UpdateKill(const Frame& a_frame)
		{
			if (!killing) {
				return;
			}
			if (!a_frame.exists || a_frame.dead || a_frame.loading) {
				LC_LOG("the GTA IV player is dead (%.1f s after Minecraft's death): GTA's wasted flow takes over", killTimer);
				killing = false;
				return;
			}
			killTimer += a_frame.dt;
			if (killTimer < kKillRetrySeconds * static_cast<float>(killStage + 1)) {
				return;
			}
			++killStage;
			unsigned health = 0;
			S::GET_CHAR_HEALTH(a_frame.ped, &health);
			if (killStage == 1) {
				LC_LOG("the player still lives after SET_CHAR_HEALTH 0 (health %u); DAMAGE_CHAR 2000", health);
				S::SET_CHAR_INVINCIBLE(a_frame.ped, false);
				S::DAMAGE_CHAR(a_frame.ped, 2000, false);
			} else {
				LC_LOG("WARNING: couldn't kill the GTA IV player (health %u); giving up", health);
				killing = false;
			}
		}

		// ---- test hooks (LibertyCraft.ini CombatSelfTest / DebugWarpOutdoors; not in the default ini) -----
		void TestHooks(const Frame& a_frame)
		{
			if (Cfg().combatSelfTest && owned.engaged && (selfTestTimer -= a_frame.dt) <= 0.0f) {
				selfTestTimer = kSelfTestSeconds;
				LC_LOG("self-test: DAMAGE_CHAR(player, 20)");
				S::DAMAGE_CHAR(a_frame.ped, 20, false);
			}
			// Once, 15 s into play: to the nearest road node (out of a car or an interior), where peds
			// walk. Puppeting, Game sees the jump and resyncs Minecraft there.
			if (Cfg().debugWarpOutdoors && !warped && a_frame.exists && !a_frame.loading && !a_frame.dead && a_frame.mcInWorld &&
				(warpTimer += a_frame.dt) > 15.0f) {
				warped = true;
				float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
				S::GET_CHAR_COORDINATES(a_frame.ped, &x, &y, &z);
				if (!S::GET_CLOSEST_CAR_NODE(x, y, z, &nx, &ny, &nz)) {
					LC_LOG("debug warp: no road node near GTA %.1f %.1f %.1f", x, y, z);
					return;
				}
				const bool inCar = S::IS_CHAR_IN_ANY_CAR(a_frame.ped);
				LC_LOG("debug warp: player %s at GTA %.1f %.1f %.1f -> road node %.1f %.1f %.1f", inCar ? "in a car" : "on foot", x, y, z, nx, ny, nz);
				if (inCar) {
					S::WARP_CHAR_FROM_CAR_TO_COORD(a_frame.ped, nx, ny, nz + 1.0f);
				} else {
					S::SET_CHAR_COORDINATES(a_frame.ped, nx, ny, nz + 1.0f);
				}
			}
		}

		void LogStats()
		{
			const auto now = ::GetTickCount64();
			const bool diag = Cfg().diagnostics;
			if (lastStatsMs == 0) {
				lastStatsMs = now;
				return;
			}
			if (now - lastStatsMs < (diag ? 1000ull : 10000ull)) {
				return;
			}
			const double secs = double(now - lastStatsMs) / 1000.0;
			lastStatsMs = now;
			const auto& c = counters;
			const bool  active = c.events || c.hurtFrames || c.explosions || c.hits;
			if (diag || active) {
				LC_LOG("stats %.0fs: actor table %u writes, %.1f actors avg (max %u), gather %.0f us avg; events %u: hits %u (DAMAGE_CHAR %u, SET_CHAR_HEALTH %u, "
					   "resisted %u, stale %u, %u GTA damage, %u kills, %u ragdolls), explosions %u, deaths %u, arrows %u, unknown %u; player hurt in %u frames "
					   "-> %u kInHurt (%.0f GTA damage), dropped %u fall/drown + %u own-blast; health %s",
					secs, c.tableWrites, c.tableWrites ? double(c.actorsSent) / c.tableWrites : 0.0, c.actorsMax, c.tableWrites ? c.gatherUs / c.tableWrites : 0.0,
					c.events, c.hits, c.hitsDamageChar, c.hitsSetHealth, c.hitsResisted, c.hitsStale, c.hitGtaDamage, c.kills, c.ragdolls, c.explosions, c.deaths,
					c.arrows, c.unknownEvents, c.hurtFrames, c.hurtsSent, c.hurtGtaDamage, c.hurtDroppedIgnored, c.hurtDroppedBlast,
					owned.engaged ? "owned by Minecraft" : "GTA's");
			}
			if (diag && now - lastTypesMs >= 30000) {
				lastTypesMs = now;
				char buf[256];
				int  n = 0;
				for (unsigned t = 0; t < 32 && n < static_cast<int>(sizeof(buf)) - 16; ++t) {
					if (typeSeen[t]) {
						n += std::snprintf(buf + n, sizeof(buf) - n, " %u:%u", t, typeSeen[t]);
					}
				}
				LC_LOG("ped types seen in 30 s (GET_PED_TYPE:records):%s", n ? buf : " none");
				std::fill(std::begin(typeSeen), std::end(typeSeen), 0u);
			}
			counters = {};
		}
	}

	bool OwnsPlayerHealth()
	{
		return Cfg().combat && !safetyInvincible;
	}

	void OnIngameStartup()
	{
		Release(false, "the game is loading", false);  // the ped is going away; CPlayerInfo's max goes back
		pacer.Reset();
		proofSet = false;
		proofPed = 0;
		blastProof = 0.0f;
		killing = false;
		attackers.clear();
		shoves.clear();
		warpTimer = 0.0f;
		ClearActorTable();
	}

	std::uint32_t Tick(const Frame& a_frame)
	{
		auto& link = Link::Get();
		++counters.frames;
		const bool playable = a_frame.exists && !a_frame.loading;

		// Minecraft's events: always drained (stale hits mustn't land later), applied while in play.
		std::uint32_t   popped = 0;
		proto::McEvent  ev{};
		for (; popped < 64 && link.PopEvent(ev); ++popped) {
			if (!Cfg().combat) {
				LC_LOG_EVERY(1000, "event from Minecraft: type %u form %08X a %.2f b %.2f c %.2f d %.2f flags 0x%X (Combat=0: ignored)", ev.type, ev.formId,
					ev.a, ev.b, ev.c, ev.d, ev.flags);
				continue;
			}
			switch (ev.type) {
			case proto::kEvHitActor:
				if (playable) {
					ApplyHit(ev, a_frame);
				}
				break;
			case proto::kEvExplosion:
				if (playable) {
					Explode(ev, a_frame);
				}
				break;
			case proto::kEvPlayerDied:
				KillPlayer(ev, a_frame);
				break;
			case proto::kEvArrowStuck:
				++counters.arrows;  // drawing stuck arrows is the renderer's business
				break;
			default:
				++counters.unknownEvents;
				break;
			}
		}
		counters.events += popped;
		if (!Cfg().combat) {
			return popped;
		}

		if (playable) {
			LogExplosionTable();
		}
		if (playable && a_frame.mcInWorld && !a_frame.dead) {
			WriteActorTable(a_frame);
		} else {
			ClearActorTable();
		}

		// The puppeted player's health belongs to Minecraft.
		// A frame or two without puppet mode (a missed McState read) keeps it, so the buffer and a blast's
		// protection don't lapse; a changed ped, loading, death or a longer break hands it back.
		const bool want = a_frame.puppeting && playable && !a_frame.dead && !killing;
		if (owned.engaged) {
			const bool sameLivePed = owned.ped == a_frame.ped && playable && !a_frame.dead && !killing;
			if (want || (sameLivePed && (releaseGrace += a_frame.dt) < kReleaseGraceSeconds)) {
				releaseGrace = want ? 0.0f : releaseGrace;
			} else {
				Release(owned.ped == a_frame.ped && playable, owned.ped != a_frame.ped ? "the player ped changed" : "puppet mode ended");
			}
		}
		if (want && !owned.engaged) {
			Engage(a_frame.player, a_frame.ped);
		}
		if (blastProof > 0.0f && (blastProof -= a_frame.dt) <= 0.0f) {
			blastProof = 0.0f;
			if (proofSet && proofPed && playable) {
				SetBlastProof(proofPed, false);
			}
		}
		if (owned.engaged) {
			BridgePlayerDamage(a_frame);
		}
		UpdateKill(a_frame);
		CheckShoves(a_frame.dt);
		for (auto it = attackers.begin(); it != attackers.end();) {
			it = (it->age += a_frame.dt) > kHostileMemory ? attackers.erase(it) : it + 1;
		}
		TestHooks(a_frame);
		LogStats();
		return popped;
	}
}
