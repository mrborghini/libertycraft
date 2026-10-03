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
		float debugCooldown = 5.0f;
		float debugInCarT = 0.0f;
		float exitPressT = 0.0f;
		int   debugWarpCar = 0;  // DebugAutoVehicle: no car close by, so Niko is put next to this one
		bool  sawGettingIn = false;
		float enterClock = 0.0f;  // seconds since the vehicle key (log)

		struct Car
		{
			int   handle = 0;
			float distance = 0.0f;
		};

		// The closest vehicle to the player within a_radius that isn't wrecked (the vehicle pool;
		// IV-SDK has no GET_CLOSEST_CAR).
		Car ClosestCar(int a_ped, float a_radius, float a_maxDz = 4.0f)
		{
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
				if (d2 >= bestD2 || std::fabs(dz) > a_maxDz) {
					continue;
				}
				const int handle = static_cast<int>(pool->GetIndex(veh));
				if (!handle || !S::DOES_VEHICLE_EXIST(handle) || S::IS_CAR_DEAD(handle)) {
					continue;
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
			const Car car = ClosestCar(a_ped, kFallbackRadius);
			if (!car.handle) {
				LC_LOG("the enter press didn't take and no vehicle is within %.0f m", kFallbackRadius);
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
				Car car = ClosestCar(a_f.ped, kDebugCarRadius);
				debugWarpCar = 0;
				if (!car.handle && (car = ClosestCar(a_f.ped, 150.0f, 30.0f)).handle) {
					debugWarpCar = car.handle;  // walked over there in no time
				}
				if (car.handle) {
					LC_LOG("DebugAutoVehicle: vehicle %d is %.1f m away%s; pressing the vehicle key", car.handle, car.distance,
						debugWarpCar ? " (putting Niko next to it first)" : "");
					++in.vehicleActions;
					debugCooldown = kDebugCooldown;
				} else {
					debugCooldown = 2.0f;
				}
			}
			if (debugCooldown <= 0.0f && a_f.inCar && debugInCarT >= kDebugExitAfter) {
				LC_LOG("DebugAutoVehicle: %.0f s in a vehicle; pressing GTA's exit control", debugInCarT);
				exitNow = true;
				debugCooldown = kDebugCooldown;
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
		if (debugWarpCar && logic.entering() && !a_f.puppeting && a_f.exists) {
			// DebugAutoVehicle: Niko is free now; stand him 3 m from the car on our side of it.
			if (S::DOES_VEHICLE_EXIST(debugWarpCar)) {
				float cx = 0, cy = 0, cz = 0, px = 0, py = 0, pz = 0;
				S::GET_CAR_COORDINATES(debugWarpCar, &cx, &cy, &cz);
				S::GET_CHAR_COORDINATES(a_f.ped, &px, &py, &pz);
				float dx = px - cx, dy = py - cy;
				const float len = std::sqrt(dx * dx + dy * dy);
				dx = len > 0.1f ? dx / len : 1.0f;
				dy = len > 0.1f ? dy / len : 0.0f;
				S::SET_CHAR_COORDINATES(a_f.ped, cx + dx * 3.0f, cy + dy * 3.0f, cz);
				LC_LOG("DebugAutoVehicle: Niko put next to vehicle %d at %.1f %.1f %.1f", debugWarpCar, cx + dx * 3.0f, cy + dy * 3.0f, cz);
			}
			debugWarpCar = 0;
		}
		if (!logic.entering()) {
			debugWarpCar = 0;
		}
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
		if (out.inVehicle) {
			int veh = 0;
			S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
			float px = 0, py = 0, pz = 0;
			S::GET_CHAR_COORDINATES(a_f.ped, &px, &py, &pz);
			if (veh && S::DOES_VEHICLE_EXIST(veh)) {
				S::GET_CAR_HEADING(veh, &r.heading);
				if (seatLoggedFor != veh) {
					seatLoggedFor = veh;
					float cx = 0, cy = 0, cz = 0, ground = 0;
					S::GET_CAR_COORDINATES(veh, &cx, &cy, &cz);
					S::GET_GROUND_Z_FOR_3D_COORD(px, py, pz, &ground);
					LC_LOG("in vehicle %d: ped at %.2f %.2f %.2f, car at %.2f %.2f %.2f (heading %.1f), ground %.2f: ped is %.2f m above the car origin, "
						   "%.2f m above the ground; the rider's feet go %.2f m below the ped (VehicleSeatDrop)",
						veh, px, py, pz, cx, cy, cz, r.heading, ground, pz - cz, pz - ground, Cfg().vehicleSeatDrop);
				}
			}
			r.seatFeet = { px, py, pz - Cfg().vehicleSeatDrop };
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
