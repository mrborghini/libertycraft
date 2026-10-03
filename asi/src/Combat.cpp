// Unity-built into dllmain.cpp (needs IV-SDK). See Combat.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "combat"
#include "Combat.h"

#include "Config.h"
#include "Coords.h"
#include "Game.h"
#include "HostDrive.h"
#include "Link.h"
#include "Log.h"
#include "NpcBlocks.h"
#include "combat/CombatMath.h"

#include <algorithm>
#include <atomic>
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
		constexpr float    kVehicleRange = 60.0f;          // vehicles (several records each) within this
		constexpr std::size_t kMaxPedRecords = proto::kMaxActors - 112;  // room for a dozen vehicles in a crowd
		constexpr float    kActorWidth = 0.6f;             // blocks
		constexpr float    kActorHeight = 1.8f;
		constexpr float    kDeathHealth = 100.0f;          // GTA IV peds die at <= 100 health (full: 200)
		constexpr unsigned kHealthBuffer = 1000;           // the puppeted player's health while we own it
		constexpr float    kHostileMemory = 30.0f;         // s: who hurt the player counts as hostile this long
		constexpr float    kReactAgainSeconds = 4.0f;      // GtaCrimes: a ped gets a new task after this long
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
			std::uint32_t vehiclesSent = 0, vehicleHits = 0, vehicleHitsStale = 0, vehicleBlastHits = 0, occupantHits = 0, windows = 0, wrecked = 0;
			std::uint32_t vehiclePointsOnCar = 0, vehiclePointsOff = 0, vehicleNoPoint = 0;
			std::uint32_t crimes = 0, fights = 0, flees = 0, driversFled = 0;
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
			float dirX, dirY;  // where Minecraft's knockback pushed (GTA, unit)
			float heading;     // the ped's heading when hit (degrees)
			int   variant;     // how the force was applied (see Knock)
		};
		std::vector<Shove> shoves;
		int knockVariantNext = -1;  // DebugKnockbackVariant: the next variant to try
		struct VariantStats
		{
			std::uint32_t n = 0;
			float         moved = 0.0f, angle = 0.0f;
		} variantStats[4];

		// Vehicles we sent as pieces this frame (for hits): handle -> how many pieces.
		struct SentVehicle
		{
			std::uint32_t handle;
			std::uint32_t pieces;
		};
		std::vector<SentVehicle> sentVehicles;

		// kEvHitPoint: where the next kEvHitActor on this formId landed.
		struct HitPoint
		{
			bool          valid = false;
			std::uint32_t formId = 0;
			double        at[3]{};
			float         yaw = 0.0f, pitch = 0.0f;
		} hitPoint;
		std::vector<std::pair<float, int>> vehicleCandidates;

		std::vector<proto::ActorRecord> records;
		bool  tableCleared = true;
		bool  explosionTableLogged = false;
		std::uint64_t lastTypesMs = 0;
		int   damageCharWorks = -1;  // -1 unknown, 0 DAMAGE_CHAR seems to do nothing, 1 it works
		bool  warped = false;
		float warpTimer = 0.0f;

		// Occupants Minecraft killed: do they stay in their seat, and does the car coast to a stop? Logged
		// 0.5, 1.5 and 4 s after.
		struct SeatWatch
		{
			int   ped, veh;
			float t;
			int   logged;
			float speedAtKill;
			int   variant;
			float seated[3];
		};
		std::vector<SeatWatch> seatWatch;
		unsigned dieInCarToggle = 0;

		// GtaCrimes: who we told to fight or flee lately (ped handle -> seconds since), and the
		// DebugWanted hook's state.
		std::vector<Recent> reacted;
		struct WantedTest
		{
			bool  started = false;
			float timer = 0.0f, logTimer = 0.0f, since = 0.0f;
		} wantedTest;

		// DebugTestCar: a parked car with people in it, east of the player, then shown to the camera.
		struct TestCar
		{
			int   stage = 0;  // 0 waiting, 1 model requested, 2 parked, 3 shown, 4 done
			float timer = 0.0f;
			int   car = 0, driver = 0, passenger = 0;
			float logTimer = 0.0f;
			char  last[160] = "";
			float shotTimer = 0.0f;
			int   shots = 0;
		} testCar;
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

		// Vehicles near the player, nearest first, as pieces along their length (proto::kActorVehicle)
		// in whatever room the peds left. Not the one the player uses (sits in or is getting into).
		void AddVehicles(const Frame& a_frame, float a_px, float a_py, float a_pz)
		{
			sentVehicles.clear();
			vehicleCandidates.clear();
			auto* pool = CPools::ms_pVehiclePool;
			if (!pool) {
				return;
			}
			int playerCar = 0;
			if (a_frame.ped) {
				S::GET_CAR_CHAR_IS_USING(a_frame.ped, &playerCar);
			}
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CVehicle* veh = pool->Get(slot);
				if (!veh || !veh->m_pMatrix || veh->m_nVehicleType == VEHICLE_TYPE_TRAIN || veh->m_nVehicleType == VEHICLE_TYPE_PLANE) {
					continue;
				}
				const auto& m = veh->m_pMatrix->pos;
				const float dx = m.x - a_px, dy = m.y - a_py, dz = m.z - a_pz;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 <= kVehicleRange * kVehicleRange) {
					vehicleCandidates.emplace_back(d2, slot);
				}
			}
			std::sort(vehicleCandidates.begin(), vehicleCandidates.end());
			for (const auto& [d2, slot] : vehicleCandidates) {
				CVehicle* veh = pool->Get(slot);
				const int handle = veh ? static_cast<int>(pool->GetIndex(veh)) : 0;
				if (!handle || handle == playerCar || !S::DOES_VEHICLE_EXIST(handle)) {
					continue;
				}
				float lo[3], hi[3];
				if (!NpcBlocks::ModelBox(handle, veh->m_nModelIndex, lo, hi)) {
					continue;
				}
				const CMatrix& mat = *veh->m_pMatrix;
				// (|cos| + |sin| of its heading against the world axes: how much its boxes must shrink.)
				const float axisLen = std::hypot(mat.up.x, mat.up.y);
				const float e = axisLen > 1e-3f ? (std::fabs(mat.up.x) + std::fabs(mat.up.y)) / axisLen : 1.0f;
				const auto  layout = VehicleSegments(hi[1] - lo[1], hi[0] - lo[0], e);
				if (records.size() + layout.count > proto::kMaxActors) {
					break;
				}
				const float    lx = (lo[0] + hi[0]) * 0.5f, ly = (lo[1] + hi[1]) * 0.5f, lz = (lo[2] + hi[2]) * 0.5f;
				// IV-SDK's CMatrix names its rows after GTA SA's: "up" is the forward (y) axis, "at" the up (z) axis.
				const auto&    fwd = mat.up;
				const auto&    upv = mat.at;
				const float    cx = mat.pos.x + mat.right.x * lx + fwd.x * ly + upv.x * lz;
				const float    cy = mat.pos.y + mat.right.y * lx + fwd.y * ly + upv.y * lz;
				const float    cz = mat.pos.z + mat.right.z * lx + fwd.z * ly + upv.z * lz;
				const float    height = hi[2] - lo[2];
				const float    flen = std::hypot(fwd.x, fwd.y);
				const float    fx = flen > 1e-3f ? fwd.x / flen : 0.0f, fy = flen > 1e-3f ? fwd.y / flen : 1.0f;
				const bool     dead = S::IS_CAR_DEAD(handle);
				unsigned       health = 0;
				S::GET_CAR_HEALTH(handle, &health);
				unsigned model = 0;
				S::GET_CAR_MODEL(handle, &model);
				const char* name = S::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
				const float heading = std::atan2(-fx, fy) * kRadToDeg;
				for (std::uint32_t piece = 0; piece < layout.count; ++piece) {
					proto::ActorRecord r{};
					r.formId = VehicleActorId(static_cast<std::uint32_t>(handle), piece);
					r.flags = proto::kActorVehicle | (dead ? proto::kActorDead : 0u);
					// Right of the axis, seen from above: (fy, -fx).
					const float along = layout.offset[piece], side = layout.side[piece];
					const McVec feet = GtaToMc(cx + fx * along + fy * side, cy + fy * along - fx * side, cz - height * 0.5f);
					r.x = static_cast<float>(feet.x);
					r.y = static_cast<float>(feet.y);
					r.z = static_cast<float>(feet.z);
					r.yaw = GtaHeadingToMcYaw(heading);
					r.width = layout.size[piece];
					r.height = height;
					r.healthFrac = dead ? 0.0f : std::clamp(static_cast<float>(health) / 1000.0f, 0.0f, 1.0f);
					std::strncpy(r.name, name && *name ? name : "Vehicle", sizeof(r.name) - 1);
					records.push_back(r);
				}
				sentVehicles.push_back({ static_cast<std::uint32_t>(handle), layout.count });
				++counters.vehiclesSent;
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
			for (int i = 0; pool && i < static_cast<int>(pool->m_nCount) && records.size() < kMaxPedRecords; ++i) {
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
			AddVehicles(a_frame, px, py, pz);
			Link::Get().WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
			tableCleared = false;
			++counters.tableWrites;
			counters.actorsSent += static_cast<std::uint32_t>(records.size());
			counters.actorsMax = std::max<std::uint32_t>(counters.actorsMax, static_cast<std::uint32_t>(records.size()));
			counters.gatherUs += double(Qpc() - t0) / QpcPerUs();
		}

		// ---- Minecraft hit a ped -----------------------------------------------------------------------------
		// MC damage off a living ped's health: DAMAGE_CHAR, or SET_CHAR_HEALTH if DAMAGE_CHAR never works.
		// Returns how ("DAMAGE_CHAR", "SET_CHAR_HEALTH", "resisted" or "none").
		const char* DamagePed(int a_ped, float a_mcDamage, unsigned a_before, unsigned& a_after, unsigned& a_damage)
		{
			a_after = a_before;
			a_damage = PedDamageFromMc(a_mcDamage, Cfg().pedDamageScale);
			if (a_damage == 0) {
				return "none";
			}
			const char* how = "none";
			S::DAMAGE_CHAR(a_ped, a_damage, false);
			S::GET_CHAR_HEALTH(a_ped, &a_after);
			if (a_after < a_before) {
				how = "DAMAGE_CHAR";
				++counters.hitsDamageChar;
				if (damageCharWorks != 1) {
					damageCharWorks = 1;
					LC_LOG("DAMAGE_CHAR works: ped health %u -> %u for %u", a_before, a_after, a_damage);
				}
			} else if (damageCharWorks != 1) {
				// DAMAGE_CHAR has never done anything: take the health off ourselves.
				if (damageCharWorks == -1) {
					damageCharWorks = 0;
					LC_LOG("DAMAGE_CHAR(%d, %u) left health at %u; using SET_CHAR_HEALTH instead", a_ped, a_damage, a_after);
				}
				S::SET_CHAR_HEALTH(a_ped, a_before > a_damage ? a_before - a_damage : 0);
				S::GET_CHAR_HEALTH(a_ped, &a_after);
				how = "SET_CHAR_HEALTH";
				++counters.hitsSetHealth;
			} else {
				how = "resisted";  // DAMAGE_CHAR works, so this ped is protected (invincible / mission)
				++counters.hitsResisted;
			}
			counters.hitGtaDamage += a_damage;
			return how;
		}

		// Shoves a ragdolled ped along the GTA direction (a_gx, a_gy), which points away from the
		// attacker (HostActorEntity sends the way Minecraft's knockback moves the victim).
		// APPLY_FORCE_TO_PED's 10th argument decides how the vector is read. Variants (the
		// DebugKnockbackVariant test hook cycles them): 0 the world direction with it 0, 1 the
		// direction turned into the ped's frame with it 1, 2 the world direction with it 1 (what we
		// used to send), 3 no force. Measured in game (37 hits): variant 0 lands 2.5 m along the push,
		// 15 deg off it on average; 1 and 2 only work for peds facing north and throw others sideways
		// or back toward the attacker (heading 120: 4 m the wrong way), which is what players saw.
		constexpr int kKnockVariant = 0;

		void Knock(int a_ped, float a_gx, float a_gy, float a_force, float a_headingDeg, int a_variant)
		{
			const float up = a_force * 0.3f;
			switch (a_variant) {
			case 0:
				S::APPLY_FORCE_TO_PED(a_ped, 3, a_gx * a_force, a_gy * a_force, up, 0.0f, 0.0f, 0.0f, 0, 0, 1, 1);
				break;
			case 1:
				{
					float lx = 0.0f, ly = 0.0f;
					WorldToPedLocal(a_gx, a_gy, a_headingDeg, lx, ly);
					S::APPLY_FORCE_TO_PED(a_ped, 3, lx * a_force, ly * a_force, up, 0.0f, 0.0f, 0.0f, 0, 1, 1, 1);
				}
				break;
			case 2:
				S::APPLY_FORCE_TO_PED(a_ped, 3, a_gx * a_force, a_gy * a_force, up, 0.0f, 0.0f, 0.0f, 0, 1, 1, 1);
				break;
			default:
				break;
			}
		}

		void ApplyVehicleHit(const proto::McEvent& a_ev, const Frame& a_frame, std::uint32_t a_handle, std::uint32_t a_piece);
		bool PelvisInCar(int a_ped, int a_veh, float a_out[3]);
		// How a killing blow on an occupant is dealt. Measured in game (the pelvis bone in the car's frame,
		// 4 s on): 0 DAMAGE_CHAR plays the on-foot death where the ped sits, and the body sinks 0.9 m,
		// through the floor (what players saw); 1 EXPLODE_CHAR_HEAD and 2 SET_CHAR_HEALTH 0 (with
		// SET_CHAR_FORCE_DIE_IN_CAR) leave it slumped in the seat (within 0.06 m). (A bullet fired at the
		// ped with FIRE_SINGLE_BULLET didn't hurt it at all.) DebugDieInCarAB cycles them.
		constexpr int kKillOccupantVariant = 2;
		constexpr const char* kKillVariantNames[] = { "DAMAGE_CHAR", "EXPLODE_CHAR_HEAD", "SET_CHAR_HEALTH 0" };
		const char* KillOccupant(int a_ped, int a_variant, unsigned a_before, unsigned& a_after, unsigned& a_damage);

		// ---- Minecraft's attacks as GTA IV crimes (GtaCrimes) ----------------------------------------------
		// GTA IV only reacts to damage it dealt itself with the player as the attacker; DAMAGE_CHAR has
		// none. So do what it would: the victim fights back or runs, and witnesses (the police above
		// all) give the player a wanted level (CombatMath.h WantedAfterAttack).
		bool ToughPedType(unsigned a_type)
		{
			// pedtype.dat without the players: 3 to 14 gangs, 4 and 5 bikers, 18 criminals.
			return (a_type >= 3 && a_type <= 14) || a_type == 18;
		}

		void CountWitnesses(int a_victim, int a_player, unsigned& a_cops, unsigned& a_witnesses)
		{
			a_cops = a_witnesses = 0;
			float vx = 0, vy = 0, vz = 0;
			S::GET_CHAR_COORDINATES(a_victim, &vx, &vy, &vz);
			auto* pool = CPools::ms_pPedPool;
			CPed* playerPed = FindPlayerPed();
			for (int i = 0; pool && i < static_cast<int>(pool->m_nCount); ++i) {
				CPed* p = pool->Get(i);
				if (!p || p == playerPed || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float d2 = (m.x - vx) * (m.x - vx) + (m.y - vy) * (m.y - vy) + (m.z - vz) * (m.z - vz);
				if (d2 > kCopSightMetres * kCopSightMetres) {
					continue;
				}
				const int h = static_cast<int>(pool->GetIndex(p));
				if (h == a_victim || h == a_player || !S::DOES_CHAR_EXIST(h) || S::IS_CHAR_DEAD(h)) {
					continue;
				}
				unsigned type = 0;
				S::GET_PED_TYPE(h, &type);
				if (type == kPedTypeCop) {
					++a_cops;
				}
				if (d2 <= kWitnessMetres * kWitnessMetres) {
					++a_witnesses;
				}
			}
		}

		// After Minecraft hurt or killed a_ped (of GET_PED_TYPE a_type; a_driving: it sits at the wheel of a_vehicle).
		void AfterAttack(int a_ped, unsigned a_type, bool a_killed, const Frame& a_frame, int a_vehicle = 0)
		{
			if (!Cfg().gtaCrimes || !a_frame.ped) {
				return;
			}
			const bool cop = a_type == kPedTypeCop;
			unsigned   cops = 0, witnesses = 0, wanted = 0;
			CountWitnesses(a_ped, a_frame.ped, cops, witnesses);
			S::STORE_WANTED_LEVEL(a_frame.player, &wanted);
			const unsigned want = WantedAfterAttack(wanted, cop, a_killed, cops, witnesses);
			if (want > wanted) {
				S::ALTER_WANTED_LEVEL_NO_DROP(a_frame.player, want);
				S::APPLY_WANTED_LEVEL_CHANGE_NOW(a_frame.player);
				unsigned now = 0;
				S::STORE_WANTED_LEVEL(a_frame.player, &now);
				++counters.crimes;
				LC_LOG("crime: Minecraft %s a %s (%u cops, %u witnesses near): wanted level %u -> %u", a_killed ? "killed" : "hurt", PedTypeName(a_type), cops,
					witnesses, wanted, now);
			}
			if (a_killed) {
				return;
			}
			const auto handle = static_cast<std::uint32_t>(a_ped);
			for (const auto& r : reacted) {
				if (r.handle == handle) {
					return;  // told what to do a moment ago; let it get on with it
				}
			}
			reacted.push_back({ handle, 0.0f });
			if (a_vehicle) {
				// At the wheel: drive off.
				S::FORCE_PED_TO_FLEE_WHILST_DRIVING_VEHICLE(a_ped, a_vehicle);
				++counters.driversFled;
				LC_LOG("the driver of vehicle %d (ped %d) flees", a_vehicle, a_ped);
				return;
			}
			if (ReactionOf(cop, ToughPedType(a_type), handle) == Reaction::kFight) {
				S::TASK_COMBAT(a_ped, a_frame.ped);
				NoteAttacker(handle);  // hostile in the actor table from now on
				++counters.fights;
				LC_LOG("%s %d fights back", PedTypeName(a_type), a_ped);
			} else {
				S::TASK_SMART_FLEE_CHAR(a_ped, a_frame.ped, 100.0f, 20000);
				++counters.flees;
				LC_LOG("%s %d runs away", PedTypeName(a_type), a_ped);
			}
		}

		void ApplyHit(const proto::McEvent& a_ev, const Frame& a_frame)
		{
			std::uint32_t handle = 0, piece = 0;
			if (VehicleFromActorId(a_ev.formId, handle, piece)) {
				ApplyVehicleHit(a_ev, a_frame, handle, piece);
				return;
			}
			const int ped = HandleFromActorId(a_ev.formId, handle) ? static_cast<int>(handle) : 0;
			unsigned  before = 0, after = 0, damage = 0;
			if (ped && ped != a_frame.ped && S::DOES_CHAR_EXIST(ped) && !S::IS_CHAR_DEAD(ped)) {
				S::GET_CHAR_HEALTH(ped, &before);
			}
			if (before == 0) {  // gone, dead, or dying (0 health, not flagged dead yet)
				++counters.hitsStale;
				LC_LOG_EVERY(1000, "hit on actor %08X ignored: no such living ped any more", a_ev.formId);
				return;
			}
			++counters.hits;
			const char* how = DamagePed(ped, a_ev.a, before, after, damage);
			const bool  killed = S::IS_CHAR_DEAD(ped) || (damage > 0 && after == 0);  // GTA zeroes a ped's health as it dies
			if (killed) {
				++counters.kills;
			}
			const bool crit = (a_ev.flags & proto::kHitCritical) != 0;
			int        ragdollMs = 0;
			float      gx = 0.0f, gy = 0.0f;
			if (Cfg().ragdollOnHit && !killed && PushDirToGta(a_ev.b, a_ev.c, gx, gy)) {
				ragdollMs = RagdollMs(a_ev.d, crit);
				if (ragdollMs > 0) {
					float x = 0, y = 0, z = 0, heading = 0;
					S::GET_CHAR_COORDINATES(ped, &x, &y, &z);
					S::GET_CHAR_HEADING(ped, &heading);
					S::SWITCH_PED_TO_RAGDOLL(ped, ragdollMs, ragdollMs, false, false, false, false);
					// Minecraft's knockback: 0.4 for a plain hit, more for sprint hits / Knockback.
					const float force = 4.0f + 8.0f * std::clamp(a_ev.d, 0.0f, 2.0f);
					int         variant = kKnockVariant;
					if (Cfg().debugKnockbackVariant >= 0) {
						if (knockVariantNext < 0) {
							knockVariantNext = Cfg().debugKnockbackVariant & 3;
						}
						variant = knockVariantNext;
						knockVariantNext = (knockVariantNext + 1) & 3;
					}
					Knock(ped, gx, gy, force, heading, variant);
					++counters.ragdolls;
					if (shoves.size() < 16) {
						shoves.push_back({ ped, x, y, z, kKnockbackCheckSeconds, force, gx, gy, heading, variant });
					}
				}
			}
			unsigned type = 0;
			S::GET_PED_TYPE(ped, &type);
			LC_LOG("hit %s %08X for %.2f Minecraft -> %u GTA damage (%s): health %u -> %u%s%s%s%s", PedTypeName(type), a_ev.formId, a_ev.a, damage, how,
				before, after, killed ? ", killed" : "", crit ? ", critical" : "", (a_ev.flags & proto::kHitProjectile) ? ", projectile" : "",
				ragdollMs ? ", ragdoll" : "");
			if (damage > 0) {
				AfterAttack(ped, type, killed, a_frame);
			}
		}

		// ---- Minecraft hit a vehicle ---------------------------------------------------------------------
		// The ped in seat a_seat (kSeatDriver, or a passenger seat), 0 if none (or the player).
		int Occupant(int a_veh, int a_seat, int a_player)
		{
			int p = 0;
			if (a_seat == kSeatDriver) {
				S::GET_DRIVER_OF_CAR(a_veh, &p);
			} else if (a_seat >= 0) {
				// Only seats the vehicle has: the seat natives read past its seats otherwise and hand the
				// game a garbage ped (crashed it in game: a rear window of a two-door car).
				unsigned seats = 0;
				S::GET_MAXIMUM_NUMBER_OF_PASSENGERS(a_veh, &seats);
				if (static_cast<unsigned>(a_seat) < std::min(seats, 8u) && !S::IS_CAR_PASSENGER_SEAT_FREE(a_veh, static_cast<unsigned>(a_seat))) {
					S::GET_CHAR_IN_CAR_PASSENGER_SEAT(a_veh, static_cast<unsigned>(a_seat), &p);
				}
			}
			return p && p != a_player && S::DOES_CHAR_EXIST(p) && !S::IS_CHAR_DEAD(p) ? p : 0;
		}

		const char* WindowName(int a_window)
		{
			static constexpr const char* kNames[] = { "front left window", "front right window", "rear left window", "rear right window", "windscreen",
				"rear windscreen" };
			return a_window >= 0 && a_window < 6 ? kNames[a_window] : "body";
		}

		const char* FaceName(int a_face)
		{
			static constexpr const char* kNames[] = { "left side", "right side", "back", "front", "underside", "top" };
			return a_face >= 0 && a_face < 6 ? kNames[a_face] : "?";
		}

		// Body and engine health go down by MC damage * VehicleDamageScale; an engine run below 0 burns
		// and blows the car up a few seconds later (GTA's own way), and hitting a burning wreck-to-be
		// blows it up at once. Where the hit landed (kEvHitPoint, followed into the car's model box)
		// decides the rest: a blow or a projectile on a window's glass breaks that window, and a
		// projectile through it also hits whoever sits behind it; the body only takes the damage.
		// Explosions are left to the host's own blast (kEvExplosion), which hits the vehicle already.
		void ApplyVehicleHit(const proto::McEvent& a_ev, const Frame& a_frame, std::uint32_t a_handle, std::uint32_t a_piece)
		{
			const bool havePoint = hitPoint.valid && hitPoint.formId == a_ev.formId;
			hitPoint.valid = false;
			const int veh = static_cast<int>(a_handle);
			if (!veh || !S::DOES_VEHICLE_EXIST(veh) || S::IS_CAR_DEAD(veh)) {
				++counters.vehicleHitsStale;
				LC_LOG_EVERY(1000, "hit on vehicle %08X ignored: no such working vehicle any more", a_ev.formId);
				return;
			}
			if (a_ev.flags & proto::kHitExplosion) {
				++counters.vehicleBlastHits;
				return;
			}
			++counters.vehicleHits;
			auto* pool = CPools::ms_pVehiclePool;
			CVehicle* car = pool ? pool->GetAt(a_handle) : nullptr;
			unsigned  body = 0;
			S::GET_CAR_HEALTH(veh, &body);
			const float engineBefore = car ? car->m_fEngineHealth : 1000.0f;
			const float damage = VehicleDamageFromMc(a_ev.a, Cfg().vehicleDamageScale);
			const bool  projectile = (a_ev.flags & proto::kHitProjectile) != 0;
			const bool  crit = (a_ev.flags & proto::kHitCritical) != 0;

			// Where it struck: follow the line it came along into the model box.
			CarStrike strike;
			int       face = -1;
			float     local[3]{};
			bool      onCar = false;
			float     lo[3], hi[3];
			if (havePoint && car && car->m_pMatrix && NpcBlocks::ModelBox(veh, car->m_nModelIndex, lo, hi)) {
				// IV-SDK's CMatrix names its rows after GTA SA's: "up" is the forward (y) axis, "at" the up (z) axis.
				const CMatrix& m = *car->m_pMatrix;
				const float    pos[3] = { m.pos.x, m.pos.y, m.pos.z }, right[3] = { m.right.x, m.right.y, m.right.z }, fwd[3] = { m.up.x, m.up.y, m.up.z },
							up[3] = { m.at.x, m.at.y, m.at.z };
				onCar = StrikeOnCar(hitPoint.at, hitPoint.yaw, hitPoint.pitch, pos, right, fwd, up, lo, hi, face, local);
				if (onCar) {
					strike = ClassifyCarStrike(face, local, lo, hi);
				}
			}
			++(havePoint ? (onCar ? counters.vehiclePointsOnCar : counters.vehiclePointsOff) : counters.vehicleNoPoint);

			const char* what = "damaged";
			if (engineBefore < 0.0f && damage > 0.0f) {
				// Already burning: one more hit finishes it.
				S::EXPLODE_CAR(veh, true, false);
				what = "blown up (it was burning)";
				++counters.wrecked;
			} else if (damage > 0.0f) {
				const float bodyAfter = std::max(1.0f, static_cast<float>(body) - damage);
				S::SET_CAR_HEALTH(veh, static_cast<unsigned>(bodyAfter));
				float engineAfter = engineBefore - damage;
				if (engineAfter < 0.0f) {
					engineAfter = -100.0f;  // on fire: GTA blows it up shortly
					what = "set on fire";
					++counters.wrecked;
				}
				S::SET_ENGINE_HEALTH(veh, engineAfter);
			}
			const char* glass = "";
			if (strike.window >= kWindowLF && strike.window <= kWindowRR) {
				// (IS_VEH_WINDOW_INTACT only notices a smash a frame later.)
				glass = S::IS_VEH_WINDOW_INTACT(veh, static_cast<unsigned>(strike.window)) ? " (smashed)" : " (smashed again)";
				S::SMASH_CAR_WINDOW(veh, strike.window);
				++counters.windows;
			} else if (strike.window != kWindowNone) {
				// The window natives only know the four side windows: given 4 or 5 they read past them
				// (garbage, and in game a crash). A windscreen keeps its glass; whoever is behind it is hit.
				glass = " (no native breaks a windscreen)";
			}
			// A projectile through the glass hits whoever sits behind it.
			char occupantText[112] = "";
			if (projectile && strike.window != kWindowNone) {
				if (const int occupant = Occupant(veh, strike.seat, a_frame.ped)) {
					unsigned before = 0, after = 0, pedDamage = 0, type = 0;
					S::GET_CHAR_HEALTH(occupant, &before);
					S::GET_PED_TYPE(occupant, &type);
					float seatedPelvis[3] = { 0.0f, 0.0f, 0.0f };
					PelvisInCar(occupant, veh, seatedPelvis);
					// A killing blow must end in GTA's own in-car death (slumped in the seat). DAMAGE_CHAR's
					// death plays the on-foot one where the ped sits: the body sinks through the floor (seen
					// in game: the pelvis 0.9 m under the seat 4 s later). Variants (DebugDieInCarAB cycles
					// them): see KillOccupant.
					const int variant = Cfg().debugDieInCarAB ? static_cast<int>(dieInCarToggle++ % 3) : kKillOccupantVariant;
					const unsigned wouldDo = PedDamageFromMc(a_ev.a, Cfg().pedDamageScale);
					const bool     lethal = wouldDo > 0 && before <= wouldDo + static_cast<unsigned>(kDeathHealth);
					const char*    how = lethal && variant != 0 ? KillOccupant(occupant, variant, before, after, pedDamage) : DamagePed(occupant, a_ev.a, before, after, pedDamage);
					++counters.occupantHits;
					const bool killed = S::IS_CHAR_DEAD(occupant) || (pedDamage > 0 && after == 0);
					if (killed) {
						++counters.kills;
					}
					if (pedDamage > 0) {
						AfterAttack(occupant, type, killed, a_frame, strike.seat == kSeatDriver ? veh : 0);
					}
					if (killed && seatWatch.size() < 8) {
						float sp = 0.0f;
						S::GET_CAR_SPEED(veh, &sp);
						seatWatch.push_back({ occupant, veh, 0.0f, 0, sp, lethal ? variant : 0, { seatedPelvis[0], seatedPelvis[1], seatedPelvis[2] } });
					}
					std::snprintf(occupantText, sizeof(occupantText), "; through it into the %s (ped %d) for %u (%s): health %u -> %u%s",
						strike.seat == kSeatDriver ? "driver" : "passenger", occupant, pedDamage, how, before, after, killed ? ", killed" : "");
				} else {
					std::snprintf(occupantText, sizeof(occupantText), "; nobody sits behind it");
				}
			}
			// Whoever drives it takes off (GtaCrimes), unless it was just shot through the glass (above).
			if (Cfg().gtaCrimes && occupantText[0] == 0) {
				if (const int driver = Occupant(veh, kSeatDriver, a_frame.ped)) {
					unsigned type = 0;
					S::GET_PED_TYPE(driver, &type);
					AfterAttack(driver, type, false, a_frame, veh);
				}
			}
			char where[112];
			if (onCar) {
				std::snprintf(where, sizeof(where), "%s at %.2f %.2f %.2f (car frame) -> %s%s", FaceName(face), local[0], local[1], local[2],
					WindowName(strike.window), glass);
			} else {
				std::snprintf(where, sizeof(where), "%s", havePoint ? "its line misses the car (a stand-in's edge): body" : "no hit point: body");
			}
			unsigned bodyNow = 0;
			S::GET_CAR_HEALTH(veh, &bodyNow);
			LC_LOG("hit vehicle %d (piece %u) for %.2f Minecraft -> %.0f GTA damage%s%s: %s; %s, body %u -> %u, engine %.0f -> %.0f%s", veh, a_piece, a_ev.a,
				damage, projectile ? ", projectile" : "", crit ? ", critical" : "", where, what, body, bodyNow, engineBefore, car ? car->m_fEngineHealth : 0.0f,
				occupantText);
		}

		// A ped's pelvis bone in a vehicle's frame (x right, y forward, z up). False if either is gone.
		bool PelvisInCar(int a_ped, int a_veh, float a_out[3])
		{
			CVehicle* car = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(a_veh)) : nullptr;
			if (!car || !car->m_pMatrix || !S::DOES_CHAR_EXIST(a_ped)) {
				return false;
			}
			S::Vector3 b{};
			S::GET_PED_BONE_POSITION(a_ped, 0x1A1 /* BONE_PELVIS */, 0.0f, 0.0f, 0.0f, &b);
			const CMatrix& m = *car->m_pMatrix;
			const float    d[3] = { b.x - m.pos.x, b.y - m.pos.y, b.z - m.pos.z };
			a_out[0] = d[0] * m.right.x + d[1] * m.right.y + d[2] * m.right.z;
			a_out[1] = d[0] * m.up.x + d[1] * m.up.y + d[2] * m.up.z;  // ("up" is the forward axis)
			a_out[2] = d[0] * m.at.x + d[1] * m.at.y + d[2] * m.at.z;
			return true;
		}

		// Kills a vehicle's occupant so that GTA plays its in-car death (variants: kKillVariantNames).
		const char* KillOccupant(int a_ped, int a_variant, unsigned a_before, unsigned& a_after, unsigned& a_damage)
		{
			a_damage = a_before;
			if (a_variant == 1) {
				S::EXPLODE_CHAR_HEAD(a_ped);
			} else {
				S::SET_CHAR_FORCE_DIE_IN_CAR(a_ped, true);
				S::SET_CHAR_HEALTH(a_ped, 0);
			}
			S::GET_CHAR_HEALTH(a_ped, &a_after);
			return kKillVariantNames[a_variant % 3];
		}

		void CheckSeats(float a_dt)
		{
			static constexpr float kAt[] = { 0.5f, 1.5f, 4.0f };
			for (auto it = seatWatch.begin(); it != seatWatch.end();) {
				it->t += a_dt;
				if (it->t < kAt[it->logged]) {
					++it;
					continue;
				}
				const bool carThere = S::DOES_VEHICLE_EXIST(it->veh), pedThere = S::DOES_CHAR_EXIST(it->ped);
				float      cx = 0, cy = 0, cz = 0, px = 0, py = 0, pz = 0, speed = 0;
				if (carThere) {
					S::GET_CAR_COORDINATES(it->veh, &cx, &cy, &cz);
					S::GET_CAR_SPEED(it->veh, &speed);
				}
				if (pedThere) {
					S::GET_CHAR_COORDINATES(it->ped, &px, &py, &pz);
				}
				const bool seated = carThere && pedThere && S::IS_CHAR_IN_CAR(it->ped, it->veh);
				// Where the body really is (it can drop through the car while still counted in it): its
				// pelvis bone in the car's frame, against where it was in the seat just before the hit.
				float pelvis[3] = { 0.0f, 0.0f, 0.0f };
				PelvisInCar(it->ped, it->veh, pelvis);
				LC_LOG("killed occupant %d of vehicle %d (by %s), %.1f s on: %s%s, pelvis at %.2f %.2f %.2f in the car's frame (seated: %.2f %.2f %.2f); car "
					   "speed %.1f m/s (%.1f when hit)",
					it->ped, it->veh, kKillVariantNames[it->variant % 3], it->t, !pedThere ? "gone" : seated ? "still in its seat" : "OUT OF THE CAR",
					pedThere && S::IS_PED_RAGDOLL(it->ped) ? ", ragdoll" : "", pelvis[0], pelvis[1], pelvis[2], it->seated[0], it->seated[1], it->seated[2], speed,
					it->speedAtKill);
				(void)px, (void)py, (void)pz, (void)cx, (void)cy, (void)cz;
				if (++it->logged >= 3) {
					it = seatWatch.erase(it);
				} else {
					++it;
				}
			}
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
					const float mx = x - it->x, my = y - it->y;
					const float moved = std::sqrt(mx * mx + my * my);
					// Which way it went against where Minecraft pushed (0 = straight along the push).
					const float angle = AngleBetween(mx, my, it->dirX, it->dirY);
					const float along = moved > 1e-4f ? (mx * it->dirX + my * it->dirY) : 0.0f;
					static constexpr const char* kVariants[] = { "world", "ped frame", "old flags", "no force" };
					LC_LOG("knockback: ped %d moved %.2f m in %.1f s, %.2f m along the push (%.0f deg off it; push GTA %.2f %.2f, ped heading %.0f, %s force %.1f, ragdoll %d)",
						it->ped, moved, kKnockbackCheckSeconds, along, angle, it->dirX, it->dirY, it->heading, kVariants[it->variant & 3], it->force,
						S::IS_PED_RAGDOLL(it->ped));
					auto& vs = variantStats[it->variant & 3];
					++vs.n;
					vs.moved += along;
					vs.angle += angle;
					if (Cfg().debugKnockbackVariant >= 0 && vs.n % 3 == 0) {
						for (int v = 0; v < 4; ++v) {
							const auto& st = variantStats[v];
							if (st.n) {
								LC_LOG("knockback variant %s: %u hits, %.2f m along the push and %.0f deg off it on average", kVariants[v], st.n, st.moved / st.n,
									st.angle / st.n);
							}
						}
					}
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
				// One of GTA's explosions (not our own): it knocks the player over (HostDrive, RagdollOnVehicleHit),
				// away from what blew up when that is known.
				if (weapon == kWeaponExplosion && blastProof <= 0.0f && deficit >= 20.0f && p && p->m_pMatrix) {
					const CEntity* src = p->m_pLastDamageEntity;
					float          gx = -p->m_pMatrix->up.x, gy = -p->m_pMatrix->up.y;  // else backwards
					if (src && src->m_pMatrix) {
						gx = p->m_pMatrix->pos.x - src->m_pMatrix->pos.x;
						gy = p->m_pMatrix->pos.y - src->m_pMatrix->pos.y;
					}
					char what[96];
					std::snprintf(what, sizeof(what), "a GTA explosion took %.0f health off the player", deficit);
					HostDrive::KnockDown(gx, gy, std::clamp(deficit * 0.15f, 8.0f, 35.0f), 3000, what);
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

		// DebugTestCar=N (not in the default ini): N s into play, an Admiral is parked 4 m east of the
		// player facing north (its left side toward the player), frozen, with a driver and a front
		// passenger who stay put; its windows and its people's health are logged as they change. 90 s
		// later it is moved 6 m in front of the camera, left side on, for a screenshot. Minecraft can
		// then shoot it from where the player stands (config/libertycraft-autorun.txt).
		// DebugWanted=N (not in the default ini): N s into play, 2 wanted stars; then, every 3 s for a
		// minute, how interested the police are: the wanted level, cops within 80 m and how many of
		// them are in combat or shooting, and what GTA has done to the player.
		void WantedHook(const Frame& a_frame)
		{
			auto& w = wantedTest;
			if (Cfg().debugWanted <= 0 || !a_frame.exists || a_frame.loading || a_frame.dead || !a_frame.mcInWorld) {
				return;
			}
			w.timer += a_frame.dt;
			if (!w.started) {
				if (w.timer >= static_cast<float>(Cfg().debugWanted)) {
					w.started = true;
					S::ALTER_WANTED_LEVEL(a_frame.player, 2);
					S::APPLY_WANTED_LEVEL_CHANGE_NOW(a_frame.player);
					LC_LOG("DebugWanted: 2 stars (player control %s, puppeting %d)", S::IS_PLAYER_CONTROL_ON(a_frame.player) ? "on" : "off", a_frame.puppeting);
				}
				return;
			}
			if ((w.since += a_frame.dt) > 60.0f || (w.logTimer -= a_frame.dt) > 0.0f) {
				return;
			}
			w.logTimer = 3.0f;
			unsigned wanted = 0;
			S::STORE_WANTED_LEVEL(a_frame.player, &wanted);
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_frame.ped, &px, &py, &pz);
			unsigned cops = 0, fighting = 0, shooting = 0, nearest = 9999;
			auto*    pool = CPools::ms_pPedPool;
			CPed*    playerPed = FindPlayerPed();
			for (int i = 0; pool && i < static_cast<int>(pool->m_nCount); ++i) {
				CPed* p = pool->Get(i);
				if (!p || p == playerPed || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float d = std::sqrt((m.x - px) * (m.x - px) + (m.y - py) * (m.y - py) + (m.z - pz) * (m.z - pz));
				const int   h = static_cast<int>(pool->GetIndex(p));
				if (d > 80.0f || !S::DOES_CHAR_EXIST(h) || S::IS_CHAR_DEAD(h)) {
					continue;
				}
				unsigned type = 0;
				S::GET_PED_TYPE(h, &type);
				if (type != kPedTypeCop) {
					continue;
				}
				++cops;
				nearest = std::min(nearest, static_cast<unsigned>(d));
				fighting += S::IS_PED_IN_COMBAT(h) ? 1u : 0u;
				shooting += S::IS_CHAR_SHOOTING(h) ? 1u : 0u;
			}
			unsigned health = 0;
			S::GET_CHAR_HEALTH(a_frame.ped, &health);
			LC_LOG("DebugWanted +%.0fs: wanted %u, %u cops within 80 m (nearest %u m), %u in combat, %u shooting; player health %u, control %s, puppeting %d",
				w.since, wanted, cops, cops ? nearest : 0u, fighting, shooting, health, S::IS_PLAYER_CONTROL_ON(a_frame.player) ? "on" : "off", a_frame.puppeting);
		}

		// DebugTestCar=-N: a lethal arrow through side window a_window (0 front left .. 3 rear right) of
		// a_car, handed to ApplyVehicleHit as Minecraft would send it.
		void TestCarArrow(const Frame& a_frame, int a_car, int a_window)
		{
			const bool a_left = a_window == kWindowLF || a_window == kWindowLR;
			const float along = a_window <= kWindowRF ? 0.6f : 0.35f;
			CVehicle* car = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(a_car)) : nullptr;
			float     lo[3], hi[3];
			if (!car || !car->m_pMatrix || !NpcBlocks::ModelBox(a_car, car->m_nModelIndex, lo, hi)) {
				return;
			}
			const CMatrix& m = *car->m_pMatrix;
			// 0.3 m outside the window, flying across the car.
			const float lx = a_left ? lo[0] - 0.3f : hi[0] + 0.3f, ly = lo[1] + along * (hi[1] - lo[1]), lz = lo[2] + 0.8f * (hi[2] - lo[2]);
			const float gx = m.pos.x + m.right.x * lx + m.up.x * ly + m.at.x * lz;
			const float gy = m.pos.y + m.right.y * lx + m.up.y * ly + m.at.y * lz;
			const float gz = m.pos.z + m.right.z * lx + m.up.z * ly + m.at.z * lz;
			const float s = a_left ? 1.0f : -1.0f;  // along the car's right axis
			const float dx = m.right.x * s, dy = m.right.y * s;
			const McVec at = GtaToMc(gx, gy, gz);
			hitPoint.valid = true;
			hitPoint.formId = VehicleActorId(static_cast<std::uint32_t>(a_car), 0);
			hitPoint.at[0] = at.x, hitPoint.at[1] = at.y, hitPoint.at[2] = at.z;
			hitPoint.yaw = GtaHeadingToMcYaw(std::atan2(-dx, dy) * kRadToDeg);
			hitPoint.pitch = 0.0f;
			proto::McEvent ev{};
			ev.type = proto::kEvHitActor;
			ev.formId = hitPoint.formId;
			ev.a = 30.0f;
			ev.flags = proto::kHitProjectile;
			ev.weapon = proto::kWeaponArrow;
			LC_LOG("DebugTestCar: an arrow through the %s", WindowName(a_window));
			ApplyVehicleHit(ev, a_frame, static_cast<std::uint32_t>(a_car), 0);
		}

		// Puts a car's wheels on the ground at (a_x, a_y), heading a_heading.
		void PlaceCar(int a_car, float a_x, float a_y, float a_zHint, float a_heading)
		{
			float ground = a_zHint;
			S::GET_GROUND_Z_FOR_3D_COORD(a_x, a_y, a_zHint + 1.5f, &ground);
			float     lo[3]{ 0.0f, 0.0f, -0.6f }, hi[3]{};
			CVehicle* v = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(a_car)) : nullptr;
			if (v) {
				NpcBlocks::ModelBox(a_car, v->m_nModelIndex, lo, hi);
			}
			S::SET_CAR_COORDINATES_NO_OFFSET(a_car, a_x, a_y, ground - lo[2]);
			S::SET_CAR_HEADING(a_car, a_heading);
		}

		void TestCarHook(const Frame& a_frame)
		{
			auto& t = testCar;
			if (Cfg().debugTestCar == 0 || t.stage >= 4 || !a_frame.exists || a_frame.loading || a_frame.dead || !a_frame.mcInWorld) {
				return;
			}
			t.timer += a_frame.dt;
			const unsigned model = S::GET_HASH_KEY("admiral");
			if (t.stage == 0) {
				if (t.timer >= static_cast<float>(std::abs(Cfg().debugTestCar))) {
					CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
					t.stage = 1;
				}
				return;
			}
			if (t.stage == 1) {
				if (!S::HAS_MODEL_LOADED(model)) {
					return;
				}
				float x = 0, y = 0, z = 0;
				S::GET_CHAR_COORDINATES(a_frame.ped, &x, &y, &z);
				S::CREATE_CAR(model, x + 4.0f, y, z, &t.car, true);
				S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
				if (!t.car) {
					LC_LOG("DebugTestCar: CREATE_CAR failed");
					t.stage = 4;
					return;
				}
				if (Cfg().debugTestCar < 0 && a_frame.mc) {
					// DebugTestCar=-N: 6 m in front of the camera instead, left side on, and it stays there.
					const float h = McYawToGtaHeading(a_frame.mc->yaw), r = h * kDegToRad;
					PlaceCar(t.car, x - std::sin(r) * 6.0f, y + std::cos(r) * 6.0f, z, h + 90.0f);
				} else {
					PlaceCar(t.car, x + 4.0f, y, z, 0.0f);
				}
				S::FREEZE_CAR_POSITION(t.car, true);
				S::CREATE_RANDOM_CHAR_AS_DRIVER(t.car, &t.driver);
				S::CREATE_RANDOM_CHAR_AS_PASSENGER(t.car, 0, &t.passenger);
				int rear[2] = { 0, 0 };
				if (Cfg().debugTestCar < 0) {  // in view: the back seats too, for the scripted arrows
					S::CREATE_RANDOM_CHAR_AS_PASSENGER(t.car, 1, &rear[0]);
					S::CREATE_RANDOM_CHAR_AS_PASSENGER(t.car, 2, &rear[1]);
				}
				for (const int p : { t.driver, t.passenger, rear[0], rear[1] }) {
					if (p) {
						S::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(p, true);  // they sit still when hit
						S::TASK_PAUSE(p, 600000);
					}
				}
				float cx = 0, cy = 0, cz = 0;
				S::GET_CAR_COORDINATES(t.car, &cx, &cy, &cz);
				float heading = 0.0f;
				S::GET_CAR_HEADING(t.car, &heading);
				LC_LOG("DebugTestCar: car %d parked at GTA %.2f %.2f %.2f heading %.0f (player at %.2f %.2f %.2f), driver %d, passenger %d", t.car, cx, cy, cz,
					heading, x, y, z, t.driver, t.passenger);
				t.stage = 2;
				t.timer = Cfg().debugTestCar < 0 ? -1e9f : 0.0f;  // (in view already: never moved)
				return;
			}
			if (!S::DOES_VEHICLE_EXIST(t.car)) {
				LC_LOG("DebugTestCar: the car is gone");
				t.stage = 4;
				return;
			}
			if ((t.logTimer -= a_frame.dt) <= 0.0f) {
				t.logTimer = 0.5f;
				char     line[160];
				unsigned body = 0, dh = 0, ph = 0;
				S::GET_CAR_HEALTH(t.car, &body);
				if (t.driver && S::DOES_CHAR_EXIST(t.driver)) {
					S::GET_CHAR_HEALTH(t.driver, &dh);
				}
				if (t.passenger && S::DOES_CHAR_EXIST(t.passenger)) {
					S::GET_CHAR_HEALTH(t.passenger, &ph);
				}
				int n = std::snprintf(line, sizeof(line), "windows");
				for (unsigned w = 0; w < 4; ++w) {  // (the natives only know the side windows)
					n += std::snprintf(line + n, sizeof(line) - n, " %s", S::IS_VEH_WINDOW_INTACT(t.car, w) ? "ok" : "BROKEN");
				}
				std::snprintf(line + n, sizeof(line) - n, " (LF RF LR RR); body %u; driver %u, passenger %u", body, dh, ph);
				if (std::strcmp(line, t.last) != 0) {
					std::strcpy(t.last, line);
					LC_LOG("DebugTestCar: %s", line);
				}
			}
			// In view (DebugTestCar=-N): from 8 s on, every 4 s, a lethal arrow through one side window
			// (front left, front right, rear left, rear right), through the same code as Minecraft's
			// (kEvHitPoint + kEvHitActor), to watch the bodies.
			if (Cfg().debugTestCar < 0 && t.stage == 2) {
				t.shotTimer += a_frame.dt;
				const int shot = t.shotTimer < 8.0f ? 0 : std::min(4, 1 + static_cast<int>((t.shotTimer - 8.0f) / 4.0f));
				if (shot > t.shots) {
					t.shots = shot;
					TestCarArrow(a_frame, t.car, shot - 1);
				}
			}
			if (t.stage == 2 && t.timer >= 90.0f && a_frame.mc) {
				// 6 m in front of the camera (Minecraft's look), turned so its left side faces it.
				const float h = McYawToGtaHeading(a_frame.mc->yaw);
				float       x = 0, y = 0, z = 0;
				S::GET_CHAR_COORDINATES(a_frame.ped, &x, &y, &z);
				const float r = h * kDegToRad;
				PlaceCar(t.car, x - std::sin(r) * 6.0f, y + std::cos(r) * 6.0f, z, h + 90.0f);
				LC_LOG("DebugTestCar: shown to the camera (heading %.0f), 6 m ahead", h);
				t.stage = 3;
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
			TestCarHook(a_frame);
			WantedHook(a_frame);
		}

		// ---- Diagnostics=1: who called into the game when it faulted ----------------------------------------
		// A vectored handler (first in line, it only looks): for the first few access violations, the
		// faulting address and the return addresses on the stack that point into GTAIV.exe or into this
		// plugin, so a crash in the game's code can be traced back to the call of ours that led there.
		std::uintptr_t selfBase = 0, selfEnd = 0;

		LONG CALLBACK FaultTrace(EXCEPTION_POINTERS* a_e)
		{
			static std::atomic<int> logged{ 0 };
			if (!a_e || !a_e->ExceptionRecord || a_e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || logged.fetch_add(1) >= 8) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			const auto* c = a_e->ContextRecord;
			char        buf[600];
			int         n = std::snprintf(buf, sizeof(buf), "fault: access violation at %08lX (reading %08lX), ecx %08lX, thread %lu; stack:", c->Eip,
						static_cast<unsigned long>(a_e->ExceptionRecord->ExceptionInformation[1]), c->Ecx, ::GetCurrentThreadId());
			const auto* sp = reinterpret_cast<const std::uintptr_t*>(c->Esp);
			int         found = 0;
			for (int i = 0; i < 768 && found < 16 && n < static_cast<int>(sizeof(buf)) - 16; ++i) {
				std::uintptr_t v = 0;
				__try {
					v = sp[i];
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					break;
				}
				if (v >= 0x401000 && v < 0x1000000) {
					n += std::snprintf(buf + n, sizeof(buf) - n, " %08X", static_cast<unsigned>(v));
					++found;
				} else if (v >= selfBase && v < selfEnd) {
					n += std::snprintf(buf + n, sizeof(buf) - n, " lc+%05X", static_cast<unsigned>(v - selfBase));
					++found;
				}
			}
			LC_LOG("%s", buf);
			return EXCEPTION_CONTINUE_SEARCH;
		}

		void InstallFaultTrace()
		{
			static bool installed = false;
			if (installed || !Cfg().diagnostics) {
				return;
			}
			installed = true;
			HMODULE self = nullptr;
			if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&FaultTrace),
					&self) &&
				self) {
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(self) + dos->e_lfanew);
				selfBase = reinterpret_cast<std::uintptr_t>(self);
				selfEnd = selfBase + nt->OptionalHeader.SizeOfImage;
			}
			::AddVectoredExceptionHandler(1, FaultTrace);
			LC_LOG("fault trace on (Diagnostics=1): plugin at %08X..%08X", static_cast<unsigned>(selfBase), static_cast<unsigned>(selfEnd));
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
			const bool  active = c.events || c.hurtFrames || c.explosions || c.hits || c.vehicleHits;
			if (diag || active) {
				LC_LOG("stats %.0fs: actor table %u writes, %.1f actors avg (max %u), gather %.0f us avg; events %u: hits %u (DAMAGE_CHAR %u, SET_CHAR_HEALTH %u, "
					   "resisted %u, stale %u, %u GTA damage, %u kills, %u ragdolls), explosions %u, deaths %u, arrows %u, unknown %u; player hurt in %u frames "
					   "-> %u kInHurt (%.0f GTA damage), dropped %u fall/drown + %u own-blast; health %s",
					secs, c.tableWrites, c.tableWrites ? double(c.actorsSent) / c.tableWrites : 0.0, c.actorsMax, c.tableWrites ? c.gatherUs / c.tableWrites : 0.0,
					c.events, c.hits, c.hitsDamageChar, c.hitsSetHealth, c.hitsResisted, c.hitsStale, c.hitGtaDamage, c.kills, c.ragdolls, c.explosions, c.deaths,
					c.arrows, c.unknownEvents, c.hurtFrames, c.hurtsSent, c.hurtGtaDamage, c.hurtDroppedIgnored, c.hurtDroppedBlast,
					owned.engaged ? "owned by Minecraft" : "GTA's");
				LC_LOG("stats %.0fs: vehicles %.1f per table write; vehicle hits %u (stale %u, explosions left to GTA %u; landed on the car %u, off it %u, "
					   "no hit point %u), occupants hit %u, windows %u, set on fire or blown up %u; crimes %u, fought back %u, ran %u, drivers fled %u",
					secs, c.tableWrites ? double(c.vehiclesSent) / c.tableWrites : 0.0, c.vehicleHits, c.vehicleHitsStale, c.vehicleBlastHits, c.vehiclePointsOnCar,
					c.vehiclePointsOff, c.vehicleNoPoint, c.occupantHits, c.windows, c.wrecked, c.crimes, c.fights, c.flees, c.driversFled);
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
		reacted.clear();
		seatWatch.clear();
		wantedTest = WantedTest{};
		shoves.clear();
		warpTimer = 0.0f;
		testCar = TestCar{};
		ClearActorTable();
	}

	std::uint32_t Tick(const Frame& a_frame)
	{
		InstallFaultTrace();
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
			case proto::kEvHitPoint:
				hitPoint.valid = true;
				hitPoint.formId = ev.formId;
				hitPoint.at[0] = ev.a, hitPoint.at[1] = ev.b, hitPoint.at[2] = ev.c;
				hitPoint.yaw = ev.d;
				std::memcpy(&hitPoint.pitch, &ev.flags, sizeof(float));
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
		NpcBlocks::Tick(a_frame);  // Minecraft's blocks stop GTA's peds and vehicles (its own NpcBlocks=1)
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
		CheckSeats(a_frame.dt);
		for (auto it = attackers.begin(); it != attackers.end();) {
			it = (it->age += a_frame.dt) > kHostileMemory ? attackers.erase(it) : it + 1;
		}
		for (auto it = reacted.begin(); it != reacted.end();) {
			it = (it->age += a_frame.dt) > kReactAgainSeconds ? reacted.erase(it) : it + 1;
		}
		TestHooks(a_frame);
		LogStats();
		return popped;
	}
}
