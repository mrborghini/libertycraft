// Unity-built into dllmain.cpp (needs IV-SDK). See PauseMenu.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "pause"
#include "PauseMenu.h"

#include "Config.h"
#include "Game.h"
#include "Log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace lc::PauseMenu
{
	namespace
	{
		namespace S = ::Scripting;

		// ---- GTA IV 1.0.8.0's frontend --------------------------------------------------------------
		// The pause menu draws its map, tabs and backgrounds from the "frontend" texture dictionary:
		// opening it (0x47BFA0) finds or creates that slot in the texture dictionary store (0x12291E0: +0
		// the slots, +4 a used/free flag byte each, +8 the count, +C the slot size; a slot is +0 the
		// dictionary, +4 its reference count, +8 its name hash), loads it and takes a reference; closing
		// it (0x47B5C0) drops the reference and releases the dictionary (0x47C0C0). When the load fails
		// the slot keeps its reference but no dictionary: the map page is white but for its blips, and
		// closing the menu releases nothing, a write to 0x0000000C at 0x8CC398 that ends the game (what
		// players saw after Combat asked GET_DRIVER_OF_CAR every frame, see Combat.cpp Occupied). The
		// test hook logs that state as EMPTY. 0xF2DFC8 is the frontend's current screen (1 the map page).
		constexpr std::uintptr_t kTxdStore = 0x12291E0;
		constexpr std::uintptr_t kScreen = 0xF2DFC8;
		constexpr std::uintptr_t kHashFn = 0x8CFBB0;  // atStringHash wrapper (cdecl, const char*)

		bool frontendKnown = false;
		std::uint32_t frontendHash = 0;

		template <class T>
		bool Read(std::uintptr_t a_addr, T& a_out)
		{
			__try {
				a_out = *reinterpret_cast<const volatile T*>(a_addr);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool CodeMatches(std::uintptr_t a_addr, const std::uint8_t* a_bytes, std::size_t a_n)
		{
			for (std::size_t i = 0; i < a_n; ++i) {
				std::uint8_t b = 0;
				if (!Read(a_addr + i, b) || b != a_bytes[i]) {
					return false;
				}
			}
			return true;
		}

		void InitFrontend()
		{
			static bool done = false;
			if (done) {
				return;
			}
			done = true;
			// 0x47C0EB: push "frontend" (0xE5BA5C) in the pause menu's release; 0x8CFBB0: the hash's first bytes.
			static constexpr std::uint8_t kPush[] = { 0x68, 0x5C, 0xBA, 0xE5, 0x00 };
			static constexpr std::uint8_t kHash[] = { 0x8B, 0x44, 0x24, 0x04, 0x6A, 0x00, 0x50, 0xE8 };
			if (!CodeMatches(0x47C0EB, kPush, sizeof(kPush)) || !CodeMatches(kHashFn, kHash, sizeof(kHash))) {
				LC_LOG("frontend: not the expected GTA IV 1.0.8.0 code; its state isn't logged");
				return;
			}
			frontendHash = reinterpret_cast<std::uint32_t(__cdecl*)(const char*)>(kHashFn)("frontend");
			frontendKnown = true;
		}

		struct FrontendState
		{
			std::uint32_t screen = 0;
			int           slot = -1;  // the "frontend" dictionary's slot, -1 none
			std::uint32_t dict = 0;
			int           refs = 0;
			bool operator==(const FrontendState& a_o) const { return screen == a_o.screen && slot == a_o.slot && dict == a_o.dict && refs == a_o.refs; }
		};

		FrontendState ReadFrontend()
		{
			FrontendState f;
			if (!frontendKnown) {
				return f;
			}
			Read(kScreen, f.screen);
			std::uintptr_t store = 0, slots = 0, flags = 0;
			int            count = 0, size = 0;
			if (!Read(kTxdStore, store) || !store || !Read(store, slots) || !Read(store + 4, flags) || !Read(store + 8, count) || !Read(store + 12, size)) {
				return f;
			}
			for (int i = 0; i < count && i < 65536; ++i) {
				std::uint8_t flag = 0x80;
				std::uint32_t hash = 0;
				if (Read(flags + i, flag) && !(flag & 0x80) && Read(slots + std::uintptr_t(i) * size + 8, hash) && hash == frontendHash) {
					f.slot = i;
					Read(slots + std::uintptr_t(i) * size, f.dict);
					Read(slots + std::uintptr_t(i) * size + 4, f.refs);
					break;
				}
			}
			return f;
		}

		// The 32-bit address space: free in all and the largest free block (MiB).
		void AddressSpace(double& a_free, double& a_largest)
		{
			SIZE_T                   total = 0, largest = 0;
			std::uintptr_t           p = 0;
			MEMORY_BASIC_INFORMATION mbi{};
			while (::VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
				if (mbi.State == MEM_FREE) {
					total += mbi.RegionSize;
					largest = mbi.RegionSize > largest ? mbi.RegionSize : largest;
				}
				const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				if (next <= p) {
					break;
				}
				p = next;
			}
			a_free = double(total) / 1048576.0;
			a_largest = double(largest) / 1048576.0;
		}

		void LogState(const char* a_what)
		{
			const FrontendState f = ReadFrontend();
			double              freeMiB = 0, largestMiB = 0;
			AddressSpace(freeMiB, largestMiB);
			LC_LOG("%s: frontend screen 0x%X, its texture dictionary %s (slot %d, dictionary %08X, %d refs); address space free %.0f MiB (largest block %.1f MiB)",
				a_what, f.screen, f.slot < 0 ? "not loaded" : f.dict ? "loaded" : "EMPTY", f.slot, f.dict, f.refs, freeMiB, largestMiB);
		}

		std::atomic<bool> inGame{ false }, seated{ false };  // the last Tick's view (DebugPauseMenu's thread reads it)

		// ---- DebugPauseMenu ---------------------------------------------------------------------------
		void SendEsc()
		{
			INPUT in{};
			in.type = INPUT_KEYBOARD;
			in.ki.wScan = 0x01;  // Esc
			in.ki.dwFlags = KEYEVENTF_SCANCODE;
			::SendInput(1, &in, sizeof(in));
			::Sleep(120);
			in.ki.dwFlags |= KEYEVENTF_KEYUP;
			::SendInput(1, &in, sizeof(in));
		}

		bool WaitMenu(bool a_open, DWORD a_ms)
		{
			const ULONGLONG end = ::GetTickCount64() + a_ms;
			while (::GetTickCount64() < end) {
				if (Game::State().gtaMenuOpen.load(std::memory_order_relaxed) == a_open) {
					return true;
				}
				::Sleep(10);
			}
			return false;
		}

		DWORD WINAPI WatchFrontend(LPVOID a_stop)
		{
			FrontendState last;
			last.screen = 0xFFFFFFFF;
			while (!static_cast<std::atomic<bool>*>(a_stop)->load()) {
				const FrontendState f = ReadFrontend();
				if (!(f == last)) {
					LC_LOG("frontend: screen 0x%X, \"frontend\" texture dictionary slot %d, dictionary %08X, %d refs", f.screen, f.slot, f.dict, f.refs);
					last = f;
				}
				::Sleep(2);
			}
			return 0;
		}

		DWORD WINAPI DebugThread(LPVOID)
		{
			const auto& cfg = Config::Get();
			const DWORD readyMs = static_cast<DWORD>(cfg.debugPauseMenu * 1000.0f);
			auto&       st = Game::State();
			// The frontend's state, logged on every change while the test runs.
			std::atomic<bool> stop{ false };
			HANDLE watcher = ::CreateThread(nullptr, 0, &WatchFrontend, &stop, 0, nullptr);
			for (int cycle = 1; cycle <= cfg.debugPauseMenuCycles; ++cycle) {
				// Ready: in game (with DebugDriveWander: seated, the AI driving) and the game's window in front
				// (SendInput goes to the foreground window), for DebugPauseMenu s.
				ULONGLONG since = 0;
				for (;;) {
					DWORD      fgPid = 0;
					const HWND fg = ::GetForegroundWindow();
					if (fg) {
						::GetWindowThreadProcessId(fg, &fgPid);
					}
					const bool ready = st.linkReady.load() && !st.gtaMenuOpen.load() && inGame.load() &&
					                   (cfg.debugDriveWander > 0.0f ? seated.load() : true) && fgPid == ::GetCurrentProcessId();
					const ULONGLONG now = ::GetTickCount64();
					since = ready ? (since ? since : now) : 0;
					if (since && now - since >= readyMs) {
						break;
					}
					::Sleep(50);
				}
				const char* where = seated.load() ? "seated in a vehicle" : "on foot";
				LogState("DebugPauseMenu: before");
				LC_LOG("DebugPauseMenu: cycle %d of %d (%s): Esc opens GTA's pause menu", cycle, cfg.debugPauseMenuCycles, where);
				SendEsc();
				if (!WaitMenu(true, 3000)) {
					LC_LOG("DebugPauseMenu: the menu didn't open within 3 s");
					continue;
				}
				::Sleep(6000);
				LogState("DebugPauseMenu: SCREENSHOT the map page");
				::Sleep(2500);
				LC_LOG("DebugPauseMenu: cycle %d: Esc closes it", cycle);
				SendEsc();
				const bool closed = WaitMenu(false, 3000);
				LogState(closed ? "DebugPauseMenu: back in game" : "DebugPauseMenu: the menu didn't close within 3 s");
				::Sleep(3000);
				LogState("DebugPauseMenu: 3 s after closing");
			}
			LC_LOG("DebugPauseMenu: done");
			::Sleep(500);
			stop = true;
			if (watcher) {
				::WaitForSingleObject(watcher, 1000);
				::CloseHandle(watcher);
			}
			return 0;
		}

		// ---- DebugDriveWander -------------------------------------------------------------------------
		// On foot for 4 s (puppet mode or not, so it runs without Minecraft too): an Admiral is parked on
		// the nearest road node and the player is put in it; then GTA's AI drives it through the city.
		int   wanderCar = 0;
		float wanderT = 0.0f, wanderLogT = 0.0f, onFootT = 0.0f;
		bool  modelRequested = false;

		void DriveWander(const Frame& a_f)
		{
			const float speed = Config::Get().debugDriveWander;
			if (speed <= 0.0f || !a_f.exists || a_f.loading || a_f.paused) {
				return;
			}
			if (!a_f.inCar) {
				wanderCar = 0;
				if ((onFootT += a_f.dt) < 4.0f) {
					return;
				}
				const unsigned model = S::GET_HASH_KEY("admiral");
				if (!modelRequested) {
					modelRequested = true;
					CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
				}
				if (!S::HAS_MODEL_LOADED(model)) {
					return;
				}
				float x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, heading = 0;
				S::GET_CHAR_COORDINATES(a_f.ped, &x, &y, &z);
				if (!S::GET_CLOSEST_CAR_NODE_WITH_HEADING(x, y, z, &nx, &ny, &nz, &heading)) {
					nx = x, ny = y, nz = z;
				}
				int car = 0;
				S::CREATE_CAR(model, nx, ny, nz + 0.5f, &car, true);
				S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
				modelRequested = false;
				onFootT = 0.0f;
				if (car) {
					S::SET_CAR_HEADING(car, heading);
					S::WARP_CHAR_INTO_CAR(a_f.ped, car);
					S::MARK_CAR_AS_NO_LONGER_NEEDED(&car);
					LC_LOG("DebugDriveWander: the player put in an Admiral on the road at GTA %.0f %.0f %.0f (%.0f m from him)", nx, ny, nz,
						std::sqrt((nx - x) * (nx - x) + (ny - y) * (ny - y)));
				}
				return;
			}
			onFootT = 0.0f;
			int veh = 0;
			S::GET_CAR_CHAR_IS_USING(a_f.ped, &veh);
			if (!veh || !S::DOES_VEHICLE_EXIST(veh)) {
				return;
			}
			wanderT -= a_f.dt;
			if (veh != wanderCar || wanderT <= 0.0f) {
				// Style 2 drives on through red lights and round traffic.
				S::TASK_CAR_DRIVE_WANDER(a_f.ped, veh, speed, 2);
				S::SET_DRIVE_TASK_CRUISE_SPEED(a_f.ped, speed);
				if (veh != wanderCar) {
					LC_LOG("DebugDriveWander: GTA's AI drives the player's vehicle %d through the city at up to %.0f m/s", veh, speed);
				}
				wanderCar = veh;
				wanderT = 10.0f;
			}
			if ((wanderLogT -= a_f.dt) <= 0.0f) {
				wanderLogT = 5.0f;
				float v = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
				S::GET_CAR_SPEED(veh, &v);
				S::GET_CAR_COORDINATES(veh, &x, &y, &z);
				const auto* peds = CPools::ms_pPedPool;
				const auto* cars = CPools::ms_pVehiclePool;
				LC_LOG("DebugDriveWander: %.1f m/s at GTA %.0f %.0f %.0f; peds %u, vehicles %u", v, x, y, z, peds ? peds->m_nUsed : 0u, cars ? cars->m_nUsed : 0u);
			}
		}
	}

	void Tick(const Frame& a_f)
	{
		static bool started = false;
		if (!started && Config::Get().debugPauseMenu > 0.0f) {
			started = true;
			InitFrontend();
			if (HANDLE h = ::CreateThread(nullptr, 0, &DebugThread, nullptr, 0, nullptr)) {
				::CloseHandle(h);
				LC_LOG("DebugPauseMenu: GTA's pause menu opens and closes (real Esc key events) %d times, %.0f s after the player is in game each time",
					Config::Get().debugPauseMenuCycles, Config::Get().debugPauseMenu);
			}
		}
		inGame = a_f.exists && !a_f.loading && !a_f.paused;
		seated = a_f.exists && a_f.inCar;
		DriveWander(a_f);
	}
}
