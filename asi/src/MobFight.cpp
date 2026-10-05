// Unity-built into dllmain.cpp (needs IV-SDK). See MobFight.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "mobs"
#include "MobFight.h"

#include "Combat.h"
#include "Config.h"
#include "Coords.h"
#include "Link.h"
#include "Log.h"
#include "Missions.h"
#include "combat/CombatMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lc::MobFight
{
	namespace
	{
		namespace S = ::Scripting;

		constexpr float    kReactEvery = 0.25f;     // seconds between reaction passes
		constexpr float    kShootEvery = 1.8f;      // a shooter's order is renewed this often (s)
		constexpr float    kReaimAfter = 0.6f;      // ... or this soon when the mob moved kReaimMoved m from where it aims
		constexpr float    kReaimMoved = 1.2f;
		constexpr unsigned kShootMs = 2000;         // each shooting order lasts this long
		constexpr int      kShootPattern = 5;       // TASK_SHOOT_AT_COORD's last argument
		constexpr float    kFleeAgain = 3.0f;       // a running ped is told again at most this often (s)
		constexpr float    kCopRange = 25.0f;       // police this close to a mob that is after a ped join in
		constexpr int      kCopsPerMob = 3;
		constexpr float    kFleeFrom = 15.0f;       // unarmed peds run from a mob this close
		constexpr float    kFleeRange = 40.0f;
		constexpr unsigned kFleeMs = 10000;
		constexpr float    kHurtRange = 12.0f;      // a ped a mob hurt looks for it this far
		constexpr float    kShooterReach = 2.5f;    // a shot's muzzle is this close to who fired it (m)
		constexpr unsigned kPedTypeCop = 2;         // GET_PED_TYPE
		constexpr unsigned kWeaponPistol = 7;
		constexpr unsigned kDefaultDamage = 25;     // GTA damage when the weapon's isn't known

		const Config& Cfg() { return Config::Get(); }

		struct Mob
		{
			int   id = 0;
			float x = 0, y = 0, z0 = 0, z1 = 0, half = 0.3f;  // GTA: middle, feet, top, half width
			int   target = 0;                                // the ped it is after (script handle), 0: none
			bool  afterPlayer = false;
			bool  passive = false;  // not a monster (a golem, an animal): only bullets, unless it is after a ped
			float age = 0.0f;
		};
		std::mutex       mobLock;  // mobs and shots: the game thread and GTA's bullet trace
		std::vector<Mob> mobs;

		struct Shot
		{
			int   mob;
			float from[3], at[3];
		};
		std::vector<Shot> shots;

		struct PedState
		{
			float since = 99.0f;  // seconds since its last order
			float aim[3]{};       // where it was told to shoot
			int   mob = 0;        // the mob it deals with
			float age = 0.0f;
		};
		std::unordered_map<int, PedState> peds;
		float reactTimer = 0.0f, statsTimer = 0.0f;

		struct Counters
		{
			std::uint32_t events = 0, orders = 0, flees = 0, gunsGiven = 0, shotsAtMobs = 0, hitsSent = 0, blocked = 0;
		} counters;

		// DebugMobFight: a cop and a civilian standing still ahead of the player.
		struct FightTest
		{
			int   stage = 0;  // 0 waiting, 1 models requested, 2 standing, 3 done
			float timer = 0.0f, logTimer = 0.0f;
			int   cop = 0, civ = 0;
		} test;

		// DebugShore=N: N s into puppet mode, the player is put in the nearest water (GTA's water searched in 16
		// directions out to 600 m), 6 m past where it starts, at its surface. 4 s later, once GTA IV has the
		// collision there, it logs a map of the ground around (GET_GROUND_Z_FOR_3D_COORD from just above the
		// water and from high up, GET_WATER_HEIGHT) in Minecraft's coordinates, to pick test spots from: a
		// beach, shallow water over a probed sea bed, a pier.
		struct ShoreTest
		{
			int   stage = 0;
			float timer = 0.0f, water = 0.0f;
		} shore;

		void ShoreMap(float a_x, float a_y, float a_w)
		{
			constexpr int   kHalf = 25;   // cells each way
			constexpr float kStep = 2.0f; // m
			LC_LOG("DebugShore map around MC %.0f %.0f (water surface %.2f), 2 m cells, MC x across, MC z down. '~' water over ground 3+ m"
				   " down, '=' 1.5 to 3 m, '-' up to 1.5 m, 'b' ground 0 to 1.5 m above the water, '#' higher, '?' no ground; uppercase or '*': something more"
				   " than 2.5 m above that ground (a pier, a boardwalk)", a_x, -a_y, a_w);
			for (int j = -kHalf; j <= kHalf; ++j) {
				char row[2 * kHalf + 2] = {};
				for (int i = -kHalf; i <= kHalf; ++i) {
					const float gx = a_x + i * kStep, gy = a_y - j * kStep;  // MC z grows southward: GTA y shrinks
					float       low = -1.0e4f, top = -1.0e4f, h = 0.0f;
					S::GET_GROUND_Z_FOR_3D_COORD(gx, gy, a_w + 2.5f, &low);
					S::GET_GROUND_Z_FOR_3D_COORD(gx, gy, a_w + 60.0f, &top);
					const bool wet = S::GET_WATER_HEIGHT(gx, gy, a_w + 2.0f, &h) && std::fabs(h - a_w) < 1.0f;
					char       c = '?';
					if (std::fabs(low) > 1.0e-4f && low > -1.0e3f) {
						const float d = low - a_w;
						c = d > 1.5f ? '#' : d >= 0.0f ? 'b' : !wet ? 'b' : d < -3.0f ? '~' : d < -1.5f ? '=' : '-';
						if (top - low > 2.5f) {
							c = c == 'b' ? 'B' : c == '#' ? '*' : c == '~' ? 'W' : c == '=' ? 'E' : c == '-' ? 'S' : c;
						}
					}
					row[i + kHalf] = c;
				}
				LC_LOG("DebugShore map z %+4d: %s", static_cast<int>(std::lround(-a_y + j * kStep)), row);
			}
		}

		void ShoreHook(const Combat::Frame& a_frame)
		{
			if (Cfg().debugShore <= 0 || shore.stage >= 2 || !a_frame.exists || !a_frame.ped) {
				return;
			}
			if (shore.stage == 0 && !a_frame.puppeting) {
				return;
			}
			shore.timer += a_frame.dt;
			float x = 0, y = 0, z = 0;
			S::GET_CHAR_COORDINATES(a_frame.ped, &x, &y, &z);
			if (shore.stage == 1) {
				if (shore.timer >= 4.0f) {
					shore.stage = 2;
					ShoreMap(x, y, shore.water);
				}
				return;
			}
			if (shore.timer < static_cast<float>(Cfg().debugShore)) {
				return;
			}
			shore.stage = 1, shore.timer = 0.0f;
			float best = 1e9f, bx = 0, by = 0, w = 0;
			for (int k = 0; k < 16; ++k) {
				const float a = static_cast<float>(k) * (kPi / 8.0f), cx = std::cos(a), cy = std::sin(a);
				for (float d = 4.0f; d <= 600.0f && d < best; d += 2.0f) {
					float h = 0.0f;
					if (S::GET_WATER_HEIGHT(x + cx * d, y + cy * d, z + 2.0f, &h)) {
						best = d, bx = cx, by = cy, w = h;
						break;
					}
				}
			}
			if (best > 1e8f) {
				shore.stage = 2;
				LC_LOG("DebugShore: no water within 600 m");
				return;
			}
			shore.water = w;
			const float px = x + bx * (best + 6.0f), py = y + by * (best + 6.0f);
			const float heading = std::atan2(-bx, by) / kDegToRad;
			S::SET_CHAR_COORDINATES(a_frame.ped, px, py, w + 1.0f);
			S::SET_CHAR_HEADING(a_frame.ped, heading);
			const McVec at = GtaToMc(px, py, w), edge = GtaToMc(x + bx * best, y + by * best, w);
			LC_LOG("DebugShore: player put in the water at GTA %.1f %.1f (MC %.1f %.1f %.1f) heading %.0f, %.0f m from where he was; the water (surface %.2f) starts 6 m back, at MC %.1f %.1f; a ground map follows in 4 s",
				px, py, at.x, at.y, at.z, heading, best + 6.0f, w, edge.x, edge.z);
		}

		std::int32_t Bits(float a_f)
		{
			std::int32_t i = 0;
			std::memcpy(&i, &a_f, sizeof(i));
			return i;
		}

		bool IsGun(unsigned a_w) { return a_w == kWeaponPistol || (a_w >= 9 && a_w <= 17); }  // pistols, shotguns, SMGs, rifles, sniper rifles

		// The gun a ped has (in hand, or in a gun slot with ammo), 0: none.
		unsigned GunOf(int a_ped)
		{
			unsigned w = 0;
			S::GET_CURRENT_CHAR_WEAPON(a_ped, &w);
			if (IsGun(w)) {
				return w;
			}
			for (const unsigned slot : { 2u, 4u, 5u, 3u, 6u }) {
				unsigned type = 0, ammo0 = 0, ammo1 = 0;
				S::GET_CHAR_WEAPON_IN_SLOT(a_ped, slot, &type, &ammo0, &ammo1);
				if (IsGun(type) && (ammo0 > 0 || ammo1 > 0)) {
					return type;
				}
			}
			return 0;
		}

		// (Never a mission character: a script runs it, and told to run or shoot it walked out of its mission,
		// the date ran off. Missions.h)
		bool Usable(int a_ped, int a_player)
		{
			return a_ped && a_ped != a_player && S::DOES_CHAR_EXIST(a_ped) && !S::IS_CHAR_DEAD(a_ped) && !S::IS_CHAR_IN_ANY_CAR(a_ped) && !S::IS_PED_RAGDOLL(a_ped) &&
			       !Missions::IsMissionPed(a_ped);
		}

		void Order(int a_ped, const Mob& a_mob, bool a_cop, const char* a_why)
		{
			PedState& st = peds[a_ped];
			st.age = 0.0f;
			unsigned gun = GunOf(a_ped);
			if (!gun && a_cop) {
				S::GIVE_WEAPON_TO_CHAR(a_ped, kWeaponPistol, 120, false);  // police carry one
				gun = kWeaponPistol;
				++counters.gunsGiven;
			}
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &px, &py, &pz);
			const float dist = std::hypot(a_mob.x - px, a_mob.y - py);
			const float tx = a_mob.x, ty = a_mob.y, tz = a_mob.z0 + (a_mob.z1 - a_mob.z0) * 0.6f;
			const bool  first = st.mob != a_mob.id;
			if (gun) {
				// Renewed now and then, sooner when the mob moved away from where it aims: every new
				// order turns and aims first, so a ped told again too often never fires.
				const float moved = std::hypot(tx - st.aim[0], ty - st.aim[1]);
				if (!first && st.since < kShootEvery && (st.since < kReaimAfter || moved < kReaimMoved)) {
					return;
				}
				st.since = 0.0f;
				st.mob = a_mob.id;
				st.aim[0] = tx, st.aim[1] = ty, st.aim[2] = tz;
				S::SET_CURRENT_CHAR_WEAPON(a_ped, gun, true);
				if (Cfg().debugMobShoot == 1) {
					S::TASK_AIM_GUN_AT_COORD(a_ped, tx, ty, tz, kShootMs + 200);
					S::FIRE_PED_WEAPON(a_ped, tx, ty, tz);
				} else {
					S::TASK_SHOOT_AT_COORD(a_ped, Bits(tx), Bits(ty), Bits(tz), static_cast<std::int32_t>(kShootMs), kShootPattern);
				}
				++counters.orders;
				if (first) {
					LC_LOG("%s %d shoots at Minecraft mob %d %.1f m away (%s; weapon %u)", a_cop ? "cop" : "ped", a_ped, a_mob.id, dist, a_why, gun);
				}
				return;
			}
			if (dist > kFleeFrom || (!first && st.since < kFleeAgain)) {
				return;
			}
			st.since = 0.0f;
			st.mob = a_mob.id;
			S::TASK_SMART_FLEE_POINT(a_ped, a_mob.x, a_mob.y, a_mob.z0, kFleeRange, kFleeMs);
			++counters.flees;
			LC_LOG("ped %d runs from Minecraft mob %d %.1f m away (%s)", a_ped, a_mob.id, dist, a_why);
		}

		// Who deals with which mob this pass: each ped the nearest one it has reason to.
		struct Duty
		{
			Mob         mob;
			bool        cop = false;
			const char* why = "";
			float       d2 = 1e30f;
		};
		std::unordered_map<int, Duty> duties;

		void Consider(int a_ped, const Mob& a_mob, bool a_cop, const char* a_why)
		{
			float x = 0, y = 0, z = 0;
			S::GET_CHAR_COORDINATES(a_ped, &x, &y, &z);
			const float d2 = (a_mob.x - x) * (a_mob.x - x) + (a_mob.y - y) * (a_mob.y - y);
			auto [it, fresh] = duties.try_emplace(a_ped);
			if (fresh || d2 < it->second.d2) {
				it->second = Duty{ a_mob, a_cop, a_why, d2 };
			}
		}

		void React(int a_player)
		{
			std::vector<Mob> now;
			{
				std::lock_guard lock(mobLock);
				now = mobs;
			}
			for (auto& [ped, st] : peds) {
				st.since += kReactEvery;
			}
			duties.clear();
			auto* pool = CPools::ms_pPedPool;
			for (const Mob& m : now) {
				if (!m.target) {
					continue;
				}
				unsigned type = 0;
				if (Usable(m.target, a_player)) {
					S::GET_PED_TYPE(m.target, &type);
					Consider(m.target, m, type == kPedTypeCop, "it is after this ped");
				}
				// Police nearby join in.
				int cops = 0;
				for (int i = 0; pool && i < static_cast<int>(pool->m_nCount) && cops < kCopsPerMob; ++i) {
					CPed* p = pool->Get(i);
					if (!p || !p->m_pMatrix) {
						continue;
					}
					const auto& pos = p->m_pMatrix->pos;
					if (std::hypot(pos.x - m.x, pos.y - m.y) > kCopRange || std::fabs(pos.z - m.z0) > 6.0f) {
						continue;
					}
					const int handle = static_cast<int>(pool->GetIndex(p));
					if (handle == m.target || !Usable(handle, a_player)) {
						continue;
					}
					S::GET_PED_TYPE(handle, &type);
					if (type != kPedTypeCop) {
						continue;
					}
					++cops;
					Consider(handle, m, true, "police nearby");
				}
			}
			for (const auto& [ped, duty] : duties) {
				Order(ped, duty.mob, duty.cop, duty.why);
			}
		}

		// The ped that fired a shot from a_from: the player, or the nearest ped to the muzzle.
		int Shooter(const float a_from[3], int a_player)
		{
			float best = kShooterReach * kShooterReach;
			int   who = 0;
			if (a_player && S::DOES_CHAR_EXIST(a_player)) {
				float x = 0, y = 0, z = 0;
				S::GET_CHAR_COORDINATES(a_player, &x, &y, &z);
				const float d = (x - a_from[0]) * (x - a_from[0]) + (y - a_from[1]) * (y - a_from[1]) + (z - a_from[2]) * (z - a_from[2]);
				if (d < best) {
					best = d, who = a_player;
				}
			}
			auto* pool = CPools::ms_pPedPool;
			for (int i = 0; pool && i < static_cast<int>(pool->m_nCount); ++i) {
				CPed* p = pool->Get(i);
				if (!p || !p->m_pMatrix) {
					continue;
				}
				const auto& pos = p->m_pMatrix->pos;
				const float d = (pos.x - a_from[0]) * (pos.x - a_from[0]) + (pos.y - a_from[1]) * (pos.y - a_from[1]) + (pos.z - a_from[2]) * (pos.z - a_from[2]);
				if (d < best) {
					best = d, who = static_cast<int>(pool->GetIndex(p));
				}
			}
			return who;
		}

		void SendHits(int a_player)
		{
			std::vector<Shot> todo;
			{
				std::lock_guard lock(mobLock);
				todo.swap(shots);
			}
			for (const Shot& s : todo) {
				const int shooter = Shooter(s.from, a_player);
				unsigned  weapon = 0, damage = kDefaultDamage;
				if (shooter && S::DOES_CHAR_EXIST(shooter)) {
					S::GET_CURRENT_CHAR_WEAPON(shooter, &weapon);
				}
				if (IsGun(weapon)) {
					if (const CWeaponInfo* info = CWeaponInfo::GetWeaponInfo(weapon); info && info->m_nDamage > 0) {
						damage = info->m_nDamage;
					}
				}
				const float          mc = static_cast<float>(damage) / proto::kMobDamageScale;
				const std::int32_t   who = !shooter ? 0 : shooter == a_player ? proto::kMobHitByPlayer : static_cast<std::int32_t>(combat::ActorIdFromHandle(static_cast<std::uint32_t>(shooter)));
				Link::Get().PushInput(proto::kInMobHit, static_cast<std::uint16_t>(weapon), s.mob, who, static_cast<std::int32_t>(std::lround(mc * 100.0f)));
				++counters.hitsSent;
				LC_LOG_EVERY(200, "a GTA bullet (weapon %u, %u damage) from %s %d hit Minecraft mob %d at GTA %.1f %.1f %.1f: %.1f Minecraft damage", weapon, damage,
					shooter == a_player ? "the player" : "ped", shooter, s.mob, s.at[0], s.at[1], s.at[2], mc);
			}
		}

		// DebugMobFight=N: N s into play, a cop 5 m ahead and 2.5 m left of the player and a civilian 5 m
		// ahead and 2.5 m right stand still (Minecraft coordinates logged, for an autorun to summon mobs
		// beside them); their health, weapon and whether they shoot are logged every second for 40 s.
		void TestHook(float a_dt, int a_player, const proto::McState* a_mc)
		{
			auto& t = test;
			if (Cfg().debugMobFight <= 0 || t.stage >= 3 || !a_player) {
				return;
			}
			const unsigned copModel = S::GET_HASH_KEY("m_y_cop");
			if (t.stage == 0) {
				if ((t.timer += a_dt) < static_cast<float>(Cfg().debugMobFight)) {
					return;
				}
				CStreaming::ScriptRequestModel(static_cast<std::int32_t>(copModel));
				t.stage = 1;
				return;
			}
			if (t.stage == 1) {
				if (!S::HAS_MODEL_LOADED(copModel)) {
					return;
				}
				float x = 0, y = 0, z = 0, heading = 0;
				S::GET_CHAR_COORDINATES(a_player, &x, &y, &z);
				if (a_mc) {
					heading = McYawToGtaHeading(a_mc->yaw);
				} else {
					S::GET_CHAR_HEADING(a_player, &heading);
				}
				const float r = heading * kDegToRad, fx = -std::sin(r), fy = std::cos(r), rx = std::cos(r), ry = std::sin(r);
				const float cx = x + fx * 5.0f - rx * 2.5f, cy = y + fy * 5.0f - ry * 2.5f;
				const float vx = x + fx * 5.0f + rx * 2.5f, vy = y + fy * 5.0f + ry * 2.5f;
				S::CREATE_CHAR(6 /* PEDTYPE_COP */, copModel, cx, cy, z, &t.cop, true);
				S::MARK_MODEL_AS_NO_LONGER_NEEDED(copModel);
				S::CREATE_RANDOM_CHAR(vx, vy, z, &t.civ);
				for (const int ped : { t.cop, t.civ }) {
					if (!ped) {
						continue;
					}
					float px = 0, py = 0, pz = 0, ground = z - 1.0f;
					S::GET_CHAR_COORDINATES(ped, &px, &py, &pz);
					S::GET_GROUND_Z_FOR_3D_COORD(px, py, z + 1.0f, &ground);
					S::SET_CHAR_COORDINATES(ped, px, py, ground);
					S::SET_CHAR_HEADING(ped, heading + 180.0f);
					S::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, true);
					S::TASK_STAND_STILL(ped, 60000);
				}
				auto mcOf = [](int a_ped) {
					float px = 0, py = 0, pz = 0;
					if (a_ped) {
						S::GET_CHAR_COORDINATES(a_ped, &px, &py, &pz);
					}
					return GtaToMc(px, py, pz - 1.0f);
				};
				const McVec c = mcOf(t.cop), v = mcOf(t.civ);
				LC_LOG("DebugMobFight: cop %d (%08X) at MC %.2f %.2f %.2f, civilian %d (%08X) at MC %.2f %.2f %.2f; the player looks along MC %.3f %.3f", t.cop,
					combat::ActorIdFromHandle(static_cast<std::uint32_t>(t.cop)), c.x, c.y, c.z, t.civ, combat::ActorIdFromHandle(static_cast<std::uint32_t>(t.civ)), v.x,
					v.y, v.z, fx, -fy);
				t.stage = 2;
				t.timer = 0.0f;
				return;
			}
			t.timer += a_dt;
			if ((t.logTimer += a_dt) >= 1.0f) {
				t.logTimer = 0.0f;
				for (const int ped : { t.cop, t.civ }) {
					const bool exists = ped && S::DOES_CHAR_EXIST(ped);
					unsigned   health = 0, weapon = 0;
					float      px = 0, py = 0, pz = 0;
					if (exists) {
						S::GET_CHAR_HEALTH(ped, &health);
						S::GET_CURRENT_CHAR_WEAPON(ped, &weapon);
						S::GET_CHAR_COORDINATES(ped, &px, &py, &pz);
					}
					LC_LOG("DebugMobFight +%.0fs: %s %d %s, health %u, weapon %u, shooting %d, at GTA %.1f %.1f %.1f", t.timer, ped == t.cop ? "cop" : "civilian", ped,
						!exists ? "gone" : S::IS_CHAR_DEAD(ped) ? "dead" : "alive", health, weapon, exists && S::IS_CHAR_SHOOTING(ped), px, py, pz);
				}
			}
			if (t.timer > 40.0f) {
				for (int* ped : { &t.cop, &t.civ }) {
					if (*ped && S::DOES_CHAR_EXIST(*ped)) {
						S::MARK_CHAR_AS_NO_LONGER_NEEDED(ped);
					}
				}
				t.stage = 3;
			}
		}
	}

	void OnMob(const proto::McEvent& a_ev)
	{
		++counters.events;
		if (!Cfg().pedsFightMobs) {
			return;
		}
		Mob m;
		m.id = static_cast<int>(a_ev.flags);
		const GtaVec feet = McToGta(a_ev.a, a_ev.b, a_ev.c);
		m.x = static_cast<float>(feet.x), m.y = static_cast<float>(feet.y), m.z0 = static_cast<float>(feet.z);
		m.z1 = m.z0 + std::clamp(a_ev.d, 0.2f, 4.0f);
		m.half = std::clamp(static_cast<float>(a_ev.weapon & 0xFFFFu) / 200.0f, 0.1f, 2.0f);
		m.afterPlayer = (a_ev.weapon & proto::kMobAfterPlayer) != 0;
		m.passive = (a_ev.weapon & proto::kMobPassive) != 0;
		std::uint32_t handle = 0;
		m.target = a_ev.formId && combat::HandleFromActorId(a_ev.formId, handle) ? static_cast<int>(handle) : 0;
		std::lock_guard lock(mobLock);
		for (Mob& old : mobs) {
			if (old.id == m.id) {
				old = m;
				return;
			}
		}
		if (mobs.size() < proto::kMaxMobs * 2) {
			mobs.push_back(m);
		}
	}

	void Attacked(int a_ped, bool a_killed, int a_vehicle, float a_pushX, float a_pushY)
	{
		if (a_vehicle && !a_killed && Cfg().pedsFightMobs && a_ped && S::DOES_CHAR_EXIST(a_ped) && !S::IS_CHAR_DEAD(a_ped) && S::DOES_VEHICLE_EXIST(a_vehicle) &&
			!Missions::IsMissionPed(a_ped)) {
			// A mob is at the car: its driver drives off (not the player, whom GTA IV's own controls drive).
			CPed* player = FindPlayerPed();
			const int playerPed = player && CPools::ms_pPedPool ? static_cast<int>(CPools::ms_pPedPool->GetIndex(player)) : 0;
			PedState& st = peds[a_ped];
			st.age = 0.0f;
			if (a_ped != playerPed && st.since >= kFleeAgain) {
				st.since = 0.0f;
				S::FORCE_PED_TO_FLEE_WHILST_DRIVING_VEHICLE(a_ped, a_vehicle);
				++counters.flees;
				LC_LOG("the driver of vehicle %d (ped %d) drives off from a Minecraft mob", a_vehicle, a_ped);
			}
			return;
		}
		if (a_killed || a_vehicle || !Cfg().pedsFightMobs || !Usable(a_ped, 0)) {
			return;
		}
		float x = 0, y = 0, z = 0;
		S::GET_CHAR_COORDINATES(a_ped, &x, &y, &z);
		// The mob that did it: the nearest one we know, else a point against the push.
		Mob culprit;
		culprit.id = -1;
		float best = kHurtRange * kHurtRange;
		{
			std::lock_guard lock(mobLock);
			for (const Mob& m : mobs) {
				const float d = (m.x - x) * (m.x - x) + (m.y - y) * (m.y - y);
				if (m.passive && !m.target) {
					continue;  // a cow or an idle golem didn't do it
				}
				if (d < best) {
					best = d, culprit = m;
				}
			}
		}
		if (culprit.id < 0) {
			culprit.x = x - a_pushX * 2.0f, culprit.y = y - a_pushY * 2.0f, culprit.z0 = z - 1.0f, culprit.z1 = z + 0.8f;
			culprit.id = 0;
		}
		unsigned type = 0;
		S::GET_PED_TYPE(a_ped, &type);
		peds[a_ped].since = 99.0f;  // at once
		Order(a_ped, culprit, type == kPedTypeCop, "a mob hurt it");
	}

	int ClipShot(const float a_from[3], float a_to[3])
	{
		if (!Cfg().pedsFightMobs) {
			return 0;
		}
		const float d[3] = { a_to[0] - a_from[0], a_to[1] - a_from[1], a_to[2] - a_from[2] };
		float       best = 2.0f;
		int         id = 0;
		std::lock_guard lock(mobLock);
		for (const Mob& m : mobs) {
			const float lo[3] = { m.x - m.half, m.y - m.half, m.z0 }, hi[3] = { m.x + m.half, m.y + m.half, m.z1 };
			float t0 = 0.0f, t1 = 1.0f;
			bool  miss = false;
			for (int k = 0; k < 3 && !miss; ++k) {
				if (std::fabs(d[k]) < 1e-6f) {
					miss = a_from[k] < lo[k] || a_from[k] > hi[k];
					continue;
				}
				float a = (lo[k] - a_from[k]) / d[k], b = (hi[k] - a_from[k]) / d[k];
				if (a > b) {
					std::swap(a, b);
				}
				t0 = std::max(t0, a);
				t1 = std::min(t1, b);
				miss = t0 > t1;
			}
			if (!miss && t0 < best) {
				best = t0, id = m.id;
			}
		}
		if (id) {
			for (int k = 0; k < 3; ++k) {
				a_to[k] = a_from[k] + d[k] * best;
			}
			++counters.shotsAtMobs;
		}
		return id;
	}

	void ShotDone(int a_mob, const float a_from[3], const float a_at[3], int a_hits)
	{
		std::lock_guard lock(mobLock);
		if (a_hits > 0) {
			++counters.blocked;  // something in front of the mob took it
			return;
		}
		if (shots.size() < 64) {
			shots.push_back({ a_mob, { a_from[0], a_from[1], a_from[2] }, { a_at[0], a_at[1], a_at[2] } });
		}
	}

	void Tick(const Combat::Frame& a_frame)
	{
		const float a_dt = a_frame.dt;
		const int   a_player = a_frame.ped;
		const bool  a_playable = a_frame.exists && !a_frame.loading;
		{
			std::lock_guard lock(mobLock);
			for (Mob& m : mobs) {
				m.age += a_dt;
			}
			mobs.erase(std::remove_if(mobs.begin(), mobs.end(), [](const Mob& m) { return m.age > proto::kMobGoneSeconds; }), mobs.end());
		}
		for (auto it = peds.begin(); it != peds.end();) {
			it = (it->second.age += a_dt) > 30.0f ? peds.erase(it) : std::next(it);
		}
		if (!a_playable) {
			std::lock_guard lock(mobLock);
			shots.clear();
			return;
		}
		TestHook(a_dt, a_player, a_frame.mc);
		ShoreHook(a_frame);
		if (!Cfg().pedsFightMobs) {
			return;
		}
		SendHits(a_player);
		if ((reactTimer += a_dt) >= kReactEvery) {
			reactTimer = 0.0f;
			React(a_player);
		}
		if ((statsTimer += a_dt) >= 10.0f) {
			statsTimer = 0.0f;
			std::size_t known = 0;
			{
				std::lock_guard lock(mobLock);
				known = mobs.size();
			}
			if (counters.events || counters.orders || counters.flees || counters.shotsAtMobs) {
				LC_LOG("stats 10s: %u mob reports (%zu mobs known now); %u shooting orders, %u peds ran, %u cops given a pistol; GTA shots at mobs %u (%u hit one, %u "
					   "stopped in front)",
					counters.events, known, counters.orders, counters.flees, counters.gunsGiven, counters.shotsAtMobs, counters.hitsSent, counters.blocked);
			}
			counters = Counters{};
		}
	}

	void Reset()
	{
		{
			std::lock_guard lock(mobLock);
			mobs.clear();
			shots.clear();
		}
		peds.clear();
		test = FightTest{};
	}
}
