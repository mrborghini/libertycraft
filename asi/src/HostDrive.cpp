// Unity-built into dllmain.cpp (needs IV-SDK). See HostDrive.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "drive"
#include "HostDrive.h"

#include "Config.h"
#include "DriveLogic.h"
#include "Game.h"
#include "Log.h"

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

		void SetHidden(int a_ped, bool a_hide)
		{
			if (a_hide && hiddenPed != a_ped) {
				if (hiddenPed && S::DOES_CHAR_EXIST(hiddenPed)) {
					S::SET_CHAR_VISIBLE(hiddenPed, true);
				}
				S::SET_CHAR_VISIBLE(a_ped, false);
				hiddenPed = a_ped;
				LC_LOG("Niko hidden in the vehicle (Minecraft's player rides there)");
			} else if (!a_hide && hiddenPed) {
				if (S::DOES_CHAR_EXIST(hiddenPed)) {
					S::SET_CHAR_VISIBLE(hiddenPed, true);
				}
				hiddenPed = 0;
				LC_LOG("Niko visible again");
			}
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
			LC_LOG("the enter press didn't take (GTA's enter control read %d, last %d before our press)", padEnterCurrent.load(), padEnterLast.load());
			if (!car.handle) {
				LC_LOG("no %svehicle is within %.0f m", how != "task" ? "empty " : "", kFallbackRadius);
				return;
			}
			if (how == "task") {
				LC_LOG("the enter press didn't take: TASK_ENTER_CAR_AS_DRIVER vehicle %d (%.1f m)", car.handle, car.distance);
				S::TASK_ENTER_CAR_AS_DRIVER(a_ped, car.handle, 10000);
			} else {
				LC_LOG("the enter press didn't take: WARP_CHAR_INTO_CAR vehicle %d (%.1f m)", car.handle, car.distance);
				S::WARP_CHAR_INTO_CAR(a_ped, car.handle);
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
		in.toggles = togglePresses.exchange(0, std::memory_order_relaxed);
		in.vehicleActions = vehiclePresses.exchange(0, std::memory_order_relaxed);

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
					if ((debugIndoorT += 1.0f) >= 6.0f && !debugRelocate) {
						float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0;
						S::GET_CHAR_COORDINATES(a_f.ped, &x, &y, &z);
						if (S::GET_CLOSEST_CAR_NODE(x, y, z, &nx, &ny, &nz)) {
							debugRelocate = 3;
							debugRelocateTo[0] = nx, debugRelocateTo[1] = ny, debugRelocateTo[2] = nz + 1.0f;
							debugIndoorT = 0.0f;
							LC_LOG("DebugAutoVehicle: indoors/above ground (interior %d, %.1f m up): moving Niko to the road at %.1f %.1f %.1f", interior,
								aboveGround, nx, ny, nz);
						}
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
						LC_LOG("DebugAutoVehicle: test car %d parked at %.1f %.1f %.1f", debugCar, x, y, z);
						debugCooldown = 1.5f;
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
			LC_LOG("vehicle key: handing Niko to GTA IV and pressing its enter-vehicle control");
			enterClock = 0.0f;
		}
		if (wasEntering && !logic.entering() && a_f.inCar) {
			LC_LOG("Niko is in a vehicle (%.2f s after the key)", enterClock);
		}
		enterClock += a_f.dt;
		if (logic.entering() && in.gettingIn && !sawGettingIn) {
			LC_LOG("Niko is getting into a vehicle (%.2f s after the key)", enterClock);
		}
		sawGettingIn = logic.entering() && (sawGettingIn || in.gettingIn);
		pressEnter.store(out.pressEnter, std::memory_order_relaxed);
		if (out.fallbackEnter && a_f.exists) {
			EnterByOtherMeans(a_f.ped);
		}
		if (out.enterFailed) {
			LC_LOG("no vehicle entered after %.0f s; Minecraft takes the player back", drive::Logic::kGiveUpAfter);
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
				r.blocker = "DebugAutoVehicle: moving Niko to the road";
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

		// Minecraft's player sits on its mount in the seat: Niko would be in the way.
		const bool hide = out.inVehicle && Cfg().hideNikoInVehicle && logic.mode() == drive::Mode::kMinecraft && a_f.mcInWorld;
		if (a_f.exists) {
			SetHidden(a_f.ped, hide);
		}

		auto& st = Game::State();
		st.hostDrives = out.hostDrives;
		st.inVehicle = out.inVehicle;
		st.nikoMode = logic.mode() == drive::Mode::kNiko;
		return r;
	}
}
