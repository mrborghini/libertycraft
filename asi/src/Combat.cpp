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
#include "collision/Rays.h"
#include "combat/CombatMath.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>
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
		constexpr float    kVehicleRange = 60.0f;          // ground vehicles (several records each) within this (helicopters: kAircraftRange)
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
			std::uint32_t corpseHits = 0, corpseBumps = 0, bumpNudges = 0, bumpStumbles = 0, bumpKnockdowns = 0;
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
			float    hudHealth = 200.0f;                // the health GTA's HUD is shown (Minecraft's, CombatMath.h HudHealth)
			bool     mirroring = false;                 // GTA's HUD shows Minecraft's health and armour (logged once)
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

		// A lethal hit's killing damage, after the ragdoll it was given (ApplyHit, PendingKills).
		struct PendingKill
		{
			int             ped;
			proto::McEvent  ev;
			unsigned        before;
			float           age;
			int             frames;
			float           x, y, z;  // where it stood when hit
			float           gx, gy;   // the push
		};
		std::vector<PendingKill> pendingKills;
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
			bool          started = false;
			float         timer = 0.0f, logTimer = 0.0f, since = 0.0f;
			std::uint32_t shots = 0, shotFrames = 0, frames = 0;  // this window: cop shots (per frame), frames with one
			std::uint32_t gunHits = 0, gunHitsLogged = 0;         // the player hurt by gunfire (BridgePlayerDamage)
		} wantedTest;

		// DebugTestCar: a parked car with people in it, east of the player, then shown to the camera.
		struct TestCar
		{
			int   stage = 0;  // 0 waiting, 1 model requested, 2 parked, 3 shown, 4 done
			float timer = 0.0f;
			int   car = 0, driver = 0, passenger = 0;
			int   cover = 0;  // DebugCarCover: the ped beyond the bonnet
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

		// ---- vehicles we know ---------------------------------------------------------------------------------
		// How many frames we have seen each vehicle near the player (AddVehicles). A vehicle created a
		// moment ago isn't put together yet (its skeleton, its seats): the window and seat natives read
		// garbage on it and crashed the game in tests, so it is left alone for its first frames.
		constexpr std::uint32_t kSettledFrames = 30;
		struct SeenVehicle
		{
			std::uint32_t first, last;
		};
		std::unordered_map<int, SeenVehicle> seenVehicles;
		std::uint32_t                         frameNo = 0;

		void NoteVehicle(int a_handle)
		{
			auto [it, fresh] = seenVehicles.try_emplace(a_handle, SeenVehicle{ frameNo, frameNo });
			it->second.last = frameNo;
			(void)fresh;
		}

		bool Settled(int a_handle)
		{
			const auto it = seenVehicles.find(a_handle);
			return it != seenVehicles.end() && frameNo - it->second.first >= kSettledFrames;
		}

		void PruneVehicles()
		{
			if (frameNo % 600 != 0) {
				return;
			}
			for (auto it = seenVehicles.begin(); it != seenVehicles.end();) {
				it = frameNo - it->second.last > 600 ? seenVehicles.erase(it) : std::next(it);
			}
		}

		// ---- vehicle shapes ---------------------------------------------------------------------------------
		// Each car model's shape (CombatMath.h VehicleShape), measured once with line probes against the
		// collision of one of its cars (VEHICLES only, nothing else counts): its top every
		// 1/kShapeSlices of the length (five vertical probes across), its ends (bumper height) and its
		// half width at door height (no mirrors), the greenhouse's at window height. Until a car of the
		// model has been measured (parked, upright, unhurt, near the player), a typical saloon in its
		// model box stands in (GuessShape). Bikes, boats and helicopters stay their model box.
		struct ShapeEntry
		{
			VehicleShape  shape;
			bool          measured = false;
			int           tries = 0;
			std::uint32_t nextTry = 0;  // frameNo
			bool          logged = false;
		};
		std::unordered_map<std::int32_t, ShapeEntry> shapes;
		bool                                         shapeProbed = false;  // this frame (one model a frame)
		constexpr int                                kShapeTries = 4;
		constexpr float                              kShapeProbeRange = 40.0f;

		void LocalToWorld(const CMatrix& a_m, float a_x, float a_y, float a_z, float a_out[3])
		{
			// (IV-SDK's CMatrix: "up" is the forward (y) axis, "at" the up (z) axis.)
			a_out[0] = a_m.pos.x + a_m.right.x * a_x + a_m.up.x * a_y + a_m.at.x * a_z;
			a_out[1] = a_m.pos.y + a_m.right.y * a_x + a_m.up.y * a_y + a_m.at.y * a_z;
			a_out[2] = a_m.pos.z + a_m.right.z * a_x + a_m.up.z * a_y + a_m.at.z * a_z;
		}

		// A probe from a_from to a_to (the vehicle's frame) against a_veh alone: where it first meets
		// it, in the vehicle's frame. Other vehicles in the way are skipped.
		int shapeRays = 0;
		bool ProbeVehicle(const CVehicle* a_veh, const float a_from[3], const float a_to[3], float a_hit[3])
		{
			const CMatrix& m = *a_veh->m_pMatrix;
			float          from[3], to[3];
			LocalToWorld(m, a_from[0], a_from[1], a_from[2], from);
			LocalToWorld(m, a_to[0], a_to[1], a_to[2], to);
			for (int pass = 0; pass < 3; ++pass) {
				tLineOfSightResults res;
				++shapeRays;
				--col::rayCounters.rays;  // not a map probe
				if (!col::CastGta(from, to, res, VEHICLES)) {
					return false;
				}
				const float* p = &res.m_vEndPosition.x;
				if (!std::isfinite(p[0] + p[1] + p[2])) {
					return false;
				}
				const auto* inst = reinterpret_cast<const rage::phInst*>(res.m_pInst);
				if (!inst || inst->m_pEntity == a_veh) {
					const float d[3] = { p[0] - m.pos.x, p[1] - m.pos.y, p[2] - m.pos.z };
					a_hit[0] = d[0] * m.right.x + d[1] * m.right.y + d[2] * m.right.z;
					a_hit[1] = d[0] * m.up.x + d[1] * m.up.y + d[2] * m.up.z;
					a_hit[2] = d[0] * m.at.x + d[1] * m.at.y + d[2] * m.at.z;
					return true;
				}
				// Something else first: go on from just past it.
				const float dir[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
				const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
				const float along = ((p[0] - from[0]) * dir[0] + (p[1] - from[1]) * dir[1] + (p[2] - from[2]) * dir[2]) / std::max(len, 1e-4f) + 0.02f;
				if (len < 1e-4f || along >= len) {
					return false;
				}
				for (int k = 0; k < 3; ++k) {
					from[k] += dir[k] / len * along;
				}
			}
			return false;
		}

		bool ProbeShape(const CVehicle* a_veh, const float a_lo[3], const float a_hi[3], VehicleShape& a_out)
		{
			VehicleShape s = GuessShape(a_lo, a_hi, true);
			const float  w = a_hi[0] - a_lo[0], len = a_hi[1] - a_lo[1], cx = s.centreX, cyBox = (a_lo[1] + a_hi[1]) * 0.5f;
			const float  d = len / static_cast<float>(kShapeSlices);
			float        tops[kShapeSlices];
			int          first = -1, last = -1, topHits = 0;
			float        hit[3];
			// 1. The top, slice by slice.
			for (int i = 0; i < kShapeSlices; ++i) {
				const float y = a_lo[1] + (static_cast<float>(i) + 0.5f) * d;
				tops[i] = -1e9f;
				for (const float k : { -0.3f, -0.15f, 0.0f, 0.15f, 0.3f }) {
					const float from[3] = { cx + k * w, y, a_hi[2] + 0.5f }, to[3] = { cx + k * w, y, a_lo[2] - 0.05f };
					if (ProbeVehicle(a_veh, from, to, hit)) {
						tops[i] = std::max(tops[i], hit[2]);
					}
				}
				if (tops[i] > -1e8f) {
					++topHits;
					first = first < 0 ? i : first;
					last = i;
				}
			}
			if (topHits < kShapeSlices / 2) {
				LC_LOG("vehicle shape: only %d of %d slices met the car from above", topHits, kShapeSlices);
				return false;
			}
			// The lowest top (bonnet, boot) away from the ends, for the side probes' heights.
			float low = 1e9f, high = -1e9f;
			const int trim = std::max(1, (last - first) / 10);
			for (int i = first + trim; i <= last - trim; ++i) {
				if (tops[i] > -1e8f) {
					low = std::min(low, tops[i]);
					high = std::max(high, tops[i]);
				}
			}
			if (low > high) {
				return false;
			}
			s.top = high;
			const float zLow = a_lo[2] + 0.45f * (low - a_lo[2]), zHigh = a_lo[2] + 0.8f * (low - a_lo[2]);
			// 2. The ends, at bumper height.
			float nose = -1e9f, tail = 1e9f;
			for (const float k : { -0.25f, 0.0f, 0.25f }) {
				for (const float z : { zLow, zHigh }) {
					const float f0[3] = { cx + k * w, a_hi[1] + 0.5f, z }, f1[3] = { cx + k * w, cyBox, z };
					if (ProbeVehicle(a_veh, f0, f1, hit)) {
						nose = std::max(nose, hit[1]);
					}
					const float t0[3] = { cx + k * w, a_lo[1] - 0.5f, z }, t1[3] = { cx + k * w, cyBox, z };
					if (ProbeVehicle(a_veh, t0, t1, hit)) {
						tail = std::min(tail, hit[1]);
					}
				}
			}
			s.nose = nose > -1e8f ? nose : a_lo[1] + d * static_cast<float>(last + 1);
			s.tail = tail < 1e8f ? tail : a_lo[1] + d * static_cast<float>(first);
			if (s.nose - s.tail < 0.5f) {
				return false;
			}
			// 3. The half width at door height, along the middle 70%.
			std::vector<float> halves;
			for (int j = 0; j < 12; ++j) {
				const float y = s.tail + (s.nose - s.tail) * (0.15f + 0.7f * static_cast<float>(j) / 11.0f);
				for (const float side : { -1.0f, 1.0f }) {
					for (const float z : { zLow, zHigh }) {
						const float from[3] = { cx + side * (w * 0.5f + 0.5f), y, z }, to[3] = { cx, y, z };
						if (ProbeVehicle(a_veh, from, to, hit)) {
							halves.push_back(std::fabs(hit[0] - cx));
						}
					}
				}
			}
			if (halves.size() < 8) {
				LC_LOG("vehicle shape: only %u side probes met the car", static_cast<unsigned>(halves.size()));
				return false;
			}
			std::sort(halves.begin(), halves.end());
			s.halfWidth = std::clamp(halves[halves.size() * 3 / 4], 0.3f, w * 0.5f);
			// 4. The greenhouse: where the top rises above the bonnet and boot, at window height.
			const float zWin = (low + high) * 0.5f + 0.05f;
			std::vector<float> cabin;
			if (high - low > 0.25f) {
				for (int i = first; i <= last; ++i) {
					if (tops[i] < low + 0.25f || (i - first) % 3 != 0) {
						continue;
					}
					const float y = a_lo[1] + (static_cast<float>(i) + 0.5f) * d;
					for (const float side : { -1.0f, 1.0f }) {
						const float from[3] = { cx + side * (w * 0.5f + 0.5f), y, zWin }, to[3] = { cx, y, zWin };
						if (ProbeVehicle(a_veh, from, to, hit)) {
							cabin.push_back(std::fabs(hit[0] - cx));
						}
					}
				}
			}
			if (cabin.size() >= 2) {
				std::sort(cabin.begin(), cabin.end());
				s.cabinHalfWidth = std::clamp(cabin[cabin.size() / 2], 0.25f, s.halfWidth);
			} else {
				s.cabinHalfWidth = high - low > 0.25f ? s.halfWidth * 0.78f : 0.0f;
			}
			// The top profile over the body's length (the probes' slices covered the model box).
			s.slices = kShapeSlices;
			const float sd = s.SliceLength();
			for (int j = 0; j < s.slices; ++j) {
				const float y0 = s.tail + sd * static_cast<float>(j), y1 = y0 + sd;
				float       t = -1e9f;
				for (int i = 0; i < kShapeSlices; ++i) {
					const float a0 = a_lo[1] + d * static_cast<float>(i);
					if (a0 < y1 && a0 + d > y0 && tops[i] > -1e8f) {
						t = std::max(t, tops[i]);
					}
				}
				if (t < -1e8f) {  // (a slice no probe met: the nearest that did)
					const int   i = std::clamp(static_cast<int>(((y0 + y1) * 0.5f - a_lo[1]) / d), first, last);
					int         best = first;
					for (int k = first; k <= last; ++k) {
						if (tops[k] > -1e8f && std::abs(k - i) < std::abs(best - i)) {
							best = k;
						}
					}
					t = tops[best];
				}
				s.tops[j] = t;
			}
			a_out = s;
			return true;
		}

		const VehicleShape& ShapeOf(int a_handle, CVehicle* a_veh, const float a_lo[3], const float a_hi[3], float a_dist2)
		{
			const bool car = a_veh->m_nVehicleType == VEHICLE_TYPE_AUTOMOBILE;
			auto [it, fresh] = shapes.try_emplace(a_veh->m_nModelIndex);
			ShapeEntry& e = it->second;
			if (fresh) {
				e.shape = GuessShape(a_lo, a_hi, car);
				e.measured = !car;  // (nothing to measure)
			}
			if (e.measured || shapeProbed || e.tries >= kShapeTries || frameNo < e.nextTry || a_dist2 > kShapeProbeRange * kShapeProbeRange) {
				return e.shape;
			}
			// Only a car that stands still, upright and undamaged, settled in the world.
			CVector v{};
			a_veh->GetVelocity(&v);
			unsigned health = 0;
			S::GET_CAR_HEALTH(a_handle, &health);
			if (a_veh->m_pMatrix->at.z < 0.97f || v.x * v.x + v.y * v.y + v.z * v.z > 0.25f || health < 900 || S::IS_CAR_DEAD(a_handle) || !Settled(a_handle)) {
				return e.shape;
			}
			shapeProbed = true;
			const auto t0 = Qpc();
			shapeRays = 0;
			VehicleShape measured;
			unsigned     model = 0;
			S::GET_CAR_MODEL(a_handle, &model);
			const char* name = S::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
			if (!ProbeShape(a_veh, a_lo, a_hi, measured)) {
				++e.tries;
				e.nextTry = frameNo + 300;
				LC_LOG("vehicle shape %s: probing car %d failed (try %d of %d); a typical car's shape stands in", name, a_handle, e.tries, kShapeTries);
				return e.shape;
			}
			e.shape = measured;
			e.measured = true;
			const VehicleShape& m = e.shape;
			char profile[160];
			int  n = 0;
			for (int i = 0; i < 8; ++i) {  // the top at 8 points from the tail to the nose
				const float y = m.tail + (static_cast<float>(i) + 0.5f) * m.Length() / 8.0f;
				n += std::snprintf(profile + n, sizeof(profile) - static_cast<std::size_t>(n), " %.2f", ShapeTop(m, y - 0.01f, y + 0.01f, TopOf::kHighest, 0.0f) - m.bottom);
			}
			LC_LOG("vehicle shape %s (model %08X): model box %.2f x %.2f x %.2f m (x %+.2f..%+.2f, y %+.2f..%+.2f); body %.2f wide (%.2f less: mirrors), "
				   "%.2f long (tail %+.2f, nose %+.2f), greenhouse %.2f wide, top %.2f m over the wheels' bottoms, tops from the tail:%s; %d probes, %.2f ms",
				name, model, a_hi[0] - a_lo[0], a_hi[1] - a_lo[1], a_hi[2] - a_lo[2], a_lo[0], a_hi[0], a_lo[1], a_hi[1], 2.0f * m.halfWidth,
				a_hi[0] - a_lo[0] - 2.0f * m.halfWidth, m.Length(), m.tail, m.nose, 2.0f * m.cabinHalfWidth, m.top - m.bottom, profile, shapeRays,
				static_cast<double>(Qpc() - t0) / QpcPerUs() / 1000.0);
			return e.shape;
		}

		// Vehicles near the player, nearest first, as pieces along their length (proto::kActorVehicle)
		// in whatever room the peds left. Not the one the player uses (sits in or is getting into).
		void AddVehicles(const Frame& a_frame, float a_px, float a_py, float a_pz)
		{
			sentVehicles.clear();
			vehicleCandidates.clear();
			shapeProbed = false;
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
				// Helicopters out to a crossbow firework's reach (CombatMath.h kAircraftRange).
				const float range = veh->m_nVehicleType == VEHICLE_TYPE_HELI ? kAircraftRange : kVehicleRange;
				if (d2 <= range * range) {
					vehicleCandidates.emplace_back(VehicleSortKey(d2, range), slot);
					NoteVehicle(static_cast<int>(pool->GetIndex(veh)));
				}
			}
			std::sort(vehicleCandidates.begin(), vehicleCandidates.end());
			for (const auto& [key, slot] : vehicleCandidates) {
				CVehicle* veh = pool->Get(slot);
				const int handle = veh ? static_cast<int>(pool->GetIndex(veh)) : 0;
				if (!handle || handle == playerCar || !S::DOES_VEHICLE_EXIST(handle)) {
					continue;
				}
				const auto& vp = veh->m_pMatrix->pos;
				const float d2 = (vp.x - a_px) * (vp.x - a_px) + (vp.y - a_py) * (vp.y - a_py) + (vp.z - a_pz) * (vp.z - a_pz);
				float lo[3], hi[3];
				if (!NpcBlocks::ModelBox(handle, veh->m_nModelIndex, lo, hi)) {
					continue;
				}
				const CMatrix& mat = *veh->m_pMatrix;
				// (|cos| + |sin| of its heading against the world axes: how much its boxes must shrink.)
				const float axisLen = std::hypot(mat.up.x, mat.up.y);
				const float e = axisLen > 1e-3f ? (std::fabs(mat.up.x) + std::fabs(mat.up.y)) / axisLen : 1.0f;
				const VehicleShape& shape = ShapeOf(handle, veh, lo, hi, d2);
				const auto          layout = VehiclePieces(shape, e);
				auto it = shapes.find(veh->m_nModelIndex);
				if (it != shapes.end() && it->second.measured && !it->second.logged && Cfg().diagnostics && veh->m_nVehicleType == VEHICLE_TYPE_AUTOMOBILE) {
					// Once a model: its boxes (lined up with the world axes: e = 1), front to back.
					it->second.logged = true;
					const auto l1 = VehiclePieces(shape, 1.0f);
					char       line[400];
					int        n = 0;
					for (std::uint32_t i = 0; i < l1.count && n < static_cast<int>(sizeof(line)) - 40; ++i) {
						n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), "%s%+.2f:%.2fx%.2f", i == l1.bodyCount ? " | cabin" : " ", l1.offset[i],
							l1.size[i], l1.height[i]);
					}
					unsigned model = 0;
					S::GET_CAR_MODEL(handle, &model);
					LC_LOG("vehicle pieces %s along an axis (offset from the middle: side x height):%s", S::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model), line);
				}
				if (records.size() + layout.count > proto::kMaxActors) {
					break;
				}
				// IV-SDK's CMatrix names its rows after GTA SA's: "up" is the forward (y) axis, "at" the up (z) axis.
				const auto&    fwd = mat.up;
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
					// The piece's bottom centre, in the vehicle's frame and then the world's.
					float at[3];
					LocalToWorld(mat, shape.centreX + layout.side[piece], shape.Middle() + layout.offset[piece], shape.bottom, at);
					const McVec feet = GtaToMc(at[0], at[1], at[2]);
					r.x = static_cast<float>(feet.x);
					r.y = static_cast<float>(feet.y);
					r.z = static_cast<float>(feet.z);
					r.yaw = GtaHeadingToMcYaw(heading);
					r.width = layout.size[piece];
					r.height = layout.height[piece];
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
					if (p->m_nPedFlags2.bInCar || S::IS_CHAR_IN_ANY_CAR(handle)) {
						continue;  // slumped in a seat: hits go through the car
					}
					// A corpse on the ground: a low box over its torso, between the pelvis and the neck
					// (hit and pushed by Minecraft, not solid).
					S::Vector3 pelvis{}, neck{};
					S::GET_PED_BONE_POSITION(handle, 0x1A1 /* pelvis */, 0.0f, 0.0f, 0.0f, &pelvis);
					S::GET_PED_BONE_POSITION(handle, 0x4B4 /* neck */, 0.0f, 0.0f, 0.0f, &neck);
					const float bx = neck.x - pelvis.x, by = neck.y - pelvis.y;
					if (std::isfinite(pelvis.x + pelvis.y + pelvis.z + neck.x + neck.y + neck.z) && bx * bx + by * by < 4.0f) {
						x = (pelvis.x + neck.x) * 0.5f;
						y = (pelvis.y + neck.y) * 0.5f;
						z = std::min(pelvis.z, neck.z) - 0.25f + feetDrop;  // (feetDrop comes off below)
						r.width = std::clamp(std::sqrt(bx * bx + by * by) + 0.6f, 0.9f, 1.6f);
					} else {
						z = m.z - 0.3f + feetDrop;
						r.width = 1.2f;
					}
					r.height = 0.6f;
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

		// SWITCH_PED_TO_RAGDOLL with its kind (CombatMath.h kRagdollFall / kRagdollBalance): IV-SDK declares
		// that argument a bool, so the native is called with plain ints.
		bool SwitchToRagdoll(int a_ped, int a_minMs, int a_maxMs, int a_kind)
		{
			return ::NativeInvoke::Invoke<b8>(NATIVE_SWITCH_PED_TO_RAGDOLL, a_ped, a_minMs, a_maxMs, a_kind, 0, 0, 0);
		}

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

		// GTA IV 1.0.8.0's CCrime::ReportCrime(eCrimeType, CEntity* victim, CPed* criminal) at 0xA503E0
		// (cdecl): what the game calls when the player hurts someone (CombatMath.h GtaCrime). With the
		// player as the criminal, GTA's own wanted system takes it from there: who saw it, the points
		// toward each star, escalation as the crimes go on, the police response and the decay.
		using ReportCrimeFn = void(__cdecl*)(int, void*, void*);
		ReportCrimeFn reportCrime = nullptr;
		int           reportCrimeState = -1;  // -1 not checked, 0 unavailable, 1 ok
		unsigned      copKills = 0;           // (the fallback's escalation) police killed in this wanted episode

		bool ReportGtaCrime(int a_crime, void* a_victim)
		{
			if (reportCrimeState < 0) {
				reportCrimeState = 0;
				if (plugin::gameVer == plugin::VERSION_1080) {
					auto* at = reinterpret_cast<const std::uint8_t*>(AddressSetter::gBaseAddress) + (0xA503E0 - 0x400000);
					// push ebp; mov ebp, esp; and esp, -16; sub esp, 0x24; mov eax, [ebp+0x10] (the criminal)
					static constexpr std::uint8_t kExpect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x24, 0x8B, 0x45, 0x10 };
					if (std::memcmp(at, kExpect, sizeof(kExpect)) == 0) {
						reportCrime = reinterpret_cast<ReportCrimeFn>(const_cast<std::uint8_t*>(at));
						reportCrimeState = 1;
						LC_LOG("crimes: Minecraft's attacks go to GTA IV's own crime reports (CCrime::ReportCrime): vanilla wanted levels");
					}
				}
				if (reportCrimeState == 0) {
					LC_LOG("crimes: GTA IV's crime report isn't as expected: wanted levels follow LibertyCraft's own rules");
				}
			}
			CPed* player = FindPlayerPed();
			if (reportCrimeState != 1 || !player) {
				return false;
			}
			reportCrime(a_crime, a_victim, player);
			return true;
		}

		const char* CrimeName(int a_crime)
		{
			switch (a_crime) {
			case kCrimeHitPed: return "HIT_PED";
			case kCrimeHitCop: return "HIT_COP";
			case kCrimeShootPed: return "SHOOT_PED";
			case kCrimeShootCop: return "SHOOT_COP";
			case kCrimeCauseExplosion: return "CAUSE_EXPLOSION";
			case kCrimeStabPed: return "STAB_PED";
			case kCrimeStabCop: return "STAB_COP";
			case kCrimeDestroyVehicle: return "DESTROY_VEHICLE";
			case kCrimeDamageToProperty: return "DAMAGE_TO_PROPERTY";
			default: return "?";
			}
		}

		// After Minecraft hurt or killed a_ped (of GET_PED_TYPE a_type; a_driving: it sits at the wheel of a_vehicle),
		// a_crime (CombatMath.h CrimeForAttack) with it as the victim (or a_vehicle, for damage to property).
		void AfterAttack(int a_ped, unsigned a_type, bool a_killed, const Frame& a_frame, int a_vehicle, int a_crime)
		{
			if (!Cfg().gtaCrimes || !a_frame.ped) {
				return;
			}
			const bool cop = a_type == kPedTypeCop;
			unsigned   cops = 0, witnesses = 0, wanted = 0;
			S::STORE_WANTED_LEVEL(a_frame.player, &wanted);
			if (wanted == 0) {
				copKills = 0;  // a new wanted episode
			}
			if (cop && a_killed) {
				++copKills;
			}
			void* victim = nullptr;
			if (a_crime == kCrimeDamageToProperty || a_crime == kCrimeDestroyVehicle) {
				victim = a_vehicle && CPools::ms_pVehiclePool ? static_cast<void*>(CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(a_vehicle))) : nullptr;
			} else {
				victim = CPools::ms_pPedPool ? static_cast<void*>(CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_ped))) : nullptr;
			}
			if (ReportGtaCrime(a_crime, victim)) {
				++counters.crimes;
				// GTA's own points stop climbing at some point (4 stars after a dozen police in tests);
				// the police killed this episode keep raising it as players expect (WantedForCopKills).
				unsigned now = 0, max = 0;
				S::STORE_WANTED_LEVEL(a_frame.player, &now);
				S::GET_MAX_WANTED_LEVEL(&max);
				const unsigned floor = std::min(WantedForCopKills(copKills), max ? max : 6u);
				if (cop && a_killed && floor > now) {
					S::ALTER_WANTED_LEVEL_NO_DROP(a_frame.player, floor);
					S::APPLY_WANTED_LEVEL_CHANGE_NOW(a_frame.player);
					S::STORE_WANTED_LEVEL(a_frame.player, &now);
				}
				LC_LOG("crime: Minecraft %s a %s -> GTA IV's report %s: wanted level %u -> %u (max %u; %u police killed this episode)", a_killed ? "killed" : "hurt",
					PedTypeName(a_type), CrimeName(a_crime), wanted, now, max, copKills);
			} else {
				CountWitnesses(a_ped, a_frame.ped, cops, witnesses);
				const unsigned want = WantedAfterAttack(wanted, cop, a_killed, cops, witnesses, copKills);
				if (want > wanted) {
					S::ALTER_WANTED_LEVEL_NO_DROP(a_frame.player, want);
					S::APPLY_WANTED_LEVEL_CHANGE_NOW(a_frame.player);
					unsigned now = 0;
					S::STORE_WANTED_LEVEL(a_frame.player, &now);
					++counters.crimes;
					LC_LOG("crime: Minecraft %s a %s (%u cops, %u witnesses near, %u police killed this episode): wanted level %u -> %u",
						a_killed ? "killed" : "hurt", PedTypeName(a_type), cops, witnesses, copKills, wanted, now);
				}
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

		// ---- bodies Minecraft runs into or hits (kEvBump; corpses) -----------------------------------------
		// Where a ped is (its root), dead or alive: from its matrix (the natives refuse dead peds).
		bool PedRoot(int a_ped, float& a_x, float& a_y, float& a_z)
		{
			CPed* p = CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_ped)) : nullptr;
			if (!p || !p->m_pMatrix) {
				return false;
			}
			a_x = p->m_pMatrix->pos.x, a_y = p->m_pMatrix->pos.y, a_z = p->m_pMatrix->pos.z;
			return true;
		}

		std::unordered_map<int, float> bumpCooldown;    // ped -> seconds until it can be knocked again
		std::unordered_map<int, float> speechCooldown;  // ped -> seconds until it complains again

		// A ped's physics state (1.0.8.0: CPed +0x7C8; 2 animated, 6 ragdoll; a settled corpse lies animated).
		int PhysicsState(int a_ped)
		{
			const CPed* p = CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(a_ped)) : nullptr;
			if (!p || plugin::gameVer != plugin::VERSION_1080) {
				return -1;
			}
			int v = 0;
			std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(p) + 0x7C8, sizeof(v));
			return v;
		}

		// A corpse takes a push. While its death ragdoll still moves (physics state 6) the push is a
		// force on the ragdoll. A settled corpse lies animated in its dead pose (state 2) and its dead
		// task won't let it be a ragdoll again (measured: SWITCH_PED_TO_RAGDOLL, any kind, makes it one
		// for a single frame, then it is back in its pose and the force is lost), so it is thrown
		// along an arc instead, as it lies (CorpseFlings): a_speed m/s along the push, a_lift up.
		struct CorpseFling
		{
			int   ped;
			float x, y, z;        // where it started (its root)
			float vx, vy, vz;     // m/s
			float above;          // its root's height over the ground where it lay
			float t;
		};
		std::vector<CorpseFling> corpseFlings;

		// (Measured: force 7.2 moves a fresh ragdoll 1 to 4 m; the arc covers about the same.)
		bool PushCorpse(int a_ped, float a_gx, float a_gy, float a_force, float a_speed, float a_lift)
		{
			float x = 0, y = 0, z = 0;
			if (!PedRoot(a_ped, x, y, z)) {
				return false;
			}
			if (shoves.size() < 16) {
				shoves.push_back({ a_ped, x, y, z, kKnockbackCheckSeconds, a_force, a_gx, a_gy, 0.0f, kKnockVariant });
			}
			if (PhysicsState(a_ped) == 6 || S::IS_PED_RAGDOLL(a_ped)) {
				Knock(a_ped, a_gx, a_gy, a_force, 0.0f, kKnockVariant);
				return true;
			}
			float ground = z - 0.2f;
			S::GET_GROUND_Z_FOR_3D_COORD(x, y, z + 0.5f, &ground);
			for (auto& f : corpseFlings) {
				if (f.ped == a_ped) {
					f = CorpseFling{ a_ped, x, y, z, a_gx * a_speed, a_gy * a_speed, a_lift, std::max(z - ground, 0.05f), 0.0f };
					return false;
				}
			}
			if (corpseFlings.size() < 16) {
				corpseFlings.push_back({ a_ped, x, y, z, a_gx * a_speed, a_gy * a_speed, a_lift, std::max(z - ground, 0.05f), 0.0f });
			}
			return false;
		}

		void CorpseFlings(float a_dt)
		{
			for (auto it = corpseFlings.begin(); it != corpseFlings.end();) {
				CorpseFling& f = *it;
				CPed* p = CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(f.ped)) : nullptr;
				if (!p || !p->m_pMatrix || !S::DOES_CHAR_EXIST(f.ped)) {
					it = corpseFlings.erase(it);
					continue;
				}
				f.t += a_dt;
				const float t = f.t;
				float       nx = f.x + f.vx * t, ny = f.y + f.vy * t, nz = f.z + f.vz * t - 4.9f * t * t;
				float       ground = nz - f.above;
				S::GET_GROUND_Z_FOR_3D_COORD(nx, ny, std::max(nz, f.z) + 1.0f, &ground);
				const bool landed = (t > 0.05f && nz <= ground + f.above) || t > 1.5f;
				if (landed) {
					nz = ground + f.above;
				}
				CVector to{ nx, ny, nz };
				p->Teleport(&to, false, true);
				if (landed) {
					it = corpseFlings.erase(it);
					continue;
				}
				++it;
			}
		}

		void HitCorpse(int a_ped, const proto::McEvent& a_ev)
		{
			float gx = 0.0f, gy = 0.0f;
			if (!PushDirToGta(a_ev.b, a_ev.c, gx, gy)) {
				LC_LOG_EVERY(1000, "hit on corpse %08X: no knockback direction", a_ev.formId);
				return;
			}
			const bool  projectile = (a_ev.flags & proto::kHitProjectile) != 0;
			const float force = CorpseHitForce(a_ev.d, a_ev.a, Cfg().hitForce);
			const bool  ragdoll = PushCorpse(a_ped, gx, gy, force, force * 0.3f, 2.0f);
			++counters.corpseHits;
			LC_LOG("hit corpse %08X for %.2f Minecraft (knockback %.2f%s): %s along GTA %.2f %.2f", a_ev.formId, a_ev.a, a_ev.d, projectile ? ", projectile" : "",
				ragdoll ? "its ragdoll pushed (force)" : "thrown as it lies (settled)", gx, gy);
		}

		// The Minecraft player ran into a ped's stand-in (proto::kEvBump; CombatMath.h BumpOf).
		void Bump(const proto::McEvent& a_ev, const Frame& a_frame)
		{
			std::uint32_t handle = 0;
			const int     ped = HandleFromActorId(a_ev.formId, handle) ? static_cast<int>(handle) : 0;
			if (!ped || ped == a_frame.ped || !S::DOES_CHAR_EXIST(ped)) {
				return;
			}
			float x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;
			if (!PedRoot(ped, x, y, z) || !PedRoot(a_frame.ped, px, py, pz)) {
				return;
			}
			// Away from the player, and along his run when he's moving.
			float mx = 0.0f, my = 0.0f;
			const bool moving = a_ev.a > 1.0f && PushDirToGta(a_ev.b, a_ev.c, mx, my);
			float ax = x - px, ay = y - py;
			const float al = std::sqrt(ax * ax + ay * ay);
			ax = al > 1e-3f ? ax / al : mx, ay = al > 1e-3f ? ay / al : my;
			float gx = ax + (moving ? mx : 0.0f), gy = ay + (moving ? my : 0.0f);
			const float gl = std::sqrt(gx * gx + gy * gy);
			if (gl < 1e-3f) {
				return;
			}
			gx /= gl, gy /= gl;
			const float speed = std::max(a_ev.a, 0.0f);
			const bool  newContact = (a_ev.flags & proto::kBumpNewContact) != 0;
			float&      cooldown = bumpCooldown[ped];
			unsigned    health = 0;
			const bool  dead = S::IS_CHAR_DEAD(ped) || (S::GET_CHAR_HEALTH(ped, &health), health == 0);
			if (dead) {
				// Dragged and rolled along: a push now and then while the player walks into it.
				if (cooldown > 0.0f) {
					return;
				}
				cooldown = 0.35f;
				const float force = CorpseBumpForce(speed);
				// (A walk drags it along the ground; faster throws it.)
				const bool ragdoll = PushCorpse(ped, gx, gy, force, std::max(speed, 1.5f) * 0.8f, speed > kBumpStumbleSpeed ? speed * 0.15f : 0.4f);
				++counters.corpseBumps;
				LC_LOG_EVERY(250, "bump: the player (%.1f m/s) pushed corpse %08X (%s)", speed, a_ev.formId, ragdoll ? "its ragdoll, force" : "as it lies");
				return;
			}
			if (S::IS_CHAR_IN_ANY_CAR(ped)) {
				return;
			}
			const BumpOutcome o = BumpOf(speed);
			if (o.kind != BumpOutcome::kNudge && cooldown <= 0.0f && (newContact || o.kind == BumpOutcome::kKnockdown)) {
				float heading = 0.0f;
				S::GET_CHAR_HEADING(ped, &heading);
				S::UNLOCK_RAGDOLL(ped, true);
				const int kind = o.kind == BumpOutcome::kStumble && Cfg().debugStumbleKind >= 0 ? Cfg().debugStumbleKind : o.ragdollKind;
				bool      switched = SwitchToRagdoll(ped, o.ragdollMs, o.ragdollMs, kind);
				if (!switched && o.kind == BumpOutcome::kKnockdown) {
					S::CLEAR_CHAR_TASKS_IMMEDIATELY(ped);
					switched = SwitchToRagdoll(ped, o.ragdollMs, o.ragdollMs, kind);
				}
				if (switched) {
					cooldown = o.kind == BumpOutcome::kKnockdown ? 2.0f : 1.5f;
					Knock(ped, gx, gy, o.force, heading, kKnockVariant);
					++counters.ragdolls;
					++(o.kind == BumpOutcome::kKnockdown ? counters.bumpKnockdowns : counters.bumpStumbles);
					if (shoves.size() < 16) {
						shoves.push_back({ ped, x, y, z, kKnockbackCheckSeconds, o.force, gx, gy, heading, kKnockVariant });
					}
					unsigned type = 0;
					S::GET_PED_TYPE(ped, &type);
					LC_LOG("bump: the player hit %s %08X at %.1f m/s%s%s: %s, ragdoll %d ms (%s), force %.1f along GTA %.2f %.2f, %.0f GTA damage", PedTypeName(type),
						a_ev.formId, speed, (a_ev.flags & proto::kBumpSprinting) ? " (sprinting)" : "", (a_ev.flags & proto::kBumpFlying) ? " (flying)" : "",
						o.kind == BumpOutcome::kKnockdown ? "knocked down" : "stumbles", o.ragdollMs, kind >= kRagdollBalance ? "balance" : kind == 1 ? "kind 1" : "fall",
						o.force, gx, gy, o.gtaDamage);
					if (o.gtaDamage > 0.0f) {
						proto::McEvent hurt{};
						hurt.type = proto::kEvBump;
						hurt.formId = a_ev.formId;
						hurt.a = o.gtaDamage / std::max(Cfg().pedDamageScale, 0.01f);  // (as Minecraft damage)
						hurt.weapon = proto::kWeaponUnarmed;
						if (health <= static_cast<unsigned>(o.gtaDamage + kDeathHealth) && pendingKills.size() < 16) {
							pendingKills.push_back({ ped, hurt, health, 0.0f, 0, x, y, z, gx, gy });  // dies in the ragdoll (PendingKills)
						} else {
							unsigned after = 0, damage = 0;
							DamagePed(ped, hurt.a, health, after, damage);
							if (damage > 0) {
								AfterAttack(ped, type, S::IS_CHAR_DEAD(ped), a_frame, 0, CrimeForAttack(0, proto::kWeaponUnarmed, type == kPedTypeCop));
							}
						}
					}
					return;
				}
			}
			// Walked into: out of the way (a step along the push, the overlap's worth), and a word about it.
			CPed* p = CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAt(static_cast<std::uint32_t>(ped)) : nullptr;
			if (!p || S::IS_PED_RAGDOLL(ped)) {
				return;
			}
			const float step = NudgeStep(std::max(a_ev.d, 0.0f));
			if (step > 0.0f) {
				CVector to{ x + gx * step, y + gy * step, z };
				p->Teleport(&to, false, true);
				++counters.bumpNudges;
			}
			float& talk = speechCooldown[ped];
			if (newContact && talk <= 0.0f) {
				talk = 6.0f;
				S::SAY_AMBIENT_SPEECH(ped, "BUMP", true, true, 0);
			}
			LC_LOG_EVERY(500, "bump: the player (%.1f m/s) walked into %08X: nudged %.2f m along GTA %.2f %.2f", speed, a_ev.formId, step, gx, gy);
		}

		// DebugBumpPed=N (not in the default ini): N s into play, a ped stands still 3 m ahead of the player
		// (for a Minecraft player to run, fall or glide into: tools/fake_minecraft.py --charge, or an
		// autorun's tp above it); its health is logged every second for 20 s.
		struct BumpPedTest
		{
			int   stage = 0;  // 0 waiting, 1 standing, 2 done
			float timer = 0.0f, logTimer = 0.0f;
			int   ped = 0;
		} bumpPedTest;

		void BumpPedHook(const Frame& a_frame)
		{
			auto& b = bumpPedTest;
			if (Cfg().debugBumpPed <= 0 || b.stage >= 2 || !a_frame.exists || a_frame.loading || !a_frame.mc) {
				return;
			}
			b.timer += a_frame.dt;
			if (b.stage == 0) {
				if (b.timer < static_cast<float>(Cfg().debugBumpPed)) {
					return;
				}
				float x = 0, y = 0, z = 0;
				S::GET_CHAR_COORDINATES(a_frame.ped, &x, &y, &z);
				const float hd = McYawToGtaHeading(a_frame.mc->yaw), h = hd * kDegToRad;
				const float tx = x - std::sin(h) * 3.0f, ty = y + std::cos(h) * 3.0f;
				S::CREATE_RANDOM_CHAR(tx, ty, z, &b.ped);
				if (!b.ped) {
					LC_LOG("DebugBumpPed: CREATE_RANDOM_CHAR failed");
					b.stage = 2;
					return;
				}
				float ground = z - 1.0f;
				S::GET_GROUND_Z_FOR_3D_COORD(tx, ty, z + 1.0f, &ground);
				S::SET_CHAR_COORDINATES(b.ped, tx, ty, ground);
				S::SET_CHAR_HEADING(b.ped, hd + 180.0f);
				S::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(b.ped, true);
				S::TASK_STAND_STILL(b.ped, 60000);
				LC_LOG("DebugBumpPed: ped %d (%08X) 3 m ahead of the player at GTA %.2f %.2f %.2f", b.ped, ActorIdFromHandle(static_cast<std::uint32_t>(b.ped)), tx, ty,
					ground);
				b.stage = 1;
				b.timer = 0.0f;
				return;
			}
			if ((b.logTimer += a_frame.dt) >= 1.0f) {
				b.logTimer = 0.0f;
				float x = 0, y = 0, z = 0;
				unsigned health = 0;
				const bool exists = S::DOES_CHAR_EXIST(b.ped);
				const bool dead = exists && S::IS_CHAR_DEAD(b.ped);
				if (exists) {
					S::GET_CHAR_HEALTH(b.ped, &health);
					PedRoot(b.ped, x, y, z);
				}
				LC_LOG("DebugBumpPed +%.0fs: ped %d %s, health %u, ragdoll %d, at GTA %.2f %.2f %.2f", b.timer, b.ped, !exists ? "gone" : dead ? "dead" : "alive", health,
					exists && S::IS_PED_RAGDOLL(b.ped), x, y, z);
			}
			if (b.timer > 20.0f) {
				b.stage = 2;
			}
		}

		void TickBumps(float a_dt)
		{
			for (auto* m : { &bumpCooldown, &speechCooldown }) {
				for (auto it = m->begin(); it != m->end();) {
					it = (it->second -= a_dt) <= -10.0f ? m->erase(it) : std::next(it);
				}
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
			if (before == 0 && ped && ped != a_frame.ped && S::DOES_CHAR_EXIST(ped)) {
				HitCorpse(ped, a_ev);  // dead: the body flies along the hit
				return;
			}
			if (before == 0) {  // gone
				++counters.hitsStale;
				LC_LOG_EVERY(1000, "hit on actor %08X ignored: no such ped any more", a_ev.formId);
				return;
			}
			++counters.hits;
			const bool crit = (a_ev.flags & proto::kHitCritical) != 0;
			float      gx = 0.0f, gy = 0.0f;
			// A killing blow on a ped on foot: GTA starts its scripted death clip as soon as the health
			// runs out, and a dead ped takes no ragdoll or push. So the ped goes limp and is shoved along
			// the knockback first, and the killing damage follows a couple of frames later (PendingKills),
			// once the ragdoll has it: it dies inside the ragdoll and flies, as from a gunshot. (In a
			// vehicle the occupant path keeps its seated death.)
			const unsigned wouldDo = PedDamageFromMc(a_ev.a, Cfg().pedDamageScale);
			const bool     lethal = wouldDo > 0 && before <= wouldDo + static_cast<unsigned>(kDeathHealth);
			if (lethal && Cfg().ragdollOnHit && PushDirToGta(a_ev.b, a_ev.c, gx, gy) && !S::IS_CHAR_IN_ANY_CAR(ped) && pendingKills.size() < 16) {
				float x = 0, y = 0, z = 0, heading = 0;
				S::GET_CHAR_COORDINATES(ped, &x, &y, &z);
				S::GET_CHAR_HEADING(ped, &heading);
				S::UNLOCK_RAGDOLL(ped, true);  // (some peds, gang members in their scenarios among them, have it locked)
				bool switched = S::SWITCH_PED_TO_RAGDOLL(ped, 4000, 4000, false, false, false, false);
				if (!switched) {
					// Refused (a scenario such as leaning or sitting holds it): out of it, then again.
					S::CLEAR_CHAR_TASKS_IMMEDIATELY(ped);
					switched = S::SWITCH_PED_TO_RAGDOLL(ped, 4000, 4000, false, false, false, false);
				}
				const float force = HitShoveForce(a_ev.d, Cfg().hitForce);
				Knock(ped, gx, gy, force, heading, kKnockVariant);
				++counters.ragdolls;
				pendingKills.push_back({ ped, a_ev, before, 0.0f, 0, x, y, z, gx, gy });
				LC_LOG("hit on %08X is lethal (%u of %u health): ragdolled (%s) and shoved (force %.1f) first, the killing damage follows", a_ev.formId, wouldDo,
					before, switched ? "switched" : "the switch was refused twice", force);
				return;
			}
			const char* how = DamagePed(ped, a_ev.a, before, after, damage);
			const bool  killed = S::IS_CHAR_DEAD(ped) || (damage > 0 && after == 0);  // GTA zeroes a ped's health as it dies
			if (killed) {
				++counters.kills;
			}
			int ragdollMs = 0;
			if (Cfg().ragdollOnHit && !killed && PushDirToGta(a_ev.b, a_ev.c, gx, gy)) {
				ragdollMs = RagdollMs(a_ev.d, crit);
				if (ragdollMs > 0) {
					float x = 0, y = 0, z = 0, heading = 0;
					S::GET_CHAR_COORDINATES(ped, &x, &y, &z);
					S::GET_CHAR_HEADING(ped, &heading);
					S::SWITCH_PED_TO_RAGDOLL(ped, ragdollMs, ragdollMs, false, false, false, false);
					// Minecraft's knockback: 0.4 for a plain hit, more for sprint hits / Knockback.
					const float force = HitShoveForce(a_ev.d, Cfg().hitForce);
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
				AfterAttack(ped, type, killed, a_frame, 0, CrimeForAttack(a_ev.flags, a_ev.weapon, type == kPedTypeCop));
			}
		}

		// The killing damage of lethal hits, once the ragdoll has the ped (at least 2 frames, then as
		// soon as IS_PED_RAGDOLL says so, at most 0.3 s).
		void PendingKills(const Frame& a_frame)
		{
			for (auto it = pendingKills.begin(); it != pendingKills.end();) {
				PendingKill& k = *it;
				k.age += a_frame.dt;
				++k.frames;
				const bool exists = S::DOES_CHAR_EXIST(k.ped);
				if (exists && !S::IS_CHAR_DEAD(k.ped) && (k.frames < 2 || (!S::IS_PED_RAGDOLL(k.ped) && k.age < 0.3f))) {
					++it;
					continue;
				}
				if (exists && !S::IS_CHAR_DEAD(k.ped)) {
					const bool ragdolled = S::IS_PED_RAGDOLL(k.ped);
					unsigned   before = 0, after = 0, damage = 0;
					S::GET_CHAR_HEALTH(k.ped, &before);
					const char* how = DamagePed(k.ped, k.ev.a, before ? before : k.before, after, damage);
					const bool  killed = S::IS_CHAR_DEAD(k.ped) || (damage > 0 && after == 0);
					if (killed) {
						++counters.kills;
					}
					float x = 0, y = 0, z = 0;
					S::GET_CHAR_COORDINATES(k.ped, &x, &y, &z);
					unsigned type = 0;
					S::GET_PED_TYPE(k.ped, &type);
					LC_LOG("hit %s %08X for %.2f Minecraft -> %u GTA damage (%s) %.2f s after its ragdoll (%s): health %u -> %u%s%s; moved %.2f m along the push "
						   "so far",
						PedTypeName(type), k.ev.formId, k.ev.a, damage, how, k.age, ragdolled ? "ragdolled" : "NOT ragdolled", before, after, killed ? ", killed" : "",
						(k.ev.flags & proto::kHitProjectile) ? ", projectile" : "", (x - k.x) * k.gx + (y - k.y) * k.gy);
					if (shoves.size() < 16) {
						const float force = HitShoveForce(k.ev.d, Cfg().hitForce);
						shoves.push_back({ k.ped, k.x, k.y, k.z, kKnockbackCheckSeconds, force, k.gx, k.gy, 0.0f, kKnockVariant });
					}
					if (damage > 0) {
						AfterAttack(k.ped, type, killed, a_frame, 0, CrimeForAttack(k.ev.flags, k.ev.weapon, type == kPedTypeCop));
					}
				}
				it = pendingKills.erase(it);
			}
		}

		// ---- breaking vehicle windows safely ---------------------------------------------------------------
		// SMASH_CAR_WINDOW (1.0.8.0: handler 0xB21D10, body 0xB1EB60) maps the window to a vehicle part
		// through a table (0xF4B3E8: windows 0 to 3 are parts 30 to 33), finds the part's bone in the model
		// (model info +0xCC, -1 if the model has no such window), then asks the vehicle's fragment
		// instance (virtual +0xA0) for the glass; the vehicle's matrix is read on the way. A vehicle the
		// pool doesn't know, or without a fragment instance, crashes it. IS_VEH_WINDOW_INTACT (0xB22DB0)
		// reads the bone's matrix from a skeleton without checking it: not used at all. The code is checked
		// once; anything unexpected (another game version) and windows are never broken.
		int  windowNatives = -1;   // -1 not checked yet, 0 unusable, 1 verified
		int  windowPart[4]{};      // the game's part for windows 0 to 3
		bool windowFaulted = false;

		bool WindowNativesVerified()
		{
			if (windowNatives >= 0) {
				return windowNatives == 1;
			}
			windowNatives = 0;
			if (plugin::gameVer != plugin::VERSION_1080) {
				LC_LOG("windows: not GTA IV 1.0.8.0: Minecraft hits never break car windows");
				return false;
			}
			const auto* base = reinterpret_cast<const std::uint8_t*>(AddressSetter::gBaseAddress);
			auto        at = [&](std::uint32_t a_va) { return base + (a_va - 0x400000u); };
			auto        rd32 = [&](std::uint32_t a_va) {
				std::uint32_t v = 0;
				std::memcpy(&v, at(a_va), 4);
				return v;
			};
			const auto rel = [&](std::uint32_t a_va) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(at(a_va))); };
			// The handler: two args, a call to the body.
			static constexpr std::uint8_t kHandler[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x40, 0x08, 0x8B, 0x48, 0x04, 0x8B, 0x10, 0x51, 0x52, 0xE8 };
			const bool handlerOk = std::memcmp(at(0xB21D10), kHandler, sizeof(kHandler)) == 0 && rel(0xB21D23) + rd32(0xB21D1F) == rel(0xB1EB60);
			// The body: the pool lookup, the part table, then (at 0xB1EC1A) the model's bone table and the
			// fragment instance's virtual.
			const bool tableOk = std::memcmp(at(0xB1EB7C), "\x8B\x0C\x8D", 3) == 0 && rd32(0xB1EB7F) == rel(0xF4B3E8);
			static constexpr std::uint8_t kBone[] = { 0x0F, 0xBF, 0x56, 0x2E, 0x8B, 0x04, 0x95 };
			static constexpr std::uint8_t kBone2[] = { 0x8B, 0x90, 0xCC, 0x00, 0x00, 0x00, 0x8B, 0x04, 0x8A, 0x83, 0xF8, 0xFF, 0x7E };
			static constexpr std::uint8_t kFrag[] = { 0x50, 0x8B, 0x06, 0x8B, 0x90, 0xA0, 0x00, 0x00, 0x00, 0x8B, 0xCE, 0xFF, 0xD2 };
			const bool boneOk = std::memcmp(at(0xB1EC1A), kBone, sizeof(kBone)) == 0 &&
			                    rd32(0xB1EC21) == static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(CModelInfo::ms_modelInfoPtrs)) &&
			                    std::memcmp(at(0xB1EC25), kBone2, sizeof(kBone2)) == 0 && std::memcmp(at(0xB1EC33), kFrag, sizeof(kFrag)) == 0;
			if (handlerOk && tableOk && boneOk) {
				for (int w = 0; w < 4; ++w) {
					windowPart[w] = static_cast<int>(rd32(0xF4B3E8 + 4u * static_cast<std::uint32_t>(w)));
				}
				windowNatives = std::all_of(std::begin(windowPart), std::end(windowPart), [](int a_p) { return a_p > 0 && a_p < 128; }) ? 1 : 0;
			}
			LC_LOG("windows: SMASH_CAR_WINDOW %s (handler %d, part table %d, bone lookup %d; parts %d %d %d %d)",
				windowNatives ? "checked: Minecraft hits break car windows" : "NOT as expected: Minecraft hits never break car windows", handlerOk, tableOk,
				boneOk, windowPart[0], windowPart[1], windowPart[2], windowPart[3]);
			return windowNatives == 1;
		}

		// The SEH part on its own (no C++ objects to unwind): calls the virtual SMASH_CAR_WINDOW will
		// call, then the native. False if anything faulted.
		bool SmashGuarded(CVehicle* a_veh, int a_handle, int a_window, bool& a_noFrag)
		{
			__try {
				using FragFn = void*(__thiscall*)(CVehicle*);
				void* frag = reinterpret_cast<FragFn>((*reinterpret_cast<void***>(a_veh))[0xA0 / 4])(a_veh);
				a_noFrag = frag == nullptr;
				if (!a_noFrag) {
					S::SMASH_CAR_WINDOW(a_handle, a_window);
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Breaks side window a_window (0 to 3) of a_handle if that is safe. Returns what happened, for the log.
		const char* SmashWindow(int a_handle, int a_window)
		{
			if (a_window < kWindowLF || a_window > kWindowRR) {
				return " (no native breaks a windscreen)";
			}
			if (!WindowNativesVerified() || windowFaulted) {
				return " (windows are left alone in this game)";
			}
			CVehicle* veh = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(a_handle)) : nullptr;
			if (!veh || !veh->m_pMatrix || !S::DOES_VEHICLE_EXIST(a_handle) || S::IS_CAR_DEAD(a_handle) || veh->m_fEngineHealth < 0.0f) {
				return " (a wreck: left alone)";
			}
			if (!Settled(a_handle)) {
				return " (a vehicle that just appeared: left alone)";
			}
			const int model = veh->m_nModelIndex;
			if (model < 0 || model >= 31000 || !CModelInfo::ms_modelInfoPtrs[model]) {
				return " (unknown model)";
			}
			const auto* info = reinterpret_cast<const std::uint8_t*>(CModelInfo::ms_modelInfoPtrs[model]);
			const int*  bones = *reinterpret_cast<const int* const*>(info + 0xCC);
			const int   bone = bones ? bones[windowPart[a_window]] : -1;
			if (bone < 0) {
				return " (this model has no such window)";
			}
			bool noFrag = false;
			if (!SmashGuarded(veh, a_handle, a_window, noFrag)) {
				windowFaulted = true;
				LC_LOG("WARNING: SMASH_CAR_WINDOW(%d, %d) faulted (model %d, bone %d): car windows are left alone from now on", a_handle, a_window, model, bone);
				return " (faulted: windows off)";
			}
			if (noFrag) {
				return " (no fragment instance: left alone)";
			}
			++counters.windows;
			return " (smashed)";
		}

		// ---- Minecraft hit a vehicle ---------------------------------------------------------------------
		// The ped in seat a_seat (kSeatDriver, or a passenger seat), 0 if none (or the player). Nobody in
		// a vehicle that just appeared (its seats may not be filled in yet: the seat natives handed the
		// game a garbage ped and crashed it in a test).
		int Occupant(int a_veh, int a_seat, int a_player)
		{
			if (!Settled(a_veh)) {
				return 0;
			}
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
			// The window natives only know the four side windows (given 4 or 5 they read past their table):
			// a windscreen keeps its glass; whoever is behind it is hit all the same.
			const char* glass = strike.window != kWindowNone ? SmashWindow(veh, strike.window) : "";
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
						AfterAttack(occupant, type, killed, a_frame, strike.seat == kSeatDriver ? veh : 0, CrimeForAttack(a_ev.flags, a_ev.weapon, type == kPedTypeCop));
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
					// (Its car took the hit: a police car's is an attack on the police, anyone else's damage to property.)
					AfterAttack(driver, type, false, a_frame, veh, type == kPedTypeCop ? kCrimeHitCop : kCrimeDamageToProperty);
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
				float x = 0, y = 0, z = 0;
				if (S::DOES_CHAR_EXIST(it->ped) && PedRoot(it->ped, x, y, z)) {
					const float mx = x - it->x, my = y - it->y;
					const float moved = std::sqrt(mx * mx + my * my);
					// Which way it went against where Minecraft pushed (0 = straight along the push).
					const float angle = AngleBetween(mx, my, it->dirX, it->dirY);
					const float along = moved > 1e-4f ? (mx * it->dirX + my * it->dirY) : 0.0f;
					static constexpr const char* kVariants[] = { "world", "ped frame", "old flags", "no force" };
					LC_LOG("knockback: %s %d moved %.2f m in %.1f s, %.2f m along the push (%.0f deg off it; push GTA %.2f %.2f, ped heading %.0f, %s force %.1f, ragdoll %d)",
						S::IS_CHAR_DEAD(it->ped) ? "corpse" : "ped", it->ped, moved, kKnockbackCheckSeconds, along, angle, it->dirX, it->dirY, it->heading,
						kVariants[it->variant & 3], it->force, S::IS_PED_RAGDOLL(it->ped));
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
		void FireworkBlast(const proto::McEvent& a_ev, const Frame& a_frame);

		void Explode(const proto::McEvent& a_ev, const Frame& a_frame)
		{
			if (a_ev.flags & proto::kExplosionFirework) {
				FireworkBlast(a_ev, a_frame);
				return;
			}
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
			// (Only a blast near the player is his doing as far as GTA's police can tell.)
			if (Cfg().gtaCrimes && a_frame.exists && dist < 40.0f && ReportGtaCrime(kCrimeCauseExplosion, nullptr)) {
				++counters.crimes;
				LC_LOG("crime: Minecraft's explosion -> GTA IV's report CAUSE_EXPLOSION");
			}
		}

		// ---- firework rockets: Minecraft's crossbow RPG (proto::kExplosionFirework) -------------------------
		// A rocket with stars bursts as GTA's rocket blast (FireworkExplosionType, 2: what an RPG's rocket
		// makes), as wide as its stars make it (the event's radius, FireworkBlast.java: 4 m for one star up
		// to the full 8 m), at full RPG damage within that. Minecraft's own firework damage never reaches
		// the stand-ins (HostActorEntity.hurtServer), so peds and vehicles are hurt once, by GTA's blast.
		// A rocket that struck a vehicle itself also does the rest of an RPG's hit to it once the blast has
		// had its frames (FireworkStrikes, CombatMath.h FireworkHitDamage): helicopters come down.
		constexpr float kFireworkSettleSeconds = 0.25f;  // GTA's blast has reached the struck vehicle by then
		struct FireworkStrike
		{
			int      veh;
			unsigned stars;
			float    age;
			unsigned bodyBefore;
			float    engineBefore;
			bool     heli;
		};
		std::vector<FireworkStrike> fireworkStrikes;

		void FireworkBlast(const proto::McEvent& a_ev, const Frame& a_frame)
		{
			const GtaVec c = McToGta(a_ev.a, a_ev.b, a_ev.c);
			const float  radius = ExplosionRadius(a_ev.d, Cfg().explosionRadiusScale);
			if (radius <= 0.0f) {
				return;
			}
			const int             type = std::clamp(Cfg().fireworkExplosionType, 0, 24);
			const tExplosionInfo* info = CExplosion::ms_ExplosionInfo;
			const float           endRadius = info ? info[type].m_fEndRadius : 0.0f;
			const float           size = ExplosionSizeScale(radius, endRadius);
			float                 px = 0, py = 0, pz = 0;
			if (a_frame.exists) {
				S::GET_CHAR_COORDINATES(a_frame.ped, &px, &py, &pz);
			}
			const float dist = a_frame.exists ? static_cast<float>(std::sqrt((c.x - px) * (c.x - px) + (c.y - py) * (c.y - py) + (c.z - pz) * (c.z - pz))) : 1e9f;
			if (owned.engaged && dist < radius * 3.0f + 5.0f) {
				// Minecraft hurt its player already (its own firework damage); GTA's blast mustn't too.
				blastProof = kBlastProofSeconds;
				SetBlastProof(a_frame.ped, true);
			}
			// The vehicle the rocket struck, as it was before the blast.
			std::uint32_t handle = 0, piece = 0;
			int           struck = 0;
			if (a_ev.formId && VehicleFromActorId(a_ev.formId, handle, piece) && handle && S::DOES_VEHICLE_EXIST(static_cast<int>(handle)) &&
				!S::IS_CAR_DEAD(static_cast<int>(handle))) {
				struck = static_cast<int>(handle);
			}
			CVehicle* car = struck && CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(handle) : nullptr;
			unsigned  body = 0;
			if (struck) {
				S::GET_CAR_HEALTH(struck, &body);
			}
			const float shake = ExplosionShake(radius, dist);
			S::ADD_EXPLOSION(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), type, size, true, false, shake);
			++counters.explosions;
			char what[96] = "";
			if (struck) {
				unsigned model = 0;
				S::GET_CAR_MODEL(struck, &model);
				std::snprintf(what, sizeof(what), ", struck vehicle %d (%s)", struck, S::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model));
			} else if (a_ev.formId) {
				std::snprintf(what, sizeof(what), ", struck actor %08X", a_ev.formId);
			}
			LC_LOG("Minecraft firework (%u star(s), radius %.1f blocks) -> ADD_EXPLOSION type %d, %.1f m (size %.2f of its %.1f m) at GTA %.1f %.1f %.1f, %.1f m "
				   "from the player, shake %.2f%s%s%s",
				a_ev.weapon, a_ev.d, type, radius, size, endRadius, c.x, c.y, c.z, dist, shake, what, (a_ev.flags & proto::kExplosionByPlayer) ? ", the player's" : "",
				proofSet ? " (player explosion-proof)" : "");
			if (struck && fireworkStrikes.size() < 8) {
				fireworkStrikes.push_back({ struck, a_ev.weapon, 0.0f, body, car ? car->m_fEngineHealth : 1000.0f, car && car->m_nVehicleType == VEHICLE_TYPE_HELI });
			}
			// The player's own rocket is his doing as far as it flies; anyone else's only near him.
			const float crimeRange = (a_ev.flags & proto::kExplosionByPlayer) ? kAircraftRange : 40.0f;
			if (Cfg().gtaCrimes && a_frame.exists && dist < crimeRange && ReportGtaCrime(kCrimeCauseExplosion, nullptr)) {
				++counters.crimes;
				LC_LOG("crime: Minecraft's firework -> GTA IV's report CAUSE_EXPLOSION");
			}
		}

		void FireworkStrikes(const Frame& a_frame)
		{
			for (auto it = fireworkStrikes.begin(); it != fireworkStrikes.end();) {
				FireworkStrike& f = *it;
				if ((f.age += a_frame.dt) < kFireworkSettleSeconds) {
					++it;
					continue;
				}
				const FireworkStrike s = f;
				it = fireworkStrikes.erase(it);
				if (!S::DOES_VEHICLE_EXIST(s.veh)) {
					LC_LOG("firework strike: vehicle %d is gone", s.veh);
					continue;
				}
				CVehicle* car = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(s.veh)) : nullptr;
				unsigned  body = 0;
				S::GET_CAR_HEALTH(s.veh, &body);
				const float engine = car ? car->m_fEngineHealth : 0.0f;
				const bool  deadAlready = S::IS_CAR_DEAD(s.veh);
				const float damage = FireworkHitDamage(s.stars);
				const char* what = "wrecked by GTA's blast already";
				if (!deadAlready && damage > 0.0f) {
					const float engineAfter = engine - damage;
					S::SET_CAR_HEALTH(s.veh, static_cast<unsigned>(std::max(1.0f, static_cast<float>(body) - damage)));
					if (engineAfter < 0.0f) {
						// An RPG's rocket blows up what it hits: at once, not after GTA's few seconds of fire.
						S::EXPLODE_CAR(s.veh, true, false);
						what = "blown up";
						++counters.wrecked;
					} else {
						S::SET_ENGINE_HEALTH(s.veh, engineAfter);
						what = "damaged";
					}
				}
				unsigned bodyNow = 0;
				S::GET_CAR_HEALTH(s.veh, &bodyNow);
				// Whoever flies or drives it: an attack on the police for a police vehicle, else damage to property.
				if (Cfg().gtaCrimes) {
					if (const int driver = Occupant(s.veh, kSeatDriver, a_frame.ped)) {
						unsigned type = 0;
						S::GET_PED_TYPE(driver, &type);
						AfterAttack(driver, type, false, a_frame, s.veh,
							type == kPedTypeCop ? kCrimeHitCop : (S::IS_CAR_DEAD(s.veh) ? kCrimeDestroyVehicle : kCrimeDamageToProperty));
					}
				}
				LC_LOG("firework struck %s %d: body %u engine %.0f before; after GTA's blast body %u engine %.0f%s; the rocket's own hit (%u star(s), %.0f) -> %s: "
					   "body %u engine %.0f, %s",
					s.heli ? "helicopter" : "vehicle", s.veh, s.bodyBefore, s.engineBefore, body, engine, deadAlready ? " (wrecked)" : "", s.stars, damage, what,
					bodyNow, car ? car->m_fEngineHealth : 0.0f, S::IS_CAR_DEAD(s.veh) ? "wrecked" : "still going");
			}
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

		// ---- Minecraft's shield (proto::kMcBlocking) --------------------------------------------------------
		// A hit Minecraft's raised shield blocks must not sound or look like one on Niko either. GTA IV
		// 1.0.8.0 starts a ped's pain voice in 0x83AB60 (audSpeechAudioEntity, the ped's at +0x580; one
		// pain description on the stack, ret 4), right after the damage that caused it is recorded on
		// the ped (its last damage entity). Its first instruction is a jump to a stub of ours, which
		// asks MutePainHook: while the shield is up, a hit from within its front arc (the same test
		// Minecraft makes: ShieldCovers) plays no pain; one from behind, or from nobody known (Minecraft
		// can't block that either), plays it as usual. Reaction animations are off while the shield is
		// up (ALLOW_REACTION_ANIMS; the puppeted Niko doesn't show them anyway). The damage itself still
		// happens (and goes to Minecraft, which blocks it): proofs would hide the hit from us as well.
		struct Shield
		{
			bool        up = false;
			int         ped = 0;
			const CPed* pedPtr = nullptr;
			float       lookYaw = 0.0f;
			float       upFor = 0.0f, logTimer = 0.0f;
			std::uint32_t muted = 0, sounded = 0, unknown = 0, painFrames = 0;
		} shield;
		int painHook = -1;  // -1 not tried, 0 not hooked, 1 hooked

		bool __cdecl MutePainHook(const void* a_speech)
		{
			const CPed* p = shield.pedPtr;
			if (!shield.up || !p || a_speech != reinterpret_cast<const char*>(p) + 0x580) {
				return false;
			}
			const CEntity* from = p->m_pLastDamageEntity;
			if (!from || from == p || !from->m_pMatrix || !p->m_pMatrix) {
				++shield.unknown;
				return false;
			}
			const int  yaw = HurtSourceYaw(HurtDirectionFlags(from->m_pMatrix->pos.x - p->m_pMatrix->pos.x, from->m_pMatrix->pos.y - p->m_pMatrix->pos.y));
			const bool covered = yaw >= 0 && ShieldCovers(shield.lookYaw, static_cast<float>(yaw));
			++(covered ? shield.muted : shield.sounded);
			if (Cfg().diagnostics) {
				LC_LOG("shield: a hit from MC yaw %d, the player looks at %.0f: pain %s", yaw, shield.lookYaw, covered ? "muted (Minecraft blocks it)" : "plays (behind the shield)");
			}
			return covered;
		}

		void HookPain()
		{
			if (painHook >= 0) {
				return;
			}
			painHook = 0;
			if (plugin::gameVer != plugin::VERSION_1080) {
				LC_LOG("shield: not GTA IV 1.0.8.0: blocked hits keep Niko's pain voice");
				return;
			}
			auto* at = reinterpret_cast<std::uint8_t*>(AddressSetter::gBaseAddress) + (0x83AB60 - 0x400000);
			// push ebp; mov ebp, [esp+8]; cmp byte [ebp+6], 0
			static constexpr std::uint8_t kExpect[] = { 0x55, 0x8B, 0x6C, 0x24, 0x08, 0x80, 0x7D, 0x06, 0x00 };
			if (std::memcmp(at, kExpect, sizeof(kExpect)) != 0) {
				LC_LOG("shield: the pain voice's code isn't as expected: blocked hits keep Niko's pain voice");
				return;
			}
			auto* stub = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!stub) {
				LC_LOG("shield: can't allocate the pain stub (error %lu)", ::GetLastError());
				return;
			}
			const auto abs32 = [](const void* a_p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_p)); };
			// push ecx; push ecx; call MutePainHook; add esp, 4; pop ecx; test al, al; jz real; ret 4;
			// real: push ebp; mov ebp, [esp+8]; jmp back (after the 5 bytes the jump replaced)
			std::uint8_t code[] = { 0x51, 0x51, 0xE8, 0, 0, 0, 0, 0x83, 0xC4, 0x04, 0x59, 0x84, 0xC0, 0x74, 0x03, 0xC2, 0x04, 0x00, 0x55, 0x8B, 0x6C, 0x24, 0x08,
				0xE9, 0, 0, 0, 0 };
			const std::uint32_t toHook = abs32(reinterpret_cast<const void*>(&MutePainHook)) - (abs32(stub) + 7u);
			const std::uint32_t back = abs32(at + 5) - (abs32(stub) + 28u);
			std::memcpy(code + 3, &toHook, 4);
			std::memcpy(code + 24, &back, 4);
			std::memcpy(stub, code, sizeof(code));
			::FlushInstructionCache(::GetCurrentProcess(), stub, sizeof(code));
			std::uint8_t jump[5] = { 0xE9 };
			const std::uint32_t toStub = abs32(stub) - (abs32(at) + 5u);
			std::memcpy(jump + 1, &toStub, 4);
			DWORD old = 0;
			if (!::VirtualProtect(at, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) {
				LC_LOG("shield: can't hook the pain voice (VirtualProtect error %lu)", ::GetLastError());
				return;
			}
			std::memcpy(at, jump, sizeof(jump));
			::VirtualProtect(at, sizeof(jump), old, &old);
			::FlushInstructionCache(::GetCurrentProcess(), at, sizeof(jump));
			painHook = 1;
			LC_LOG("shield: hits Minecraft's raised shield blocks leave Niko quiet (pain voice hooked)");
		}

		void SetShield(bool a_up, int a_ped)
		{
			if (a_up == shield.up) {
				return;
			}
			if (shield.ped && S::DOES_CHAR_EXIST(shield.ped)) {
				S::ALLOW_REACTION_ANIMS(shield.ped, !a_up);
			}
			if (a_up) {
				HookPain();
				if (a_ped && S::DOES_CHAR_EXIST(a_ped)) {
					S::ALLOW_REACTION_ANIMS(a_ped, false);
				}
				shield = Shield{};
				shield.up = true;
				shield.ped = a_ped;
			} else {
				if (Cfg().diagnostics || shield.muted + shield.sounded + shield.unknown > 0) {
					LC_LOG("shield down after %.1f s: %u hit%s from in front kept quiet, %u from behind and %u from nobody known sounded; pain voice playing %u frames",
						shield.upFor, shield.muted, shield.muted == 1 ? "" : "s", shield.sounded, shield.unknown, shield.painFrames);
				}
				shield.up = false;
				shield.pedPtr = nullptr;
				shield.ped = 0;
			}
		}

		// Every frame: the shield is up while Minecraft says so and its player is Niko (puppeted, his
		// health ours).
		void UpdateShield(const Frame& a_frame)
		{
			const bool up = owned.engaged && a_frame.puppeting && a_frame.mc && (a_frame.mc->flags & proto::kMcBlocking) != 0 && !a_frame.dead;
			SetShield(up, up ? a_frame.ped : 0);
			if (!shield.up) {
				return;
			}
			shield.pedPtr = FindPlayerPed();
			shield.lookYaw = std::fmod(std::fmod(a_frame.mc->yaw, 360.0f) + 360.0f, 360.0f);
			shield.upFor += a_frame.dt;
			shield.painFrames += S::IS_PAIN_PLAYING(a_frame.ped) ? 1u : 0u;
			if (Cfg().diagnostics && (shield.logTimer += a_frame.dt) >= 5.0f) {
				shield.logTimer = 0.0f;
				LC_LOG("shield up %.0f s (looking at MC yaw %.0f): %u hits kept quiet, %u sounded, %u from nobody known; pain voice playing %u frames", shield.upFor,
					shield.lookYaw, shield.muted, shield.sounded, shield.unknown, shield.painFrames);
			}
		}

		// ---- GTA's HUD health arc --------------------------------------------------------------------------
		// GTA IV 1.0.8.0's radar ring is health then armour: 0x8705B0 draws the health arc for
		// (health - 100) / max(max health - 100, 100) (blinking red at a quarter or less) and a red flash
		// for what was just lost, which 0x86B000 works out by comparing the health with the last one;
		// the armour arc (0x86C740) starts where the health arc ends, so it reads the health and the max
		// health too. The puppeted player's real health stays at the buffer, so these five places are
		// pointed at values of ours: the three health reads (`mov ecx, ped; call [vtable+0xFC]`) call a
		// stub that gives hudHealth for hudPed (anyone else: the real call), and the two max health reads
		// (`movss xmmN, [ped+0xA94]`) read hudMaxHealth. With the max at 200 and the health at
		// 100 + 100 x Minecraft's fraction, GTA's own HUD code shows Minecraft's health as it would its
		// own (the flash, the blink, the armour arc's place). Every site's bytes are checked first;
		// anything unexpected and nothing is patched (the arcs stay GTA's).
		// (Read by GTA's HUD code on its own thread: volatile, each a single aligned 32-bit store.)
		volatile float       hudMaxHealth = 200.0f;
		volatile float       hudHealth = 200.0f;
		const CPed* volatile hudPed = nullptr;  // the ped whose health the HUD is shown as hudHealth (none: GTA's)
		int                  hudPatch = -1;  // -1 not tried, 0 not patched, 1 patched

		struct HudSite
		{
			std::uint32_t va;           // GTA IV 1.0.8.0 address
			std::uint8_t  expect[16];   // the bytes there
			std::size_t   size;
			int           kind;         // 0: movss xmmN, [reg+0xA94] -> [hudMaxHealth]; 1: mov ecx, reg; call stub
			std::uint8_t  regByte;      // kind 0: the ModRM for [disp32] with that xmm; kind 1: the `mov ecx, reg` ModRM
			std::size_t   keepFrom;     // kind 1: these bytes of the site are kept, after `mov ecx, reg` and before the call
			std::size_t   keepSize;
		};

		void PatchHudHealth()
		{
			if (hudPatch >= 0) {
				return;
			}
			hudPatch = 0;
			if (plugin::gameVer != plugin::VERSION_1080) {
				LC_LOG("HUD: not GTA IV 1.0.8.0: the health arc stays GTA's");
				return;
			}
			static constexpr HudSite kSites[] = {
				// 0x8705B0 (the arc): mov edx, [ebp]; mov eax, [edx+0xFC]; mov ecx, ebp; call eax
				{ 0x87068E, { 0x8B, 0x55, 0x00, 0x8B, 0x82, 0xFC, 0x00, 0x00, 0x00, 0x8B, 0xCD, 0xFF, 0xD0 }, 13, 1, 0xCD, 0, 0 },
				// 0x8705B0: movss xmm1, [ebp+0xA94]; subss xmm1, xmm2
				{ 0x8706C0, { 0xF3, 0x0F, 0x10, 0x8D, 0x94, 0x0A, 0x00, 0x00, 0xF3, 0x0F, 0x5C, 0xCA }, 12, 0, 0x0D, 0, 0 },
				// 0x86B000 (what was lost): mov eax, [esi]; mov edx, [eax+0xFC]; mov ecx, esi; call edx
				{ 0x86B019, { 0x8B, 0x06, 0x8B, 0x90, 0xFC, 0x00, 0x00, 0x00, 0x8B, 0xCE, 0xFF, 0xD2 }, 12, 1, 0xCE, 0, 0 },
				// 0x86C740 (the armour arc's start): mov edx, [eax+0xFC]; mov ecx, edi; movss [esp+0x14], xmm0; call edx
				{ 0x86C869, { 0x8B, 0x90, 0xFC, 0x00, 0x00, 0x00, 0x8B, 0xCF, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x14, 0xFF, 0xD2 }, 16, 1, 0xCF, 8, 6 },
				// 0x86C740: movss xmm2, [edi+0xA94]; subss xmm2, xmm1
				{ 0x86C8A5, { 0xF3, 0x0F, 0x10, 0x97, 0x94, 0x0A, 0x00, 0x00, 0xF3, 0x0F, 0x5C, 0xD1 }, 12, 0, 0x15, 0, 0 },
			};
			const auto at = [](std::uint32_t a_va) { return reinterpret_cast<std::uint8_t*>(AddressSetter::gBaseAddress) + (a_va - 0x400000); };
			for (const HudSite& site : kSites) {
				if (std::memcmp(at(site.va), site.expect, site.size) != 0) {
					LC_LOG("HUD: the code at %08X isn't as expected: the health arc stays GTA's", site.va);
					return;
				}
			}
			// cmp ecx, [hudPed]; jne real; fld dword [hudHealth]; ret; real: mov eax, [ecx]; jmp [eax+0xFC]
			auto* stub = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!stub) {
				LC_LOG("HUD: can't allocate the health stub (error %lu): the health arc stays GTA's", ::GetLastError());
				return;
			}
			const auto abs32 = [](const volatile void* a_p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_p)); };
			std::uint8_t code[] = { 0x3B, 0x0D, 0, 0, 0, 0, 0x75, 0x07, 0xD9, 0x05, 0, 0, 0, 0, 0xC3, 0x8B, 0x01, 0xFF, 0xA0, 0xFC, 0x00, 0x00, 0x00 };
			const std::uint32_t pedAddr = abs32(&hudPed), healthAddr = abs32(&hudHealth), maxAddr = abs32(&hudMaxHealth);
			std::memcpy(code + 2, &pedAddr, 4);
			std::memcpy(code + 10, &healthAddr, 4);
			std::memcpy(stub, code, sizeof(code));
			::FlushInstructionCache(::GetCurrentProcess(), stub, sizeof(code));
			for (const HudSite& site : kSites) {
				std::uint8_t* p = at(site.va);
				std::uint8_t  patch[16];
				std::memset(patch, 0x90, sizeof(patch));
				if (site.kind == 0) {
					// movss xmmN, [hudMaxHealth]; the subss after it stays
					patch[0] = 0xF3, patch[1] = 0x0F, patch[2] = 0x10, patch[3] = site.regByte;
					std::memcpy(patch + 4, &maxAddr, 4);
					std::memcpy(patch + 8, site.expect + 8, site.size - 8);
				} else {
					// mov ecx, reg; [what the site keeps]; call stub; nops
					patch[0] = 0x8B, patch[1] = site.regByte;
					std::memcpy(patch + 2, site.expect + site.keepFrom, site.keepSize);
					const std::size_t   callAt = 2 + site.keepSize;
					const std::uint32_t rel = abs32(stub) - (abs32(p) + static_cast<std::uint32_t>(callAt) + 5u);
					patch[callAt] = 0xE8;
					std::memcpy(patch + callAt + 1, &rel, 4);
				}
				DWORD old = 0;
				if (!::VirtualProtect(p, site.size, PAGE_EXECUTE_READWRITE, &old)) {
					LC_LOG("HUD: can't patch %08X (VirtualProtect error %lu)", site.va, ::GetLastError());
					continue;
				}
				std::memcpy(p, patch, site.size);
				::VirtualProtect(p, site.size, old, &old);
				::FlushInstructionCache(::GetCurrentProcess(), p, site.size);
			}
			hudPatch = 1;
			LC_LOG("HUD: the health and armour arcs read the player's health from LibertyCraft (Minecraft's in Minecraft mode)");
		}

		// Every frame: what the HUD is shown.
		void UpdateHudHealth(const Frame& a_frame)
		{
			PatchHudHealth();
			CPed* p = a_frame.exists ? FindPlayerPed() : nullptr;
			if (owned.engaged && owned.mirroring && p) {
				hudHealth = owned.hudHealth;
				hudMaxHealth = 200.0f;
				hudPed = p;
			} else {
				hudPed = nullptr;
				if (p) {
					hudMaxHealth = p->m_fMaxHealth;
				}
			}
		}

		// GTA's HUD shows the puppeted player Minecraft's health and armour. The real health stays at the
		// buffer (a GTA hit never kills: it goes to Minecraft); the HUD is shown Minecraft's health
		// instead (CombatMath.h HudHealth; PatchHudHealth, UpdateHudHealth). The armour is Minecraft's
		// armour, scaled, for real (GTA's damage takes it first, and the refill puts it back: it counts as
		// damage like health). Without Minecraft's vitals (an older mod, the test stand-in) the HUD shows
		// the buffer: full arcs.
		void MirrorVitals(const Frame& a_frame, unsigned& a_targetArmour)
		{
			const McVitals v = a_frame.mc ? DecodeVitals(a_frame.mc->pad4C, a_frame.mc->tickPad) : McVitals{};
			a_targetArmour = owned.baseArmour;
			if (!v.valid) {
				owned.mirroring = false;
				return;
			}
			owned.hudHealth = HudHealth(v.maxHealth > 0.0f ? v.health / v.maxHealth : 1.0f);
			unsigned maxArmour = 0;
			S::GET_PLAYER_MAX_ARMOUR(owned.player, &maxArmour);
			a_targetArmour = static_cast<unsigned>(MirroredArmour(v.armour, static_cast<int>(maxArmour ? maxArmour : 100u)));
			if (!owned.mirroring) {
				owned.mirroring = true;
				LC_LOG("GTA's HUD shows Minecraft's vitals: health %.1f of %.1f -> %.1f of 200 (real health held at %u), armour %d -> %u of %u", v.health,
					v.maxHealth, owned.hudHealth, owned.base, v.armour, a_targetArmour, maxArmour);
			}
			if (Cfg().diagnostics) {
				LC_LOG_EVERY(2000, "HUD: Minecraft health %.1f of %.1f, armour %d -> the HUD's health %.1f of 200 (real %u, %s), armour %u", v.health,
					v.maxHealth, v.armour, owned.hudHealth, owned.base, hudPatch == 1 ? "patched" : "NOT patched", a_targetArmour);
			}
		}

		// The GTA weapon that last hurt a_ped. CPhysical's last-damage-weapon field holds garbage on
		// 1.0.8.0 (0xCDCDCDCD in game), so when it isn't a weapon the game is asked, weapon by weapon.
		int LastDamageWeapon(int a_ped, const CPed* a_p)
		{
			const int field = a_p ? a_p->m_nLastDamageWeapon : -1;
			if (field >= 0 && field <= kWeaponAnyMelee) {
				return field;
			}
			static constexpr int kProbe[] = { kWeaponExplosion, kWeaponRammedByCar, kWeaponRunOverByCar, kWeaponFall, kWeaponDrowning, kWeaponAnyMelee,
				kWeaponPistol, 9, 10, 11, 12, 13, 14, 15, 16, kWeaponSniperM40A1, kWeaponRocketLauncher, kWeaponFlameThrower, kWeaponMinigun,
				kWeaponUziDriveby, kWeaponGrenade, kWeaponMolotov, kWeaponRocket, kWeaponUnarmed, kWeaponBat, kWeaponPoolCue, kWeaponKnife };
			for (const int w : kProbe) {
				if (S::HAS_CHAR_BEEN_DAMAGED_BY_WEAPON(a_ped, static_cast<unsigned>(w))) {
					return w;
				}
			}
			return -1;
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
				if (armour != owned.savedArmour) {
					// (ADD_ARMOUR_TO_CHAR adds a signed amount and clamps: it also takes armour away.)
					S::ADD_ARMOUR_TO_CHAR(ped, static_cast<unsigned>(static_cast<int>(owned.savedArmour) - static_cast<int>(armour)));
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
				const int     weapon = LastDamageWeapon(a_frame.ped, p);
				const auto    attacker = p ? HandleOfPedPointer(p->m_pLastDamageEntity) : 0u;
				const auto    cls = ClassifyWeapon(weapon);
				// Which way it came from (any entity: a ped, a car, whoever threw the grenade), for
				// Minecraft's shield (proto::kHurtHasDirection).
				std::uint32_t  hurtFlags = 0;
				const CEntity* from = p ? p->m_pLastDamageEntity : nullptr;
				if (from && from != p && from->m_pMatrix && p->m_pMatrix) {
					hurtFlags = HurtDirectionFlags(from->m_pMatrix->pos.x - p->m_pMatrix->pos.x, from->m_pMatrix->pos.y - p->m_pMatrix->pos.y);
				}
				++counters.hurtFrames;
				if (cls == HurtClass::kProjectile) {
					++wantedTest.gunHits;  // (DebugWanted)
				}
				if (blastProof > 0.0f && (weapon == kWeaponExplosion || cls == HurtClass::kOther)) {
					++counters.hurtDroppedBlast;  // our own (Minecraft's) explosion: Minecraft hurt its player itself
				} else if (cls == HurtClass::kIgnore) {
					++counters.hurtDroppedIgnored;
				} else {
					pacer.Add(HurtKindOf(cls), deficit, attacker ? ActorIdFromHandle(attacker) : 0u, hurtFlags);
					NoteAttacker(attacker);
				}
				if (Cfg().diagnostics) {
					LC_LOG("player lost %.0f (health %u/%u armour %u/%u), weapon %d, attacker %s%08X%s", deficit, health, owned.base, armour, owned.lastArmour,
						weapon, attacker ? "ped " : "", attacker, (hurtFlags & proto::kHurtHasDirection) ? ", from a direction" : "");
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
			unsigned targetArmour = owned.baseArmour;
			MirrorVitals(a_frame, targetArmour);
			if (health != owned.base) {
				RaiseHealthCap();
				S::SET_CHAR_HEALTH(a_frame.ped, owned.base);
				S::GET_CHAR_HEALTH(a_frame.ped, &health);
			}
			if (armour != targetArmour) {
				// (A signed amount: it also takes armour away when Minecraft's armour comes off.)
				S::ADD_ARMOUR_TO_CHAR(a_frame.ped, static_cast<unsigned>(static_cast<int>(targetArmour) - static_cast<int>(armour)));
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
				Game::ReportHurt(static_cast<std::uint16_t>(batch.kind), HostDamageForMc(mcDamage), batch.attacker, batch.flags);
				++counters.hurtsSent;
				counters.hurtGtaDamage += batch.damage;
				LC_LOG("GTA IV hurt the player: %.0f GTA damage (%u hit%s, %s, attacker %08X, from yaw %d) -> %.2f Minecraft damage", batch.damage, batch.hits,
					batch.hits == 1 ? "" : "s", batch.kind == proto::kHurtMelee ? "melee" : batch.kind == proto::kHurtProjectile ? "projectile" : "other",
					batch.attacker, (batch.flags & proto::kHurtHasDirection) ? static_cast<int>((batch.flags >> proto::kHurtDirectionShift) & 0x1FFu) : -1, mcDamage);
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
		// DebugWanted=N (not in the default ini): N s into play, 2 wanted stars; then for a minute, every 3
		// s, how interested the police are: the wanted level, cops within 80 m and how many of them are in
		// combat, the shots they fired in those 3 s (IS_CHAR_SHOOTING holds for the frame of a shot only, so
		// every frame is counted: QA's one sample every 3 s read "0 shooting" while they fired several
		// shots a second), and how often their gunfire hurt the player.
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
					w.logTimer = 3.0f;
					w.gunHitsLogged = w.gunHits;
					S::ALTER_WANTED_LEVEL(a_frame.player, 2);
					S::APPLY_WANTED_LEVEL_CHANGE_NOW(a_frame.player);
					LC_LOG("DebugWanted: 2 stars (player control %s, puppeting %d)", S::IS_PLAYER_CONTROL_ON(a_frame.player) ? "on" : "off", a_frame.puppeting);
				}
				return;
			}
			if ((w.since += a_frame.dt) > 60.0f) {
				return;
			}
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
			w.shots += shooting;
			w.shotFrames += shooting ? 1u : 0u;
			++w.frames;
			if ((w.logTimer -= a_frame.dt) > 0.0f) {
				return;
			}
			w.logTimer = 3.0f;
			unsigned wanted = 0, health = 0;
			S::STORE_WANTED_LEVEL(a_frame.player, &wanted);
			S::GET_CHAR_HEALTH(a_frame.ped, &health);
			LC_LOG("DebugWanted +%.0fs: wanted %u, %u cops within 80 m (nearest %u m), %u in combat; %u shots in the last 3 s (in %u of %u frames), the player "
				   "hit by gunfire %u times; player health %u, control %s, puppeting %d",
				w.since, wanted, cops, cops ? nearest : 0u, fighting, w.shots, w.shotFrames, w.frames, w.gunHits - w.gunHitsLogged, health,
				S::IS_PLAYER_CONTROL_ON(a_frame.player) ? "on" : "off", a_frame.puppeting);
			w.shots = w.shotFrames = w.frames = 0;
			w.gunHitsLogged = w.gunHits;
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
			// 12 Minecraft damage: 120 off the occupant behind the glass (lethal: 200 health, dead at 100),
			// 180 off the body and engine (VehicleDamageScale 15), so the car lasts all four windows (at
			// 30 the third or fourth arrow set it on fire and wrecked it).
			proto::McEvent ev{};
			ev.type = proto::kEvHitActor;
			ev.formId = hitPoint.formId;
			ev.a = 12.0f;
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
			const unsigned model = Cfg().debugTestCarModel.empty() ? 0u : S::GET_HASH_KEY(Cfg().debugTestCarModel.c_str());
			if (t.stage == 0 && (!model || !S::IS_MODEL_IN_CDIMAGE(model) || !S::IS_THIS_MODEL_A_VEHICLE(model))) {
				LC_LOG("DebugTestCar: \"%s\" isn't a vehicle model", Cfg().debugTestCarModel.c_str());
				t.stage = 4;
				return;
			}
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
				if (Cfg().debugCarCover) {
					// DebugCarCover: facing north, its left side 0.9 m east of the player and the middle of
					// its bonnet level with him; a ped stands just beyond its right side there, facing the
					// player across the bonnet (Minecraft's hits over the bonnet must reach him).
					float     lo[3]{ -1.0f, -2.5f, -0.6f }, hi[3]{ 1.0f, 2.5f, 0.9f };
					CVehicle* v = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(t.car)) : nullptr;
					if (v) {
						NpcBlocks::ModelBox(t.car, v->m_nModelIndex, lo, hi);
					}
					const float carX = x + 0.9f - lo[0], carY = y - (hi[1] - 0.7f);
					PlaceCar(t.car, carX, carY, z, 0.0f);
					S::FREEZE_CAR_POSITION(t.car, true);
					S::CREATE_RANDOM_CHAR(carX + hi[0] + 0.45f, y, z, &t.cover);
					if (t.cover) {
						float ground = z - 1.0f;
						S::GET_GROUND_Z_FOR_3D_COORD(carX + hi[0] + 0.45f, y, z + 1.0f, &ground);
						S::SET_CHAR_COORDINATES(t.cover, carX + hi[0] + 0.45f, y, ground);  // (feet on the ground)
						S::SET_CHAR_HEADING(t.cover, 90.0f);
						S::FREEZE_CHAR_POSITION(t.cover, true);
						S::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(t.cover, true);
						S::TASK_PAUSE(t.cover, 600000);
					}
					LC_LOG("DebugCarCover: car %d at GTA %.2f %.2f (model box x %+.2f..%+.2f, y %+.2f..%+.2f), ped %d at %.2f %.2f, player at %.2f %.2f %.2f",
						t.car, carX, carY, lo[0], hi[0], lo[1], hi[1], t.cover, carX + hi[0] + 0.45f, y, x, y, z);
					t.stage = 2;
					t.timer = -1e9f;  // (stays)
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
				// (No arrow while the car is brand new: Occupant and the window natives leave a vehicle
				// alone for its first kSettledFrames, so it would only scratch the body.)
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
				// (Not the windows: IS_VEH_WINDOW_INTACT reads a bone matrix unchecked and crashed the game.)
				std::snprintf(line, sizeof(line), "body %u; driver %u, passenger %u", body, dh, ph);
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

		// DebugFireworkTargets=N (not in the default ini): N s into puppet mode, targets for crossbow
		// fireworks in the direction the player looks (fired by an autorun: config/libertycraft-autorun.txt,
		// the rockets summoned with their motion toward the spots logged here in Minecraft's coordinates):
		// a police Maverick with a police pilot hovers DebugFireworkHeliAhead (40) m ahead and 18 m up (a
		// quarter of any extra distance higher; turned or lifted to where GTA's map leaves it room), side
		// on (held there until something hurts it: 100 health off, then it's GTA's), and 14 m ahead two
		// peds stand still
		// left of a spot and a parked Admiral right of it. What happens to them is logged as it changes (at
		// most every 0.25 s) for 60 s.
		struct FireworkTargets
		{
			int   stage = 0;  // 0 waiting, 1 models requested, 2 placed, 3 done
			float timer = 0.0f, since = 0.0f, logTimer = 0.0f;
			int   heli = 0, pilot = 0, car = 0, peds[2]{};
			float hover[3]{};
			float baseZ = 0.0f;
			bool  holding = false;
			char  last[240] = "";
		} fwTargets;

		void FireworkTargetsHook(const Frame& a_frame)
		{
			auto& t = fwTargets;
			if (Cfg().debugFireworkTargets <= 0 || t.stage >= 3 || !a_frame.exists || a_frame.loading || a_frame.dead || !a_frame.mcInWorld) {
				return;
			}
			const unsigned heliModel = S::GET_HASH_KEY("polmav"), copModel = S::GET_HASH_KEY("m_y_cop"), carModel = S::GET_HASH_KEY("admiral");
			if (t.stage == 0) {
				if (!a_frame.puppeting || (t.timer += a_frame.dt) < static_cast<float>(Cfg().debugFireworkTargets)) {
					return;
				}
				for (const unsigned m : { heliModel, copModel, carModel }) {
					CStreaming::ScriptRequestModel(static_cast<std::int32_t>(m));
				}
				t.stage = 1;
				return;
			}
			if (t.stage == 1) {
				if (!S::HAS_MODEL_LOADED(heliModel) || !S::HAS_MODEL_LOADED(copModel) || !S::HAS_MODEL_LOADED(carModel)) {
					return;
				}
				float x = 0, y = 0, z = 0, heading = 0;
				S::GET_CHAR_COORDINATES(a_frame.ped, &x, &y, &z);
				S::GET_CHAR_HEADING(a_frame.ped, &heading);
				if (a_frame.mc) {
					heading = McYawToGtaHeading(a_frame.mc->yaw);
				}
				const float r = heading * kDegToRad, fx = -std::sin(r), fy = std::cos(r), rx = std::cos(r), ry = std::sin(r);
				t.baseZ = z;
				// The helicopter: ahead of the player, turned up to 40 degrees either way and lifted up to 20 m
				// until GTA's map is clear from the player's eyes to it and round it (rotor span, below).
				const float ahead = std::clamp(Cfg().debugFireworkHeliAhead, 10.0f, 150.0f);
				const float up = 18.0f + std::max(0.0f, ahead - 40.0f) * 0.25f;  // (farther: over the roofs)
				auto        blocked = [](float a_x0, float a_y0, float a_z0, float a_x1, float a_y1, float a_z1) {
					const float         from[3] = { a_x0, a_y0, a_z0 }, to[3] = { a_x1, a_y1, a_z1 };
					tLineOfSightResults res;
					return col::CastGta(from, to, res, STATIC_COLLISION | BUILDINGS | OBJECTS);
				};
				float heliHeading = heading, lifted = 0.0f;
				bool  clear = false;
				for (const float turn : { 0.0f, -10.0f, 10.0f, -20.0f, 20.0f, -30.0f, 30.0f, -40.0f, 40.0f }) {
					for (const float lift : { 0.0f, 10.0f, 20.0f }) {
						const float hr = (heading + turn) * kDegToRad, hfx = -std::sin(hr), hfy = std::cos(hr);
						const float cx = x + hfx * ahead, cy = y + hfy * ahead, cz = z + up + lift;
						if (!blocked(x, y, z + 0.6f, cx, cy, cz) && !blocked(cx - hfy * 8.0f, cy + hfx * 8.0f, cz, cx + hfy * 8.0f, cy - hfx * 8.0f, cz) &&
							!blocked(cx, cy, cz + 3.0f, cx, cy, cz - 5.0f)) {
							heliHeading = heading + turn, lifted = lift, clear = true;
							t.hover[0] = cx, t.hover[1] = cy, t.hover[2] = cz;
							break;
						}
					}
					if (clear) {
						break;
					}
				}
				if (!clear) {
					t.hover[0] = x + fx * ahead, t.hover[1] = y + fy * ahead, t.hover[2] = z + up;
				}
				S::CREATE_CAR(heliModel, t.hover[0], t.hover[1], t.hover[2], &t.heli, true);
				if (t.heli) {
					S::SET_CAR_HEADING(t.heli, heliHeading + 90.0f);
					S::SET_CAR_ENGINE_ON(t.heli, true, true);
					S::SET_HELI_BLADES_FULL_SPEED(t.heli);
					S::CREATE_CHAR_INSIDE_CAR(t.heli, 6 /* PEDTYPE_COP */, copModel, &t.pilot);
					if (t.pilot) {
						S::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(t.pilot, true);
					}
				}
				const float sx = x + fx * 14.0f, sy = y + fy * 14.0f;  // the spot on the ground
				float       spotZ = z - 1.0f;
				S::GET_GROUND_Z_FOR_3D_COORD(sx, sy, z + 1.0f, &spotZ);
				S::CREATE_CAR(carModel, sx + rx * 3.5f, sy + ry * 3.5f, z, &t.car, true);
				if (t.car) {
					PlaceCar(t.car, sx + rx * 3.5f, sy + ry * 3.5f, z, heading + 90.0f);
				}
				for (int i = 0; i < 2; ++i) {
					const float side = 1.5f + 1.5f * static_cast<float>(i), ahead = 0.8f * static_cast<float>(i);
					const float px = sx - rx * side + fx * ahead, py = sy - ry * side + fy * ahead;
					S::CREATE_RANDOM_CHAR(px, py, z, &t.peds[i]);
					if (t.peds[i]) {
						float ground = z - 1.0f;
						S::GET_GROUND_Z_FOR_3D_COORD(px, py, z + 1.0f, &ground);
						S::SET_CHAR_COORDINATES(t.peds[i], px, py, ground);
						S::SET_CHAR_HEADING(t.peds[i], heading + 180.0f);  // facing the player
						S::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(t.peds[i], true);
						S::TASK_PAUSE(t.peds[i], 600000);
					}
				}
				for (const unsigned m : { heliModel, copModel, carModel }) {
					S::MARK_MODEL_AS_NO_LONGER_NEEDED(m);
				}
				t.holding = t.heli != 0;
				t.stage = 2;
				const McVec feet = GtaToMc(x, y, z - Cfg().rootToFeet), heliMc = GtaToMc(t.hover[0], t.hover[1], t.hover[2]), spotMc = GtaToMc(sx, sy, spotZ);
				float       pedAt[3] = { sx, sy, spotZ + 1.0f };  // the nearer ped's middle (its root)
				if (t.peds[0]) {
					S::GET_CHAR_COORDINATES(t.peds[0], &pedAt[0], &pedAt[1], &pedAt[2]);
				}
				const McVec pedMc = GtaToMc(pedAt[0], pedAt[1], pedAt[2]);
				LC_LOG("DebugFireworkTargets: player at GTA %.1f %.1f %.1f heading %.0f; police helicopter %d (pilot %d) %.0f m away at heading %.0f, %.0f m up%s; car "
					   "%d and peds %d %d round the spot 14 m ahead",
					x, y, z, heading, t.heli, t.pilot, ahead, heliHeading, up + lifted, clear ? "" : " (no clear spot found)", t.car, t.peds[0], t.peds[1]);
				LC_LOG("DebugFireworkTargets: aim (Minecraft): feet %.2f %.2f %.2f heli %.2f %.2f %.2f spot %.2f %.2f %.2f ped %.2f %.2f %.2f", feet.x, feet.y, feet.z,
					heliMc.x, heliMc.y, heliMc.z, spotMc.x, spotMc.y, spotMc.z, pedMc.x, pedMc.y, pedMc.z);
				return;
			}
			t.since += a_frame.dt;
			CVehicle* heli = t.heli && S::DOES_VEHICLE_EXIST(t.heli) && CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(t.heli)) : nullptr;
			unsigned  heliBody = 0;
			if (heli) {
				S::GET_CAR_HEALTH(t.heli, &heliBody);
			}
			if (t.holding) {
				if (!heli || !heli->m_pMatrix || S::IS_CAR_DEAD(t.heli) || heliBody < 900 || heli->m_fEngineHealth < 900.0f) {
					t.holding = false;
					LC_LOG("DebugFireworkTargets: the helicopter was hit: no longer held (body %u, engine %.0f)", heliBody, heli ? heli->m_fEngineHealth : 0.0f);
				} else {
					// Hover: a spring toward the spot, through the physics collider's velocity.
					const auto& p = heli->m_pMatrix->pos;
					CVector     v{ std::clamp((t.hover[0] - p.x) * 1.5f, -4.0f, 4.0f), std::clamp((t.hover[1] - p.y) * 1.5f, -4.0f, 4.0f),
							std::clamp((t.hover[2] - p.z) * 1.5f, -4.0f, 4.0f) };
					if (auto* c = heli->GetConstrainedCollider()) {
						c->SetVelocity(&v);
					}
					S::SET_HELI_BLADES_FULL_SPEED(t.heli);
				}
			}
			if ((t.logTimer -= a_frame.dt) <= 0.0f) {
				t.logTimer = 0.25f;
				unsigned carBody = 0, pilotHealth = 0, pedHealth[2]{};
				if (t.car && S::DOES_VEHICLE_EXIST(t.car)) {
					S::GET_CAR_HEALTH(t.car, &carBody);
				}
				if (t.pilot && S::DOES_CHAR_EXIST(t.pilot)) {
					S::GET_CHAR_HEALTH(t.pilot, &pilotHealth);
				}
				for (int i = 0; i < 2; ++i) {
					if (t.peds[i] && S::DOES_CHAR_EXIST(t.peds[i])) {
						S::GET_CHAR_HEALTH(t.peds[i], &pedHealth[i]);
					}
				}
				CVehicle* car = t.car && S::DOES_VEHICLE_EXIST(t.car) && CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(t.car)) : nullptr;
				char line[240];
				std::snprintf(line, sizeof(line), "helicopter body %u engine %.0f%s at %.0f m up%s, pilot %u; car body %u engine %.0f%s; peds %u%s %u%s", heliBody,
					heli ? heli->m_fEngineHealth : 0.0f, heli && S::IS_CAR_DEAD(t.heli) ? " WRECKED" : "",
					heli && heli->m_pMatrix ? std::floor(heli->m_pMatrix->pos.z - t.baseZ) : -1.0f, heli && S::IS_CAR_ON_FIRE(t.heli) ? " burning" : "",
					pilotHealth, carBody, car ? car->m_fEngineHealth : 0.0f, car && S::IS_CAR_DEAD(t.car) ? " WRECKED" : "", pedHealth[0],
					t.peds[0] && S::DOES_CHAR_EXIST(t.peds[0]) && S::IS_PED_RAGDOLL(t.peds[0]) ? " (ragdoll)" : "", pedHealth[1],
					t.peds[1] && S::DOES_CHAR_EXIST(t.peds[1]) && S::IS_PED_RAGDOLL(t.peds[1]) ? " (ragdoll)" : "");
				if (std::strcmp(line, t.last) != 0) {
					std::strcpy(t.last, line);
					LC_LOG("DebugFireworkTargets +%.2fs: %s", t.since, line);
				}
			}
			if (t.since > 60.0f) {
				t.stage = 3;
				LC_LOG("DebugFireworkTargets: over");
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
			BumpPedHook(a_frame);
			FireworkTargetsHook(a_frame);
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
				if (c.corpseHits || c.corpseBumps || c.bumpNudges || c.bumpStumbles || c.bumpKnockdowns) {
					LC_LOG("stats %.0fs: bodies: corpses hit %u, corpses pushed by the player %u; peds nudged %u frames, stumbled %u, knocked down %u", secs,
						c.corpseHits, c.corpseBumps, c.bumpNudges, c.bumpStumbles, c.bumpKnockdowns);
				}
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
		fwTargets = FireworkTargets{};
		seenVehicles.clear();
		shield = Shield{};  // (the ped is going away)
		pendingKills.clear();
		fireworkStrikes.clear();
		bumpCooldown.clear();
		speechCooldown.clear();
		corpseFlings.clear();
		ClearActorTable();
	}

	std::uint32_t Tick(const Frame& a_frame)
	{
		InstallFaultTrace();
		++frameNo;
		PruneVehicles();
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
			case proto::kEvBump:
				if (playable && a_frame.puppeting) {
					Bump(ev, a_frame);
				}
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
		UpdateShield(a_frame);
		UpdateHudHealth(a_frame);
		PendingKills(a_frame);
		FireworkStrikes(a_frame);
		CorpseFlings(a_frame.dt);
		TickBumps(a_frame.dt);
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
