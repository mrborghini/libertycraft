// Unity-built into dllmain.cpp (needs IV-SDK). See HostDrive.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "drive"
#include "HostDrive.h"

#include "Config.h"
#include "DriveLogic.h"
#include "Game.h"
#include "Log.h"
#include "NikoBody.h"
#include "NpcBlocks.h"
#include "combat/CombatMath.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace lc::HostDrive
{
	namespace
	{
		namespace S = ::Scripting;

		constexpr float kFallbackRadius = 10.0f;     // VehicleEnterFallback looks this far for a car
		constexpr float kDebugCarRadius = 12.0f;     // DebugAutoVehicle: "near a car"
		constexpr float kDebugToggleSeconds = 10.0f;
		constexpr float kDebugExitAfter = 12.0f;     // DebugAutoVehicle: seconds in a car before getting out
		constexpr float kDebugCooldown = 25.0f;
		constexpr float kExitPressSeconds = 0.3f;

		const Config& Cfg() { return Config::Get(); }

		drive::Logic logic;
		bool         configured = false;
		const char*  lastReason = nullptr;

		// key presses from the window thread, taken by Tick
		std::atomic<int> togglePresses{ 0 };
		std::atomic<int> vehiclePresses{ 0 };
		// what Pad presses, set by Tick
		std::atomic<bool> pressEnter{ false };
		std::atomic<bool> pressExit{ false };

		int   hiddenPed = 0;  // the ped we made invisible in a vehicle (0: none)
		int   seatLoggedFor = 0;
		float debugToggleT = 0.0f;
		float debugCooldown = 12.0f;  // after a load Niko may still be falling into place
		float debugInCarT = 0.0f;
		float exitPressT = 0.0f;
		float debugExitCheckT = 0.0f;  // DebugAutoVehicle: seconds until it checks the exit press worked
		int   debugCar = 0;  // DebugAutoVehicle: the empty test car it parked next to Niko
		bool  debugCarRequested = false;
		float debugIndoorT = 0.0f;  // seconds puppeting indoors / off the ground without a car near
		int   debugRelocate = 0;    // frames left of moving Niko to the road (puppet off meanwhile)
		bool  debugRelocated = false;
		float debugRelocateTo[3]{};
		// what the pad held for GTA's enter control before Pad touched it (diagnostics)
		std::atomic<int> padEnterCurrent{ 0 }, padEnterLast{ 0 };
		bool  sawGettingIn = false;
		float enterClock = 0.0f;  // seconds since the vehicle key (log)
		int   tapsThisAttempt = 0;
		bool  lastStanding = true;
		// Pad runs only for the pad GTA hands the player's ped, i.e. while player control is on:
		// frames in a row it ran = GTA reads the player's pad again (the taps wait for that).
		std::atomic<std::uint32_t> padFrames{ 0 };
		std::uint32_t              padFramesSeen = 0;
		int                        padLiveRun = 0;
		constexpr int              kPadLiveFrames = 3;
		constexpr float            kMovingSpeed = 0.6f;  // m/s on the ground: GTA walks Niko to a door
		constexpr float            kStandWait = 0.75f;   // the taps wait this long at most for Niko to stand
		float                      releasedT = 0.0f;     // seconds since puppet mode let go for this attempt

		// GTA's enter-vehicle check (1.0.8.0: 0xA60D0B, in the player's on-foot task) takes the press
		// only while the ped counts as standing: CPed flag word 0x26C bit 0, set by the ped's ground
		// probe (0x93E710) and cleared by every move of the SET_CHAR_COORDINATES kind (0x945F38), i.e.
		// on every puppet frame. After puppet mode lets go the ped stands again 0.1 to 0.5 s later; the
		// old single press came before that and was lost (the player's second F then worked).
		bool PedStanding()
		{
			const CPed* p = FindPlayerPed();
			return p && (*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(p) + 0x26C) & 1u) != 0;
		}

		// ---- DebugWalkThroughCar (test hook) -----------------------------------------------------------
		// Walks the puppet target through a parked car and then into a pedestrian, first with the old
		// move (SET_CHAR_COORDINATES_NO_OFFSET), then with the direct one, and logs whether the car and
		// the ped survive. Both are made ambient (no longer needed) first: the clearing spares mission
		// entities, and the car the player walks into on the street is an ambient one.
		struct WalkTest
		{
			int           round = 0;  // 0 the native, 1 the direct move, 2 done
			int           step = 0;   // 0 wait, 1 spawn, 2 settle, 3 through the car, 4 into the ped, 5 back
			float         t = 0.0f, wait = 0.0f, dt = 0.0f;
			int           car = 0, ped = 0;
			bool          carGone = false, pedGone = false;
			float         carGoneAt = 0.0f, pedGoneAt = 0.0f;
			float         carClosest = 99.0f, pedClosest = 99.0f;  // the target's closest approach (m, 2D)
			bool          needStart = false, active = false, modelRequested = false;
			GtaVec        start{}, target{};
			float         dir[2]{};
			float         carPos[3]{};
			std::uint32_t vehBefore = 0, pedsBefore = 0;
		} walk;

		struct Car
		{
			int   handle = 0;
			float distance = 0.0f;
		};

		// The closest vehicle to the player within a_radius that isn't wrecked (the vehicle pool;
		// IV-SDK has no GET_CLOSEST_CAR).
		Car ClosestCar(int a_ped, float a_radius, bool a_emptyOnly = false)
		{
			constexpr float kMaxDz = 4.0f;
			Car best;
			CPool<CVehicle>* pool = CPools::ms_pVehiclePool;
			if (!pool) {
				return best;
			}
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &px, &py, &pz);
			float bestD2 = a_radius * a_radius;
			// (IV-SDK's range-for helpers for pools don't compile with clang: walk the slots.)
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CVehicle* veh = pool->Get(slot);
				if (!veh || !veh->m_pMatrix) {
					continue;
				}
				const auto& p = veh->m_pMatrix->pos;
				const float dx = p.x - px, dy = p.y - py, dz = p.z - pz;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 >= bestD2 || std::fabs(dz) > kMaxDz) {
					continue;
				}
				const int handle = static_cast<int>(pool->GetIndex(veh));
				if (!handle || !S::DOES_VEHICLE_EXIST(handle) || S::IS_CAR_DEAD(handle)) {
					continue;
				}
				if (a_emptyOnly) {
					int driver = 0;
					S::GET_DRIVER_OF_CAR(handle, &driver);
					if (driver) {
						continue;
					}
				}
				bestD2 = d2;
				best.handle = handle;
			}
			best.distance = best.handle ? std::sqrt(bestD2) : 0.0f;
			return best;
		}

		void ShowMode(drive::Mode a_mode)
		{
			const char* text = a_mode == drive::Mode::kNiko ? "LibertyCraft: Niko mode (plain GTA IV)" : "LibertyCraft: Minecraft mode";
			S::PRINT_STRING_WITH_LITERAL_STRING_NOW("STRING", text, 2000, true);
		}

		void SetHidden(int a_ped, bool a_hide, const char* a_why)
		{
			if (a_hide && hiddenPed == a_ped) {
				NikoBody::Hide(a_ped, true);  // every frame: leaving puppet mode (and scripts) show him again
			} else if (a_hide && hiddenPed != a_ped) {
				if (hiddenPed) {
					NikoBody::Hide(hiddenPed, false);
				}
				NikoBody::Hide(a_ped, true);
				hiddenPed = a_ped;
				LC_LOG("Niko (ped %d) hidden: %s", a_ped, a_why);
			} else if (!a_hide && hiddenPed) {
				NikoBody::Hide(hiddenPed, false);
				hiddenPed = 0;
				LC_LOG("Niko visible again");
			}
		}

		bool gtaTookPress = false;  // GTA reacted to a tap in this attempt

		// ---- knocked over (RagdollOnVehicleHit): a car ran into the player, a blast ------------------------
		constexpr float kUprightSpeed = 0.5f;  // m/s: slower than this on his feet counts as standing
		constexpr float kHandBackSeconds = 0.5f;  // the body stays on Niko this long at most while Minecraft takes over
		constexpr float kHitRange = 9.0f;      // vehicles this close (centre to the player) are checked
		constexpr float kHitSpeed = 3.0f;      // m/s: slower cars push, they don't knock over
		constexpr float kHitMargin = 0.35f;    // the player's radius around the car's box
		struct Knock
		{
			bool  pending = false;  // ragdoll Niko once puppet mode has let go of him
			float gx = 0.0f, gy = 0.0f, force = 0.0f, wait = 0.0f;
			int   ms = 0;
			char  what[96] = "";
		};
		Knock knock;
		int   knockdowns = 0;  // for DriveLogic, this frame
		char  uprightWhy[96] = "";  // the standing check's parts (logged when a recovery runs out of time)
		float recoverClock = 0.0f;
		// DebugBailOut / DebugRunOver
		float debugBailT = 0.0f;
		bool  debugBailed = false;
		float debugRunT = 0.0f;
		int   debugRunCar = 0;
		float debugRunDrive = 0.0f;  // seconds the test car still gets pushed at the player
		float debugRunAge = 0.0f;
		bool  debugRunRequested = false;
		// DebugCutscene
		int   debugCutPhase = 0;  // 0 waiting, 1 loading, 2 playing, 3 done
		float debugCutT = 0.0f, debugCutLog = 0.0f;

		// DebugCutscene=<name>: 20 s into puppet mode, plays one of GTA's cutscenes (INIT_CUTSCENE,
		// START_CUTSCENE, CLEAR_CUTSCENE as the mission scripts do), once.
		void DebugCutsceneTick(const Frame& a_f)
		{
			const std::string& name = Cfg().debugCutscene;
			if (name.empty() || debugCutPhase == 3 || !a_f.exists) {
				return;
			}
			debugCutT += a_f.dt;
			switch (debugCutPhase) {
			case 0:
				if (a_f.puppeting && debugCutT >= 20.0f) {
					S::INIT_CUTSCENE(name.c_str());
					debugCutPhase = 1;
					debugCutT = 0.0f;
					LC_LOG("DebugCutscene: INIT_CUTSCENE(%s)", name.c_str());
				} else if (!a_f.puppeting) {
					debugCutT = std::min(debugCutT, 10.0f);
				}
				break;
			case 1:
				if (S::HAS_CUTSCENE_LOADED()) {
					S::START_CUTSCENE();
					debugCutPhase = 2;
					debugCutT = 0.0f;
					LC_LOG("DebugCutscene: %s loaded; START_CUTSCENE", name.c_str());
				} else if (debugCutT > 30.0f) {
					S::CLEAR_CUTSCENE();
					debugCutPhase = 3;
					LC_LOG("DebugCutscene: %s didn't load in 30 s", name.c_str());
				}
				break;
			case 2:
				if ((debugCutLog -= a_f.dt) <= 0.0f) {
					debugCutLog = 2.0f;
					LC_LOG("DebugCutscene: %s playing, %u ms, section %u, running %d", name.c_str(), S::GET_CUTSCENE_TIME(), S::GET_CUTSCENE_SECTION_PLAYING(),
						CCutsceneMgr::IsRunning() ? 1 : 0);
				}
				if (S::HAS_CUTSCENE_FINISHED() || debugCutT > 300.0f) {
					S::CLEAR_CUTSCENE();
					debugCutPhase = 3;
					LC_LOG("DebugCutscene: %s finished after %.1f s; CLEAR_CUTSCENE", name.c_str(), debugCutT);
				}
				break;
			default:
				break;
			}
		}

		void StartKnock(float a_gx, float a_gy, float a_force, int a_ms, const char* a_what)
		{
			knock.pending = true;
			knock.gx = a_gx;
			knock.gy = a_gy;
			knock.force = a_force;
			knock.ms = a_ms;
			knock.wait = 0.0f;
			std::snprintf(knock.what, sizeof(knock.what), "%s", a_what);
			++knockdowns;
		}

		// While puppeting: a vehicle moving into the player (the player's point inside its box, plus
		// their radius) faster than kHitSpeed. Niko is frozen with his collision off, so GTA never
		// sees the hit itself.
		void DetectVehicleHits(const Frame& a_f)
		{
			CPed* p = FindPlayerPed();
			auto* pool = CPools::ms_pVehiclePool;
			if (!p || !p->m_pMatrix || !pool) {
				return;
			}
			const auto P = p->m_pMatrix->pos;  // the root, ~1 m above the feet
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CVehicle* veh = pool->Get(slot);
				if (!veh || !veh->m_pMatrix) {
					continue;
				}
				const auto& m = *veh->m_pMatrix;
				const float dx = P.x - m.pos.x, dy = P.y - m.pos.y, dz = P.z - m.pos.z;
				if (dx * dx + dy * dy > kHitRange * kHitRange || std::fabs(dz) > 4.0f) {
					continue;
				}
				CVector v{};
				veh->GetVelocity(&v);
				const float speed = std::hypot(v.x, v.y);
				if (!std::isfinite(speed) || speed < kHitSpeed || v.x * dx + v.y * dy <= 0.0f) {
					continue;  // slow, or moving away from the player
				}
				const int handle = static_cast<int>(pool->GetIndex(veh));
				float     lo[3], hi[3];
				if (!NpcBlocks::ModelBox(handle, veh->m_nModelIndex, lo, hi)) {
					continue;
				}
				// IV-SDK's CMatrix rows: right = x, "up" = forward (y), "at" = up (z).
				const float lx = dx * m.right.x + dy * m.right.y + dz * m.right.z;
				const float ly = dx * m.up.x + dy * m.up.y + dz * m.up.z;
				const float lz = dx * m.at.x + dy * m.at.y + dz * m.at.z;
				if (lx < lo[0] - kHitMargin || lx > hi[0] + kHitMargin || ly < lo[1] - kHitMargin || ly > hi[1] + kHitMargin || lz < lo[2] - 1.2f ||
					lz > hi[2] + 0.3f) {
					continue;
				}
				// Thrown along the car's way; harder and longer the faster it was.
				const float force = std::clamp(2.0f * speed, 8.0f, 40.0f);
				const int   ms = static_cast<int>(std::clamp(1500.0f + 150.0f * speed, 1500.0f, 5000.0f));
				// (GTA's own damage while he tumbles comes on top: Minecraft still owns his health then)
				const float gtaDamage = std::clamp(2.5f * speed, 8.0f, 100.0f);
				char        what[96];
				std::snprintf(what, sizeof(what), "vehicle %d hit the player at %.1f m/s", handle, speed);
				StartKnock(v.x / speed, v.y / speed, force, ms, what);
				float mcDamage = 0.0f;
				if (Config::Get().combat) {
					mcDamage = combat::McDamageFromGta(gtaDamage, Config::Get().playerDamageScale);
					Game::ReportHurt(::libertycraft::proto::kHurtOther, combat::HostDamageForMc(mcDamage), 0, 0);
				}
				LC_LOG("%s (%.1f m to its side, %.1f m along it): knocked over (force %.0f, ragdoll %d ms), %.0f GTA damage -> %.2f Minecraft damage", what, lx, ly,
					force, ms, gtaDamage, mcDamage);
				return;
			}
		}

		// DebugBailOut: DebugAutoVehicle's car drives off at speed after 3 s and Niko bails out at 5 s.
		bool DebugBailOutTick(const Frame& a_f)
		{
			if (!Cfg().debugBailOut || !a_f.inCar) {
				debugBailT = 0.0f;
				debugBailed = false;
				return false;
			}
			debugBailT += a_f.dt;
			int veh = 0;
			S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
			if (veh && S::DOES_VEHICLE_EXIST(veh) && debugBailT >= 3.0f && debugBailT < 6.0f) {
				S::SET_CAR_FORWARD_SPEED(veh, 14.0f);
			}
			if (!debugBailed && debugBailT >= 5.0f) {
				debugBailed = true;
				float speed = 0.0f;
				if (veh) {
					S::GET_CAR_SPEED(veh, &speed);
				}
				LC_LOG("DebugBailOut: GTA's exit control at %.1f m/s", speed);
				return true;
			}
			return false;
		}

		bool StartRelocation(int a_ped, const char* a_who, int a_interior, float a_aboveGround);

		// DebugRunOver: every 40 s of puppeting (outdoors), a test car 15 m up the road drives at the player.
		void DebugRunOverTick(const Frame& a_f)
		{
			if (!Cfg().debugRunOver || !a_f.exists) {
				return;
			}
			if (debugRunCar) {
				debugRunAge += a_f.dt;
				const bool exists = S::DOES_VEHICLE_EXIST(debugRunCar);
				if (exists && debugRunDrive > 0.0f && !knock.pending && !logic.recovering()) {
					debugRunDrive -= a_f.dt;
					S::SET_CAR_FORWARD_SPEED(debugRunCar, 12.0f);
				}
				if (!exists || debugRunAge > 12.0f) {
					if (exists) {
						S::MARK_CAR_AS_NO_LONGER_NEEDED(&debugRunCar);
					}
					debugRunCar = 0;
				}
			}
			if (!a_f.puppeting || a_f.paused || logic.mode() != drive::Mode::kMinecraft) {
				return;
			}
			debugRunT += a_f.dt;
			if (debugRunT < 15.0f || debugRunCar) {
				return;
			}
			float aboveGround = 99.0f;
			S::GET_CHAR_HEIGHT_ABOVE_GROUND(a_f.ped, &aboveGround);
			int interior = 0;
			S::GET_INTERIOR_FROM_CHAR(a_f.ped, &interior);
			if (aboveGround > 2.0f || interior != 0) {
				if (debugRunT > 21.0f && StartRelocation(a_f.ped, "DebugRunOver", interior, aboveGround)) {
					debugRunT = 0.0f;
				}
				return;
			}
			const unsigned int model = S::GET_HASH_KEY("admiral");
			if (!debugRunRequested) {
				debugRunRequested = true;
				CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
			}
			if (!S::HAS_MODEL_LOADED(model)) {
				return;
			}
			debugRunRequested = false;
			float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, heading = 0;
			S::GET_CHAR_COORDINATES(a_f.ped, &x, &y, &z);
			if (!S::GET_CLOSEST_CAR_NODE_WITH_HEADING(x, y, z, &nx, &ny, &nz, &heading)) {
				heading = 0.0f;
			}
			const float h = heading * kDegToRad;
			const float sx = x - std::sin(h) * 15.0f, sy = y + std::cos(h) * 15.0f;
			float       ground = z;
			S::GET_GROUND_Z_FOR_3D_COORD(sx, sy, z + 3.0f, &ground);
			const float aim = std::atan2(-(x - sx), y - sy) / kDegToRad;
			S::CREATE_CAR(model, sx, sy, ground + 0.5f, &debugRunCar, true);
			S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
			if (debugRunCar) {
				S::SET_CAR_HEADING(debugRunCar, aim);
			}
			debugRunDrive = 3.0f;
			debugRunAge = 0.0f;
			debugRunT = -25.0f;  // the next one 40 s on
			LC_LOG("DebugRunOver: test car %d 15 m up the road (%.1f %.1f %.1f), driving at the player at 12 m/s (heading %.0f)", debugRunCar, sx, sy, ground,
				aim);
		}

		void EnterByOtherMeans(int a_ped)
		{
			const auto& how = Cfg().vehicleEnterFallback;
			if (how == "none") {
				LC_LOG("the enter press didn't take; VehicleEnterFallback=none");
				return;
			}
			// A warp needs a free driver's seat; the task pulls the driver out like GTA's own F.
			const Car car = ClosestCar(a_ped, kFallbackRadius, how != "task");
			if (gtaTookPress) {
				LC_LOG("GTA took the enter press but Niko stopped short of getting in");
			} else {
				LC_LOG("the enter press didn't take (GTA's enter control read %d, last %d before our press)", padEnterCurrent.load(), padEnterLast.load());
			}
			if (!car.handle) {
				LC_LOG("no %svehicle is within %.0f m", how != "task" ? "empty " : "", kFallbackRadius);
				return;
			}
			if (how == "task") {
				LC_LOG("VehicleEnterFallback: TASK_ENTER_CAR_AS_DRIVER vehicle %d (%.1f m)", car.handle, car.distance);
				S::TASK_ENTER_CAR_AS_DRIVER(a_ped, car.handle, 10000);
			} else {
				LC_LOG("VehicleEnterFallback: WARP_CHAR_INTO_CAR vehicle %d (%.1f m)", car.handle, car.distance);
				S::WARP_CHAR_INTO_CAR(a_ped, car.handle);
			}
		}

		// Test hooks: a save can start indoors (Roman's flat); move Niko out to the nearest road (puppet
		// mode off for a few frames, then the usual resync).
		bool StartRelocation(int a_ped, const char* a_who, int a_interior, float a_aboveGround)
		{
			float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &x, &y, &z);
			if (debugRelocate || !S::GET_CLOSEST_CAR_NODE(x, y, z, &nx, &ny, &nz)) {
				return false;
			}
			debugRelocate = 3;
			debugRelocateTo[0] = nx, debugRelocateTo[1] = ny, debugRelocateTo[2] = nz;  // (SET_CHAR_COORDINATES adds 1 m: the root)
			LC_LOG("%s: indoors/above ground (interior %d, %.1f m up): moving Niko to the road at %.1f %.1f %.1f", a_who, a_interior, a_aboveGround, nx, ny, nz);
			return true;
		}

		std::uint32_t PoolUsed(bool a_vehicles)
		{
			if (a_vehicles) {
				return CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->m_nUsed : 0;
			}
			return CPools::ms_pPedPool ? CPools::ms_pPedPool->m_nUsed : 0;
		}

		// The closest living pedestrian on foot within a_radius, other than a_ped (the ped pool).
		int ClosestPed(int a_ped, float a_radius)
		{
			CPool<CPed>* pool = CPools::ms_pPedPool;
			if (!pool) {
				return 0;
			}
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_ped, &px, &py, &pz);
			float bestD2 = a_radius * a_radius;
			int   best = 0;
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CPed* p = pool->Get(slot);
				if (!p || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float dx = m.x - px, dy = m.y - py, dz = m.z - pz, d2 = dx * dx + dy * dy;
				if (d2 >= bestD2 || std::fabs(dz) > 3.0f) {
					continue;
				}
				const int handle = static_cast<int>(pool->GetIndex(p));
				if (!handle || handle == a_ped || !S::DOES_CHAR_EXIST(handle) || S::IS_CHAR_DEAD(handle) || S::IS_CHAR_IN_ANY_CAR(handle)) {
					continue;
				}
				bestD2 = d2;
				best = handle;
			}
			return best;
		}

		void WalkTestTick(const Frame& a_f)
		{
			auto& w = walk;
			w.dt = a_f.dt;
			if (!Cfg().debugWalkThroughCar || w.round >= 2 || a_f.paused) {
				return;
			}
			if (!a_f.puppeting || a_f.inCar || a_f.dead) {
				w.wait = 0.0f;
				if (w.step >= 3) {
					LC_LOG("DebugWalkThroughCar: puppet mode ended mid-walk; starting this round again");
					w.step = 0;
					w.active = false;
				}
				return;
			}
			w.t += a_f.dt;
			switch (w.step) {
			case 0: {
				if ((w.wait += a_f.dt) < (w.round == 0 ? 10.0f : 4.0f)) {
					break;
				}
				float aboveGround = 99.0f;
				S::GET_CHAR_HEIGHT_ABOVE_GROUND(a_f.ped, &aboveGround);
				int interior = 0;
				S::GET_INTERIOR_FROM_CHAR(a_f.ped, &interior);
				if (aboveGround > 2.0f || interior != 0) {
					StartRelocation(a_f.ped, "DebugWalkThroughCar", interior, aboveGround);
					w.wait = 0.0f;
					break;
				}
				w.step = 1;
				w.modelRequested = false;
				break;
			}
			case 1: {
				const unsigned int model = S::GET_HASH_KEY("admiral");
				if (!w.modelRequested) {
					w.modelRequested = true;
					CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
				}
				if (!S::HAS_MODEL_LOADED(model)) {
					break;
				}
				float x = 0, y = 0, z = 0, h = 0;
				S::GET_CHAR_COORDINATES(a_f.ped, &x, &y, &z);
				S::GET_CHAR_HEADING(a_f.ped, &h);
				w.dir[0] = -std::sin(h * kDegToRad);
				w.dir[1] = std::cos(h * kDegToRad);
				w.carPos[0] = x + w.dir[0] * 5.0f, w.carPos[1] = y + w.dir[1] * 5.0f, w.carPos[2] = z;
				w.car = 0;
				S::CREATE_CAR(model, w.carPos[0], w.carPos[1], w.carPos[2], &w.car, true);
				S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
				if (w.car) {
					S::SET_CAR_HEADING(w.car, h + 90.0f);  // across the path
				}
				w.ped = 0;
				S::CREATE_RANDOM_CHAR(x + w.dir[0] * 9.0f, y + w.dir[1] * 9.0f, z, &w.ped);
				if (w.ped) {
					S::TASK_STAND_STILL(w.ped, 20000);
				}
				// Ambient now, like the cars and pedestrians on the street (the clearing spares mission entities).
				int car = w.car, ped = w.ped;
				if (car) {
					S::MARK_CAR_AS_NO_LONGER_NEEDED(&car);
				}
				if (ped) {
					S::MARK_CHAR_AS_NO_LONGER_NEEDED(&ped);
				}
				LC_LOG("DebugWalkThroughCar round %d (%s move): car %d parked 5 m ahead across the path at %.1f %.1f %.1f, pedestrian %d 9 m ahead",
					w.round + 1, w.round == 0 ? "native" : "direct", w.car, w.carPos[0], w.carPos[1], w.carPos[2], w.ped);
				w.step = 2;
				w.t = 0.0f;
				break;
			}
			case 2:
				if (w.t >= 1.5f) {
					w.vehBefore = PoolUsed(true);
					w.pedsBefore = PoolUsed(false);
					w.carGone = w.pedGone = false;
					w.needStart = true;
					w.active = true;
					w.step = 3;
					w.t = 0.0f;
				}
				break;
			default: {
				if (w.car && !w.carGone && !S::DOES_VEHICLE_EXIST(w.car)) {
					w.carGone = true;
					w.carGoneAt = w.t;
					const float dx = static_cast<float>(w.target.x) - w.carPos[0], dy = static_cast<float>(w.target.y) - w.carPos[1];
					LC_LOG("DebugWalkThroughCar round %d: car %d DELETED (%.2f s into step %d, the player %.1f m from where it was parked)", w.round + 1, w.car,
						w.t, w.step, std::sqrt(dx * dx + dy * dy));
				}
				if (w.ped && !w.pedGone && !S::DOES_CHAR_EXIST(w.ped)) {
					w.pedGone = true;
					w.pedGoneAt = w.t;
					LC_LOG("DebugWalkThroughCar round %d: pedestrian %d DELETED (%.2f s into step %d)", w.round + 1, w.ped, w.t, w.step);
				}
				{
					const float dx = static_cast<float>(w.target.x) - w.carPos[0], dy = static_cast<float>(w.target.y) - w.carPos[1];
					w.carClosest = std::min(w.carClosest, std::sqrt(dx * dx + dy * dy));
				}
				if (w.step == 4 && w.ped && !w.pedGone && S::DOES_CHAR_EXIST(w.ped)) {
					float x = 0, y = 0, z = 0;
					S::GET_CHAR_COORDINATES(w.ped, &x, &y, &z);
					const float dx = static_cast<float>(w.target.x) - x, dy = static_cast<float>(w.target.y) - y;
					w.pedClosest = std::min(w.pedClosest, std::sqrt(dx * dx + dy * dy));
				}
				if (w.step == 3 && w.t >= 6.0f) {
					// Into the closest pedestrian on foot (an ambient one if one is nearer than ours).
					w.ped = ClosestPed(a_f.ped, 30.0f);
					w.step = w.ped ? 4 : 5;
					w.t = 0.0f;
					if (w.ped) {
						LC_LOG("DebugWalkThroughCar round %d: walking into pedestrian %d", w.round + 1, w.ped);
					}
				} else if (w.step == 4 && w.t >= 5.0f) {
					w.step = 5;
					w.t = 0.0f;
				} else if (w.step == 5 && w.t >= 2.0f) {
					w.active = false;
					LC_LOG("DebugWalkThroughCar round %d (%s move): car %d %s (the player came within %.2f m of its centre), pedestrian %d %s (within %.2f m); "
						   "vehicle pool %u -> %u, ped pool %u -> %u",
						w.round + 1, w.round == 0 ? "native" : "direct", w.car, w.carGone ? "DELETED" : "still there", w.carClosest, w.ped,
						!w.ped ? "(none)" : w.pedGone ? "DELETED" : "still there", w.pedClosest, w.vehBefore, PoolUsed(true), w.pedsBefore, PoolUsed(false));
					w.carClosest = w.pedClosest = 99.0f;
					++w.round;
					w.step = 0;
					w.wait = 0.0f;
				}
				break;
			}
			}
		}

		bool Usable()
		{
			const auto& st = Game::State();
			return !st.gtaMenuOpen.load(std::memory_order_relaxed);
		}
	}

	bool OnKey(std::uint32_t a_dik, bool a_down, bool a_repeat)
	{
		if (!a_dik || !Usable()) {
			return false;
		}
		const auto& st = Game::State();
		const bool  puppeting = st.puppeting.load(std::memory_order_relaxed);
		if (puppeting && st.mcScreenOpen.load(std::memory_order_relaxed)) {
			return false;  // typing into a Minecraft screen
		}
		const auto& cfg = Cfg();
		if (a_dik == cfg.ToggleKeyDik()) {
			if (a_down && !a_repeat) {
				togglePresses.fetch_add(1, std::memory_order_relaxed);
			}
			return true;
		}
		if (puppeting && a_dik == cfg.VehicleKeyDik()) {
			if (a_down && !a_repeat) {
				vehiclePresses.fetch_add(1, std::memory_order_relaxed);
			}
			return true;  // Minecraft never sees it (its F would swap hands)
		}
		return false;
	}

	void Pad(CPad* a_pad)
	{
		if (!a_pad) {
			return;
		}
		padFrames.fetch_add(1, std::memory_order_relaxed);
		if (pressEnter.load(std::memory_order_relaxed)) {
			padEnterCurrent.store(a_pad->m_aValues[INPUT_ENTER].m_nCurrentValue, std::memory_order_relaxed);
			padEnterLast.store(a_pad->m_aValues[INPUT_ENTER].m_nLastValue, std::memory_order_relaxed);
			a_pad->m_aValues[INPUT_ENTER].m_nCurrentValue = 255;
		}
		if (pressExit.load(std::memory_order_relaxed)) {
			a_pad->m_aValues[INPUT_VEH_EXIT].m_nCurrentValue = 255;
			a_pad->m_aValues[INPUT_ENTER].m_nCurrentValue = 255;
		}
	}

	void OnIngameStartup()
	{
		logic.Reset();
		pressEnter = false;
		pressExit = false;
		hiddenPed = 0;  // the ped is going away with the old session
		knock = Knock{};
		knockdowns = 0;
		debugRunCar = 0;
		NikoBody::OnIngameStartup();
		seatLoggedFor = 0;
		lastReason = nullptr;
	}

	Result Tick(const Frame& a_f)
	{
		if (!configured) {
			configured = true;
			logic = drive::Logic(Cfg().toggleStartsInMinecraft);
			LC_LOG("vehicles: %s (dik 0x%02X) enters a vehicle while Minecraft drives; %s (dik 0x%02X) toggles Minecraft/Niko mode; starting in %s mode",
				Cfg().vehicleKey.c_str(), Cfg().VehicleKeyDik(), Cfg().toggleKey.c_str(), Cfg().ToggleKeyDik(),
				logic.mode() == drive::Mode::kMinecraft ? "Minecraft" : "Niko");
		}
		const bool inGame = a_f.exists && !a_f.loading;
		const bool active = inGame && !a_f.paused;

		drive::Input in;
		in.dt = a_f.dt;
		in.inGame = inGame;
		in.paused = a_f.paused;
		in.dead = a_f.dead;
		in.inCar = a_f.inCar;
		in.gettingIn = a_f.exists && !a_f.inCar && S::IS_CHAR_GETTING_IN_TO_A_CAR(a_f.ped);
		in.cutscene = a_f.cutscene;
		in.puppeting = a_f.puppeting;
		const std::uint32_t pads = padFrames.load(std::memory_order_relaxed);
		padLiveRun = pads != padFramesSeen ? padLiveRun + 1 : 0;
		padFramesSeen = pads;
		releasedT = logic.entering() && !a_f.puppeting ? releasedT + (a_f.paused ? 0.0f : a_f.dt) : 0.0f;
		in.controlReady = a_f.exists && !a_f.puppeting && padLiveRun >= kPadLiveFrames && S::IS_PLAYER_CONTROL_ON(a_f.player) &&
		                  (PedStanding() || releasedT >= kStandWait);
		if (logic.entering() && a_f.exists && !a_f.puppeting && !a_f.inCar) {
			float vx = 0, vy = 0, vz = 0;
			S::GET_CHAR_VELOCITY(a_f.ped, &vx, &vy, &vz);
			in.moving = vx * vx + vy * vy > kMovingSpeed * kMovingSpeed;
		}
		in.toggles = togglePresses.exchange(0, std::memory_order_relaxed);
		in.vehicleActions = vehiclePresses.exchange(0, std::memory_order_relaxed);
		// Standing again? (after a vehicle or a fall, GTA keeps Niko until he is: DriveLogic)
		if (a_f.exists && !a_f.inCar && !a_f.dead) {
			in.ragdoll = S::IS_PED_RAGDOLL(a_f.ped) || S::IS_CHAR_GETTING_UP(a_f.ped);
			if (!a_f.puppeting) {
				float speed = 0.0f;
				S::GET_CHAR_SPEED(a_f.ped, &speed);
				const bool air = S::IS_CHAR_IN_AIR(a_f.ped), standing = PedStanding();
				in.upright = !in.ragdoll && !air && speed < kUprightSpeed && standing;
				std::snprintf(uprightWhy, sizeof(uprightWhy), "ragdoll/getting up %d, in the air %d, %.2f m/s, standing flag %d", in.ragdoll ? 1 : 0, air ? 1 : 0,
					speed, standing ? 1 : 0);
			}
		}
		// Knocked over: a car running into the puppeted player (here), a blast (Combat, last frame).
		if (Cfg().ragdollOnVehicleHit && active && a_f.puppeting && !a_f.inCar && logic.mode() == drive::Mode::kMinecraft && !knock.pending &&
			!logic.recovering()) {
			DetectVehicleHits(a_f);
		}
		in.knockdowns = knockdowns;
		knockdowns = 0;

		// ---- test hooks (DebugAutoToggle / DebugAutoVehicle) -----------------------------------------
		if (Cfg().debugAutoToggle && active && (debugToggleT += a_f.dt) >= kDebugToggleSeconds) {
			debugToggleT = 0.0f;
			++in.toggles;
			LC_LOG("DebugAutoToggle: toggling");
		}
		bool exitNow = false;
		if (Cfg().debugAutoVehicle && active) {
			debugCooldown -= a_f.dt;
			debugInCarT = a_f.inCar ? debugInCarT + a_f.dt : 0.0f;
			if (debugCooldown <= 0.0f && a_f.puppeting && !a_f.inCar && !logic.entering()) {
				// Traffic drives off and has drivers: park an empty car of our own next to Niko first
				// (once he stands on something: right after a load he may still be falling into place).
				float aboveGround = 99.0f;
				S::GET_CHAR_HEIGHT_ABOVE_GROUND(a_f.ped, &aboveGround);
				int interior = 0;
				S::GET_INTERIOR_FROM_CHAR(a_f.ped, &interior);
				if (const Car car = ClosestCar(a_f.ped, kDebugCarRadius); car.handle) {
					LC_LOG("DebugAutoVehicle: vehicle %d is %.1f m away; pressing the vehicle key", car.handle, car.distance);
					++in.vehicleActions;
					debugCooldown = kDebugCooldown;
				} else if (aboveGround > 2.0f || interior != 0) {
					// Indoors or up somewhere (a save can start in Roman's flat): out to the street.
					debugCooldown = 1.0f;
					if ((debugIndoorT += 1.0f) >= 6.0f && StartRelocation(a_f.ped, "DebugAutoVehicle", interior, aboveGround)) {
						debugIndoorT = 0.0f;
					}
				} else {
					debugIndoorT = 0.0f;
					if (debugCar && !debugCarRequested && S::DOES_VEHICLE_EXIST(debugCar)) {
						S::MARK_CAR_AS_NO_LONGER_NEEDED(&debugCar);  // left behind somewhere
					}
					const unsigned int model = S::GET_HASH_KEY("admiral");
					if (!debugCarRequested) {
						debugCarRequested = true;
						CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
						LC_LOG("DebugAutoVehicle: requesting a test car model");
					}
					if (S::HAS_MODEL_LOADED(model)) {
						float x = 0, y = 0, z = 0;
						S::GET_OFFSET_FROM_CHAR_IN_WORLD_COORDS(a_f.ped, 2.5f, 2.5f, 0.0f, &x, &y, &z);
						S::CREATE_CAR(model, x, y, z, &debugCar, true);
						S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
						debugCarRequested = false;
						int driver = 0;
						if (Cfg().debugVehicleDriver && debugCar) {
							S::CREATE_RANDOM_CHAR_AS_DRIVER(debugCar, &driver);  // GTA's press then carjacks
							if (driver) {
								S::TASK_PAUSE(driver, 120000);  // ...a parked car: the driver waits instead of driving off
							}
						}
						LC_LOG("DebugAutoVehicle: test car %d parked at %.1f %.1f %.1f%s", debugCar, x, y, z, driver ? " with a driver" : "");
						debugCooldown = 6.0f;  // (GTA may need a moment before a fresh car can be entered)
					} else {
						debugCooldown = 0.25f;
					}
				}
			}
			if (debugCooldown <= 0.0f && a_f.inCar && debugInCarT >= kDebugExitAfter) {
				LC_LOG("DebugAutoVehicle: %.0f s in a vehicle; pressing GTA's exit control", debugInCarT);
				exitNow = true;
				debugExitCheckT = 3.0f;
				debugCooldown = kDebugCooldown;
			}
			// A parked test car can have its doors against a wall: out through the roof, then.
			if (debugExitCheckT > 0.0f && (debugExitCheckT -= a_f.dt) <= 0.0f && a_f.inCar) {
				int veh = 0;
				S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
				float cx = 0, cy = 0, cz = 0, h = 0;
				if (veh && S::DOES_VEHICLE_EXIST(veh)) {
					S::GET_CAR_COORDINATES(veh, &cx, &cy, &cz);
					S::GET_CAR_HEADING(veh, &h);
					const float x = cx - std::cos(h * kDegToRad) * 2.5f, y = cy - std::sin(h * kDegToRad) * 2.5f;
					S::WARP_CHAR_FROM_CAR_TO_COORD(a_f.ped, x, y, cz + 0.5f);
					LC_LOG("DebugAutoVehicle: still in the vehicle 3 s after the exit press; WARP_CHAR_FROM_CAR_TO_COORD %.1f %.1f %.1f", x, y, cz + 0.5f);
				}
			}
		}
		WalkTestTick(a_f);
		if (active && DebugBailOutTick(a_f)) {
			exitNow = true;
			debugExitCheckT = 0.0f;
		}
		if (active) {
			DebugRunOverTick(a_f);
		}
		if (inGame) {
			DebugCutsceneTick(a_f);
		}
		// DebugGiveWeapon: a GTA weapon for Niko, once (puppet mode holsters it, Game.cpp).
		static float giveT = 0.0f;
		if (Cfg().debugGiveWeapon > 0 && giveT >= 0.0f && active && (giveT += a_f.dt) >= 10.0f) {
			giveT = -1.0f;
			S::GIVE_WEAPON_TO_CHAR(a_f.ped, static_cast<unsigned>(Cfg().debugGiveWeapon), 60, false);
			unsigned now = 0;
			S::GET_CURRENT_CHAR_WEAPON(a_f.ped, &now);
			LC_LOG("DebugGiveWeapon: weapon %d given to Niko (current weapon now %u)", Cfg().debugGiveWeapon, now);
		}
		if (exitNow) {
			exitPressT = kExitPressSeconds;
		}
		exitPressT = std::max(0.0f, exitPressT - a_f.dt);
		pressExit.store(exitPressT > 0.0f, std::memory_order_relaxed);

		// ---- the verdict --------------------------------------------------------------------------------
		const bool wasEntering = logic.entering();
		const auto out = logic.Step(in);
		if (out.modeChanged) {
			LC_LOG("toggle: %s mode", logic.mode() == drive::Mode::kNiko ? "Niko" : "Minecraft");
			if (inGame) {
				ShowMode(logic.mode());
			}
		}
		if (logic.entering() && !wasEntering) {
			LC_LOG("vehicle key: handing Niko to GTA IV, then tapping its enter-vehicle control");
			enterClock = 0.0f;
			tapsThisAttempt = 0;
			gtaTookPress = false;
		}
		if (out.tapStarted) {
			tapsThisAttempt = out.tapStarted;
			LC_LOG("vehicle key: tap %d of GTA's enter control (%.2f s after the key; Niko %s)", out.tapStarted, enterClock,
				PedStanding() ? "stands" : "doesn't stand yet, waited long enough");
		}
		if (logic.entering() && a_f.exists) {
			// when GTA counts the released ped as standing again (log)
			const bool standing = PedStanding();
			if (standing != lastStanding) {
				LC_LOG("vehicle key: GTA counts Niko as %s (%.2f s after the key)", standing ? "standing" : "NOT standing", enterClock);
				lastStanding = standing;
			}
		} else {
			lastStanding = true;
		}
		if (out.accepted) {
			gtaTookPress = true;
			LC_LOG("GTA took the press (tap %d): Niko %s (%.2f s after the key)", tapsThisAttempt, in.gettingIn ? "is getting in" : "walks to a door", enterClock);
		}
		if (wasEntering && !logic.entering() && a_f.inCar) {
			LC_LOG("Niko is in a vehicle (%.2f s after the key)", enterClock);
		}
		enterClock += a_f.dt;
		if (logic.entering() && in.gettingIn && !sawGettingIn) {
			LC_LOG("Niko is getting into a vehicle (%.2f s after the key)", enterClock);
		}
		sawGettingIn = logic.entering() && (sawGettingIn || in.gettingIn);
		if (Cfg().debugInjectEnterKey) {
			// Test hook: the taps as real key events (SendInput, through Wine's DirectInput) instead
			// of pad writes, to compare with what GTA does for a real F.
			static bool injected = false;
			if (out.pressEnter != injected) {
				INPUT key{};
				key.type = INPUT_KEYBOARD;
				key.ki.wScan = Cfg().VehicleKeyDik();
				key.ki.dwFlags = KEYEVENTF_SCANCODE | (out.pressEnter ? 0 : KEYEVENTF_KEYUP);
				::SendInput(1, &key, sizeof(key));
				injected = out.pressEnter;
				if (out.pressEnter) {
					LC_LOG("DebugInjectEnterKey: real key down (scan code 0x%02X)", Cfg().VehicleKeyDik());
				}
			}
			pressEnter.store(false, std::memory_order_relaxed);
		} else {
			pressEnter.store(out.pressEnter, std::memory_order_relaxed);
		}
		if (out.fallbackEnter && a_f.exists) {
			EnterByOtherMeans(a_f.ped);
		}
		if (out.enterFailed) {
			LC_LOG("no vehicle entered after %.0f s; Minecraft takes the player back", drive::Logic::kGiveUpAfter);
		}
		recoverClock = logic.recovering() ? recoverClock + a_f.dt : 0.0f;
		if (out.recovered && logic.mode() == drive::Mode::kMinecraft) {
			LC_LOG("%s: Minecraft takes over%s%s", out.recoverCapped ? "Niko didn't get back up in time" : "Niko stands again", out.recoverCapped ? " (last: " : "",
				out.recoverCapped ? (std::string(uprightWhy) + ")").c_str() : "");
		}
		// A knockdown's ragdoll, once puppet mode has let go of Niko (frozen, he wouldn't fall).
		if (knock.pending) {
			knock.wait += a_f.dt;
			if (a_f.exists && !a_f.puppeting && !a_f.inCar && !a_f.dead) {
				S::SWITCH_PED_TO_RAGDOLL(a_f.ped, knock.ms, knock.ms, false, false, false, false);
				// World-direction force (APPLY_FORCE_TO_PED's 10th argument 0; Combat.cpp measured it).
				S::APPLY_FORCE_TO_PED(a_f.ped, 3, knock.gx * knock.force, knock.gy * knock.force, knock.force * 0.3f, 0.0f, 0.0f, 0.0f, 0, 0, 1, 1);
				LC_LOG("knocked over (%s): Niko ragdolled for %d ms, pushed %.2f %.2f x %.0f", knock.what, knock.ms, knock.gx, knock.gy, knock.force);
				knock.pending = false;
			} else if (knock.wait > 1.0f) {
				LC_LOG("knockdown dropped (%s): Niko wasn't free to fall within 1 s", knock.what);
				knock.pending = false;
			}
		}
		if (out.blocker != lastReason) {
			LC_LOG("GTA IV drives the player: %s", out.blocker ? out.blocker : "no, Minecraft may");
			lastReason = out.blocker;
		}

		Result r;
		r.blocker = out.blocker;
		r.hostDrives = out.hostDrives;
		r.inVehicle = out.inVehicle;
		r.resync = out.resync;
		r.heading = a_f.heading;
		if (debugRelocate > 0 && !out.hostDrives && a_f.exists) {
			// DebugAutoVehicle's move to the road: off puppet for a frame, move, then the usual resync.
			if (!a_f.puppeting && !debugRelocated) {
				S::SET_CHAR_COORDINATES(a_f.ped, debugRelocateTo[0], debugRelocateTo[1], debugRelocateTo[2]);
				debugRelocated = true;
			}
			if (--debugRelocate > 0) {
				r.blocker = "test hook: moving Niko to the road";
				r.hostDrives = true;
			} else {
				r.resync = true;
				debugRelocated = false;
			}
		}
		if (out.inVehicle) {
			int veh = 0;
			S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
			// GET_CHAR_COORDINATES gives the vehicle's origin for a ped inside one; the ped's own matrix
			// sits in its seat (the driver's is left of and in front of the middle).
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_f.ped, &px, &py, &pz);
			float sx = px, sy = py, sz = pz;
			CPed* pedObj = FindPlayerPed();
			if (pedObj && pedObj->m_pMatrix) {
				const auto& m = pedObj->m_pMatrix->pos;
				if (std::fabs(m.x - px) < 3.0f && std::fabs(m.y - py) < 3.0f && std::fabs(m.z - pz) < 3.0f) {
					sx = m.x, sy = m.y, sz = m.z;
				}
			}
			if (veh && S::DOES_VEHICLE_EXIST(veh)) {
				S::GET_CAR_HEADING(veh, &r.heading);
				if (seatLoggedFor != veh) {
					seatLoggedFor = veh;
					float ground = 0;
					S::GET_GROUND_Z_FOR_3D_COORD(px, py, pz, &ground);
					LC_LOG("in vehicle %d (heading %.1f): origin %.2f %.2f %.2f, seat (ped matrix) %.2f %.2f %.2f = %.2f m off the origin, %.2f m above "
						   "the ground %.2f; the rider's feet go %.2f m below the seat (VehicleSeatDrop)",
						veh, r.heading, px, py, pz, sx, sy, sz, std::sqrt((sx - px) * (sx - px) + (sy - py) * (sy - py) + (sz - pz) * (sz - pz)), sz - ground,
						ground, Cfg().vehicleSeatDrop);
				}
			}
			r.seatFeet = { sx, sy, sz - Cfg().vehicleSeatDrop };
		} else {
			seatLoggedFor = 0;
		}

		// While GTA animates Niko the Minecraft body may follow his skeleton instead (NikoBody.h);
		// else, in a vehicle, Minecraft's player sits on its mount in the seat: Niko would be in the way.
		// Handing back to Minecraft (the teleport handshake, then puppet mode, a frame or a few later):
		// the body stays on Niko and he stays hidden until puppet mode has him (it hides him itself), so
		// no frame shows him in between.
		static drive::Why lastWhy = drive::Why::kNone;
		static float      handBackT = 0.0f;
		if (out.hostDrives) {
			lastWhy = out.why;
			handBackT = 0.0f;
		} else if (out.resync && logic.mode() == drive::Mode::kMinecraft && a_f.mcInWorld) {
			handBackT = kHandBackSeconds;
		}
		handBackT = a_f.puppeting ? 0.0f : std::max(0.0f, handBackT - a_f.dt);
		const bool handingBack = handBackT > 0.0f && !out.hostDrives && lastWhy != drive::Why::kNone;
		const int  bodyPed = NikoBody::Target(a_f.exists && !a_f.dead ? a_f.ped : 0, handingBack ? lastWhy : out.why, out.hostDrives || handingBack, a_f.mcInWorld);
		const bool hide = out.inVehicle && Cfg().hideNikoInVehicle && logic.mode() == drive::Mode::kMinecraft && a_f.mcInWorld;
		if (bodyPed) {
			SetHidden(bodyPed, true, "the Minecraft body follows his animation");
		} else if (a_f.exists) {
			SetHidden(a_f.ped, hide, "Minecraft's player rides in the vehicle");
		}

		auto& st = Game::State();
		st.hostDrives = out.hostDrives;
		st.inVehicle = out.inVehicle;
		st.nikoMode = logic.mode() == drive::Mode::kNiko;
		st.cutscene = out.hostDrives && out.why == drive::Why::kCutscene;
		st.padLocked = out.padLocked && !a_f.paused;
		static bool lastPadLocked = false;
		if (out.padLocked != lastPadLocked) {
			lastPadLocked = out.padLocked;
			LC_LOG("%s", out.padLocked ? "Niko gets back up: the player's pad is zeroed until Minecraft takes over" : "the player's pad is GTA's or Minecraft's again");
		}
		return r;
	}

	void AfterPuppetDecision()
	{
		if (hiddenPed) {
			NikoBody::Hide(hiddenPed, true);
		}
	}

	bool KnockedOver()
	{
		return logic.recovering() && logic.recoveringFrom() == drive::Why::kRagdoll && logic.mode() == drive::Mode::kMinecraft;
	}

	void KnockDown(float a_gx, float a_gy, float a_force, int a_ragdollMs, const char* a_what)
	{
		if (!Cfg().ragdollOnVehicleHit || knock.pending || logic.recovering() || logic.mode() != drive::Mode::kMinecraft) {
			return;
		}
		const float len = std::hypot(a_gx, a_gy);
		StartKnock(len > 1e-3f ? a_gx / len : 0.0f, len > 1e-3f ? a_gy / len : 0.0f, a_force, a_ragdollMs, a_what);
		LC_LOG("%s: knocked over (force %.0f, ragdoll %d ms)", a_what, a_force, a_ragdollMs);
	}

	int DebugPuppetTarget(GtaVec& a_feet)
	{
		auto& w = walk;
		if (!w.active) {
			return 0;
		}
		if (w.needStart) {
			w.needStart = false;
			w.start = a_feet;
			w.target = a_feet;
		}
		// Moves the target toward a point at a_speed m/s; true once there.
		const auto toward = [&](double a_x, double a_y, float a_speed) {
			const double dx = a_x - w.target.x, dy = a_y - w.target.y, d = std::sqrt(dx * dx + dy * dy), step = a_speed * w.dt;
			if (d <= step || d < 1e-6) {
				w.target.x = a_x, w.target.y = a_y;
				return true;
			}
			w.target.x += dx / d * step, w.target.y += dy / d * step;
			return false;
		};
		if (w.step == 3) {
			// 10 m out along the path (through the car at 5 m) and back, 6 s.
			const float u = std::min(w.t / 6.0f, 1.0f), d = 10.0f * (u < 0.5f ? u * 2.0f : 2.0f - u * 2.0f);
			w.target.x = w.start.x + w.dir[0] * d;
			w.target.y = w.start.y + w.dir[1] * d;
		} else if (w.step == 4 && w.ped && !w.pedGone && S::DOES_CHAR_EXIST(w.ped)) {
			float x = 0, y = 0, z = 0;
			S::GET_CHAR_COORDINATES(w.ped, &x, &y, &z);
			toward(x, y, 8.0f);  // into the pedestrian, and stay there
		} else if (w.step == 5) {
			toward(a_feet.x, a_feet.y, 6.0f);  // back to where Minecraft's player is
		}
		w.target.z = a_feet.z;
		a_feet = w.target;
		return w.round == 0 ? 1 : 2;
	}
}
