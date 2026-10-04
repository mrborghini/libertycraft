// Unity-built into dllmain.cpp (needs IV-SDK). See Hazards.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "hazard"
#include "Hazards.h"

#include "hazard/HazardGrid.h"

#include "Combat.h"
#include "Config.h"
#include "Coords.h"
#include "HostDrive.h"
#include "Log.h"
#include "NpcBlocks.h"

#include <atomic>
#include <intrin.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lc::Hazards
{
	namespace
	{
		namespace S = ::Scripting;
		namespace proto = ::libertycraft::proto;

		constexpr float  kTick = 0.1f;           // seconds between checks
		constexpr float  kRange = 80.0f;         // peds this close to the player
		constexpr double kPedRadius = 0.3;       // a ped's body around its position (blocks)
		constexpr double kPedHeight = 1.8;
		constexpr std::size_t kMaxPending = 4096;

		const Config& Cfg() { return Config::Get(); }

		// render thread -> game thread
		struct PendingLights
		{
			std::int32_t                 sx = 0, sy = 0, sz = 0;
			std::vector<proto::RenLight> lights;
		};
		struct PendingLiquids
		{
			std::int32_t                  sx = 0, sy = 0, sz = 0;
			std::vector<proto::RenLiquid> liquids;
		};
		std::mutex                  pendingLock;
		std::vector<PendingLights>  pending;
		std::vector<PendingLiquids> pendingLiquids;
		bool                       pendingClear = false;
		std::uint32_t              pendingDropped = 0;

		hazard::Grid       grid;     // game thread
		hazard::LiquidGrid liquids;  // game thread
		std::size_t        loggedLiquids = 0;

		// Vehicles in Minecraft's blocks (by handle).
		struct VehicleState
		{
			float lastSpeed = -1.0f;  // horizontal speed we left it with last frame (-1: none)
			hazard::Stall stall;      // its engine in deep water
			float logT = 0.0f;
			float groundT = 0.0f;    // seconds until the ground under it is probed again
			float ground = 0.0f;     // GTA's ground under it (GTA z)
			bool  drowned = false, lit = false, seen = false, haveGround = false, wet = false, wreckLogged = false;
			// DebugHazards: where it last stood out of Minecraft's liquids (the player's car pulled back there
			// once stalled, to see its engine start again)
			float dry[4] = {};  // x, y, z, heading
			bool  haveDry = false;
			int   pulls = 0;
			float restartLog = 0.0f;  // the player's: s left logging its engine after it started again
		};
		std::unordered_map<int, VehicleState> vehicles;
		int velocityWorks = -1;  // the physics collider's SetVelocity: -1 unknown, 1 takes, 0 doesn't (SET_CAR_FORWARD_SPEED)
		hazard::Tuning tuning;
		std::unordered_map<int, hazard::Burn> burning;  // ped handle -> its burn
		std::unordered_map<int, float>        wading;   // ped handle -> the move speed share we gave it in water
				float                                 tickT = 0.0f;
		std::size_t                           loggedCells = 0;

		struct Counters
		{
			std::uint32_t ignited = 0, hurtPeds = 0, health = 0, putOut = 0, died = 0, cars = 0, igniteFailed = 0, carsScorched = 0, carFrames = 0, stalled = 0,
			              drowned = 0, wading = 0;
			float         slowedBy = 0.0f;  // m/s taken off, summed over vehicle frames
		} counters;
		std::uint64_t nextStats = 0;

		// ---- Minecraft water as GTA's water ----------------------------------------------------------------
		// GTA IV 1.0.8.0's water level query (0x9AB6C0: x, y, z, float* height, then three it passes on;
		// returns whether there's water there): behind GET_WATER_HEIGHT and the game's own checks
		// (swimming, buoyancy, splashes, drowning; 10 callers, 32 more through 0x9B0410). Hooked, it
		// answers Minecraft's water surface in columns that hold Minecraft water (the published
		// WaterColumns, rebuilt on the game thread when the water changes; old copies kept a while for
		// readers on other threads), else asks the game. Not around the puppeted player: Minecraft swims
		// and drowns its own player.
		using WaterLevelFn = bool(__cdecl*)(float, float, float, float*, std::uint32_t, std::uint32_t, std::uint32_t);  // (the last three passed on bit for bit)
		WaterLevelFn                                       waterOriginal = nullptr;
		int                                                waterHook = -1;  // -1 not tried, 0 off/failed, 1 on
		std::atomic<const hazard::WaterColumns*>           waterPublished{ nullptr };
		std::vector<std::unique_ptr<hazard::WaterColumns>> waterKeep;  // the published one and a few before it
		std::atomic<bool>                                  waterPuppet{ false };
		std::atomic<float>                                 waterPuppetX{ 0.0f }, waterPuppetY{ 0.0f };
		std::atomic<std::uint32_t>                         waterHits{ 0 }, waterCalls{ 0 };
		// DebugHazards: who asks (return addresses), the answered ones and all.
		constexpr int                                      kCallerSlots = 64;
		std::atomic<std::uintptr_t>                        callerAddr[kCallerSlots]{};
		std::atomic<std::uint32_t>                         callerHits[kCallerSlots]{}, callerCalls[kCallerSlots]{};
		float                                              callerLast[kCallerSlots][4]{};  // (racy; diagnostics) x, y, z, and what it got

		void NoteCaller(std::uintptr_t a_ret, bool a_hit, float a_x, float a_y, float a_z, float a_h)
		{
			const int start = static_cast<int>((a_ret >> 2) % kCallerSlots);
			for (int k = 0; k < kCallerSlots; ++k) {
				auto&          slot = callerAddr[(start + k) % kCallerSlots];
				std::uintptr_t cur = slot.load(std::memory_order_relaxed);
				if (cur == 0 && slot.compare_exchange_strong(cur, a_ret)) {
					cur = a_ret;
				}
				if (cur == a_ret) {
					float* last = callerLast[(start + k) % kCallerSlots];
					last[0] = a_x, last[1] = a_y, last[2] = a_z, last[3] = a_h;
					callerCalls[(start + k) % kCallerSlots].fetch_add(1, std::memory_order_relaxed);
					if (a_hit) {
						callerHits[(start + k) % kCallerSlots].fetch_add(1, std::memory_order_relaxed);
					}
					return;
				}
			}
		}

		// Return addresses (1.0.8.0) of the query's script callers: GET_WATER_HEIGHT (through 0x9B0410,
		// which returns to 0x9B044C; its own caller, the native, at 0xB0A5DB) and
		// GET_WATER_HEIGHT_NO_WAVES (0xB0A747). Scripts, and LibertyCraft's own collision streaming
		// (collision/Water.h: GTA's water for Minecraft), keep seeing GTA's own water: else Minecraft's
		// water would come back to Minecraft as GTA water and stay there after it's gone.
		constexpr std::uintptr_t kViaLevel = 0x9B044C, kScriptLevel = 0xB0A5DB, kScriptNoWaves = 0xB0A747;

		bool __cdecl WaterLevel(float a_x, float a_y, float a_z, float* a_out, std::uint32_t a_a, std::uint32_t a_b, std::uint32_t a_c)
		{
			waterCalls.fetch_add(1, std::memory_order_relaxed);
			const auto  base = static_cast<std::uintptr_t>(AddressSetter::gBaseAddress) - 0x400000;
			auto        ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - base;
			if (ret == kViaLevel) {
				// 0x9B0410's caller: past its 7 arguments to us and its saved esi.
				ret = *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::uint8_t*>(_AddressOfReturnAddress()) + 0x24) - base;
			}
			const bool script = ret == kScriptLevel || ret == kScriptNoWaves;
			if (const auto* w = waterPublished.load(std::memory_order_acquire); w && a_out && !script) {
				const bool player = waterPuppet.load(std::memory_order_relaxed) && std::fabs(a_x - waterPuppetX.load(std::memory_order_relaxed)) < 1.0f &&
				                    std::fabs(a_y - waterPuppetY.load(std::memory_order_relaxed)) < 1.0f;
				float s = 0.0f;
				const bool hit = !player && w->Surface(a_x, a_y, a_z, s);
				if (Cfg().debugHazards) {
					NoteCaller(ret, hit, a_x, a_y, a_z, hit ? s : -1.0f);
				}
				if (hit) {
					*a_out = s;
					waterHits.fetch_add(1, std::memory_order_relaxed);
					return true;
				}
			}
			return waterOriginal(a_x, a_y, a_z, a_out, a_a, a_b, a_c);
		}

		void HookWaterLevel()
		{
			waterHook = 0;
			if (!Cfg().minecraftWaterIsGtaWater) {
				return;
			}
			if (plugin::gameVer != plugin::VERSION_1080) {
				LC_LOG("water: not GTA IV 1.0.8.0: Minecraft water stays Minecraft's only");
				return;
			}
			auto* at = reinterpret_cast<std::uint8_t*>(AddressSetter::gBaseAddress) + (0x9AB6C0 - 0x400000);
			// movss xmm6, [abs32] (8 bytes, the address relocated with the image); movss xmm1, [esp+4]
			if (!(at[0] == 0xF3 && at[1] == 0x0F && at[2] == 0x10 && at[3] == 0x35 && at[8] == 0xF3 && at[9] == 0x0F && at[10] == 0x10 && at[11] == 0x4C &&
					at[12] == 0x24 && at[13] == 0x04)) {
				LC_LOG("water: GTA's water level query isn't as expected: Minecraft water stays Minecraft's only");
				return;
			}
			auto* tramp = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!tramp) {
				LC_LOG("water: can't allocate the trampoline (error %lu)", ::GetLastError());
				return;
			}
			const auto abs32 = [](const void* a_p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_p)); };
			std::memcpy(tramp, at, 8);  // the first instruction (absolute address: copies as is)
			tramp[8] = 0xE9;
			const std::uint32_t back = abs32(at + 8) - (abs32(tramp) + 13u);
			std::memcpy(tramp + 9, &back, 4);
			::FlushInstructionCache(::GetCurrentProcess(), tramp, 13);
			waterOriginal = reinterpret_cast<WaterLevelFn>(tramp);
			std::uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
			const std::uint32_t to = abs32(reinterpret_cast<const void*>(&WaterLevel)) - (abs32(at) + 5u);
			std::memcpy(patch + 1, &to, 4);
			DWORD old = 0;
			if (!::VirtualProtect(at, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
				LC_LOG("water: can't hook GTA's water level query (VirtualProtect error %lu)", ::GetLastError());
				return;
			}
			std::memcpy(at, patch, sizeof(patch));
			::VirtualProtect(at, sizeof(patch), old, &old);
			::FlushInstructionCache(::GetCurrentProcess(), at, sizeof(patch));
			waterHook = 1;
			LC_LOG("water: GTA's water level query hooked: Minecraft water is GTA water (peds swim, vehicles float)");
		}

		// The game thread, when Minecraft's water changed: a new lookup for the hook.
		void PublishWater()
		{
			if (waterHook != 1) {
				return;
			}
			auto next = std::make_unique<hazard::WaterColumns>();
			next->Build(liquids);
			waterPublished.store(next->Empty() ? nullptr : next.get(), std::memory_order_release);
			waterKeep.push_back(std::move(next));
			while (waterKeep.size() > 8) {
				waterKeep.erase(waterKeep.begin());  // (8 changes ago: no reader still has it)
			}
		}

		// Test hooks
		float debugT = 0.0f;
		bool  debugRelocated = false;
		struct Injected
		{
			std::int32_t x, y, z;
		};
		std::vector<Injected> injected;
		float                 injectedLeft = 0.0f;

		void ApplyPending()
		{
			std::vector<PendingLights>  take;
			std::vector<PendingLiquids> takeLiquids;
			bool                        clear = false;
			std::uint32_t               dropped = 0;
			{
				std::lock_guard lock(pendingLock);
				take.swap(pending);
				takeLiquids.swap(pendingLiquids);
				clear = pendingClear;
				pendingClear = false;
				dropped = pendingDropped;
				pendingDropped = 0;
			}
			if (clear) {
				grid.Clear();
				liquids.Clear();
				injected.clear();
				loggedCells = loggedLiquids = 0;
			}
			for (const auto& p : takeLiquids) {
				liquids.SetSection(p.sx, p.sy, p.sz, p.liquids.data(), static_cast<std::uint32_t>(p.liquids.size()));
			}
			if (clear || !takeLiquids.empty()) {
				PublishWater();
			}
			if (!liquids.Empty() && (loggedLiquids == 0 || liquids.Cells() >= loggedLiquids * 2)) {
				LC_LOG("Minecraft water and lava: %zu blocks", liquids.Cells());
				loggedLiquids = liquids.Cells();
			} else if (loggedLiquids && liquids.Cells() * 2 <= loggedLiquids) {
				LC_LOG("Minecraft water and lava: down to %zu blocks", liquids.Cells());
				loggedLiquids = liquids.Cells();
			}
			for (const auto& p : take) {
				grid.SetSection(p.sx, p.sy, p.sz, p.lights.data(), static_cast<std::uint32_t>(p.lights.size()));
			}
			if (dropped) {
				LC_LOG("WARNING: %u light lists dropped (queue full)", dropped);
			}
			if (loggedCells && grid.Cells() * 2 <= loggedCells) {
				std::int32_t lo[3], hi[3];
				if (grid.Bounds(lo, hi)) {
					LC_LOG("Minecraft hazards: down to %zu blocks, MC %d %d %d to %d %d %d", grid.Cells(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
				} else {
					LC_LOG("Minecraft hazards: none left");
				}
				loggedCells = grid.Cells();
			}
			if (!grid.Empty() && (loggedCells == 0 || grid.Cells() >= loggedCells * 2)) {
				std::size_t n[4];
				grid.Count(n);
				std::int32_t lo[3], hi[3];
				grid.Bounds(lo, hi);
				LC_LOG("Minecraft hazards: %zu blocks in %zu sections (fire %zu, lava %zu, magma %zu), MC %d %d %d to %d %d %d", grid.Cells(), grid.Sections(),
					n[proto::kHazardFire], n[proto::kHazardLava], n[proto::kHazardMagma], lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
				loggedCells = grid.Cells();
			}
		}

		void HurtPed(int a_ped, unsigned a_damage)
		{
			unsigned before = 0, after = 0;
			S::GET_CHAR_HEALTH(a_ped, &before);
			S::DAMAGE_CHAR(a_ped, a_damage, false);
			S::GET_CHAR_HEALTH(a_ped, &after);
			if (after >= before && before > 0) {
				S::SET_CHAR_HEALTH(a_ped, before > a_damage ? before - a_damage : 0);  // (DAMAGE_CHAR doesn't always take)
			}
			++counters.hurtPeds;
			counters.health += a_damage;
		}

		const char* Name(std::uint8_t a_h)
		{
			return a_h == proto::kHazardLava ? "lava" : a_h == proto::kHazardFire ? "fire" : a_h == proto::kHazardMagma ? "magma" : "nothing";
		}

		// DebugHazardInject: a 3 x 3 patch of fire (or lava) under the nearest walking ped every 20 s,
		// for 8 s, in our grid only (no Minecraft blocks: tests the burning without Minecraft).
		void DebugInject(const Combat::Frame& a_f, const CVector& a_player)
		{
			const auto& kind = Cfg().debugHazardInject;
			if ((kind != "fire" && kind != "lava") || !a_f.puppeting) {
				return;
			}
			if (injectedLeft > 0.0f) {
				if ((injectedLeft -= kTick) <= 0.0f) {
					for (const auto& c : injected) {
						grid.SetCell(c.x, c.y, c.z, proto::kHazardNone);
					}
					injected.clear();
					LC_LOG("DebugHazardInject: patch removed");
				}
				return;
			}
			if ((debugT += kTick) < 20.0f) {
				return;
			}
			auto* pool = CPools::ms_pPedPool;
			CPed* player = FindPlayerPed();
			if (!pool) {
				return;
			}
			// The nearest one the camera looks at (for the screenshot), 4 to 25 m away.
			const CCam* cam = TheCamera.m_pFinalCam;
			CVector cpos = a_player, cfwd{ 0.0f, 1.0f, 0.0f };
			if (cam) {
				cpos = { cam->m_mMatrix.pos.x, cam->m_mMatrix.pos.y, cam->m_mMatrix.pos.z };
				cfwd = { cam->m_mMatrix.up.x, cam->m_mMatrix.up.y, cam->m_mMatrix.up.z };
			}
			int   best = 0;
			float bestD2 = 25.0f * 25.0f;
			CVector at{};
			for (int i = pool->FindNextUsed(0); i >= 0; i = pool->FindNextUsed(i + 1)) {
				CPed* p = pool->Get(i);
				if (!p || p == player || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float dx = m.x - cpos.x, dy = m.y - cpos.y, dz = m.z - cpos.z;
				const float d2 = dx * dx + dy * dy;
				const float ahead = (dx * cfwd.x + dy * cfwd.y + dz * cfwd.z) / std::max(0.1f, std::sqrt(d2 + dz * dz));
				const int   h = static_cast<int>(pool->GetIndex(p));
				if (d2 < bestD2 && d2 > 16.0f && ahead > 0.8f && S::DOES_CHAR_EXIST(h) && !S::IS_CHAR_DEAD(h) && !S::IS_CHAR_IN_ANY_CAR(h)) {
					bestD2 = d2;
					best = h;
					at = m;
				}
			}
			if (!best) {
				return;
			}
			debugT = 0.0f;
			const std::uint8_t h = kind == "lava" ? proto::kHazardLava : proto::kHazardFire;
			const McVec        feet = GtaToMc(at.x, at.y, at.z - Cfg().rootToFeet);
			const int          x0 = static_cast<int>(std::floor(feet.x)), y0 = static_cast<int>(std::floor(feet.y)), z0 = static_cast<int>(std::floor(feet.z));
			for (int dx = -1; dx <= 1; ++dx) {
				for (int dz = -1; dz <= 1; ++dz) {
					grid.SetCell(x0 + dx, y0, z0 + dz, h);
					injected.push_back({ x0 + dx, y0, z0 + dz });
				}
			}
			injectedLeft = 8.0f;
			LC_LOG("DebugHazardInject: 3 x 3 %s under ped %d (%.1f m ahead of the camera) at MC %d %d %d", Name(h), best, std::sqrt(bestD2), x0, y0, z0);
		}

		// DebugHazards: off to the street (out of Roman's flat) once, 15 s into puppet mode, where peds walk.
		void DebugToStreet(const Combat::Frame& a_f)
		{
			if (!Cfg().debugHazards || debugRelocated || !a_f.puppeting) {
				return;
			}
			if ((debugT += kTick) >= 15.0f) {
				debugRelocated = true;
				debugT = 0.0f;
				HostDrive::DebugRequestRoad("DebugHazards");
			}
		}

		unsigned CarHealth(int a_h)
		{
			unsigned health = 0;
			S::GET_CAR_HEALTH(a_h, &health);
			return health;
		}

		// DebugHazardInject=harbour / shore: GTA's own drowning timed (the reference for
		// hazard::StallTuning::after). 30 s into puppet mode a test car is made in GTA's water (harbour: 60 m
		// out from the nearest water's edge along 16 rays from the player, dropped in at 8 m/s) or on the
		// land 25 m before that edge, facing the water (shore: DebugDriveThrottle drives it in), with Niko in
		// it; for 60 s its engine, water and health are logged, with when the engine died after the car hit
		// the water.
		struct Harbour
		{
			int   stage = 0;  // 0 waiting, 1 model asked for, 2 logging, 3 done
			int   car = 0;
			float t = 0.0f, logT = 0.0f, inWaterAt = -1.0f, underAt = -1.0f, offAt = -1.0f, undriveableAt = -1.0f, outAt = -1.0f;
			float x = 0.0f, y = 0.0f, z = 0.0f, heading = 0.0f;
		} harbour;

		void DebugHarbour(const Combat::Frame& a_f, const CVector& a_player)
		{
			auto&      H = harbour;
			const bool shore = Cfg().debugHazardInject == "shore";
			if ((Cfg().debugHazardInject != "harbour" && !shore) || H.stage == 3) {
				return;
			}
			H.t += kTick;
			const unsigned model = S::GET_HASH_KEY("admiral");
			if (H.stage == 0) {
				if (H.t < 30.0f || !a_f.puppeting) {
					return;
				}
				float best = 1e9f;
				for (int k = 0; k < 16; ++k) {
					const float a = static_cast<float>(k) * (kPi / 8.0f), cx = std::cos(a), cy = std::sin(a);
					for (float d = 20.0f; d <= 1000.0f; d += 20.0f) {
						float w = 0.0f, w2 = 0.0f, w3 = 0.0f;
						if (!S::GET_WATER_HEIGHT(a_player.x + cx * d, a_player.y + cy * d, a_player.z + 2.0f, &w)) {
							continue;
						}
						if (d < best && S::GET_WATER_HEIGHT(a_player.x + cx * (d + 60.0f), a_player.y + cy * (d + 60.0f), a_player.z + 2.0f, &w2) &&
							S::GET_WATER_HEIGHT(a_player.x + cx * (d + 150.0f), a_player.y + cy * (d + 150.0f), a_player.z + 2.0f, &w3)) {
							best = d;
							const float at = shore ? d - 25.0f : d + 60.0f;
							H.x = a_player.x + cx * at, H.y = a_player.y + cy * at, H.z = w2;
							H.heading = std::atan2(-cx, cy) / kDegToRad;
							if (shore) {
								float g = -1000.0f;
								S::GET_GROUND_Z_FOR_3D_COORD(H.x, H.y, 120.0f, &g);
								H.z = g > -5.0f && g < 100.0f ? g : w2 + 3.0f;
							}
						}
						break;
					}
				}
				if (best > 1e8f) {
					LC_LOG("DebugHarbour: no open water within 1 km");
					H.stage = 3;
					return;
				}
				LC_LOG("DebugHarbour: open water %.0f m away; a test car %s at %.1f %.1f (%s %.2f), heading %.0f", best, shore ? "on the shore" : "in it", H.x, H.y,
					shore ? "ground" : "surface", H.z, H.heading);
				CStreaming::ScriptRequestModel(static_cast<std::int32_t>(model));
				H.stage = 1;
				return;
			}
			CPed* player = FindPlayerPed();
			if (!player || !CPools::ms_pPedPool) {
				return;
			}
			const int ped = static_cast<int>(CPools::ms_pPedPool->GetIndex(player));
			if (H.stage == 1) {
				if (!S::HAS_MODEL_LOADED(model)) {
					return;
				}
				S::CREATE_CAR(model, H.x, H.y, H.z + (shore ? 0.5f : 1.0f), &H.car, true);
				S::MARK_MODEL_AS_NO_LONGER_NEEDED(model);
				if (!H.car) {
					LC_LOG("DebugHarbour: CREATE_CAR failed");
					H.stage = 3;
					return;
				}
				S::SET_CAR_HEADING(H.car, H.heading);
				S::WARP_CHAR_INTO_CAR(ped, H.car);
				S::SET_CAR_ENGINE_ON(H.car, true, true);
				if (!shore) {
					S::SET_CAR_FORWARD_SPEED(H.car, 8.0f);
				}
				H.t = 0.0f;
				H.stage = 2;
				LC_LOG("DebugHarbour: test car %d %s with Niko in it, engine on", H.car, shore ? "on the shore" : "dropped into GTA's water at 8 m/s");
				return;
			}
			CVehicle* v = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAt(static_cast<std::uint32_t>(H.car)) : nullptr;
			if (!S::DOES_VEHICLE_EXIST(H.car) || !v || !v->m_pMatrix) {
				LC_LOG("DebugHarbour: the test car is gone at %.1f s", H.t);
				H.stage = 3;
				return;
			}
			const auto& p = v->m_pMatrix->pos;
			float       w = 0.0f;
			const bool  sea = S::GET_WATER_HEIGHT(p.x, p.y, p.z, &w);
			const bool  inWater = S::IS_CAR_IN_WATER(H.car), on = v->m_nVehicleFlags.bEngineOn, driveable = S::IS_VEH_DRIVEABLE(H.car);
			const bool  inside = S::IS_CHAR_IN_ANY_CAR(ped);
			float       speed = 0.0f;
			S::GET_CAR_SPEED(H.car, &speed);
			const auto note = [&](float& a_at, bool a_now, const char* a_what) {
				if (a_now && a_at < 0.0f) {
					a_at = H.t;
					LC_LOG("DebugHarbour: %.1f s: %s (%.1f s after it hit the water)", H.t, a_what, H.inWaterAt >= 0.0f ? H.t - H.inWaterAt : 0.0f);
				}
			};
			note(H.inWaterAt, inWater, "IS_CAR_IN_WATER");
			note(H.underAt, sea && p.z < w, "its centre is under the surface");
			note(H.offAt, H.inWaterAt >= 0.0f && !on, "its engine is off (GTA's own)");
			note(H.undriveableAt, !driveable, "not driveable");
			note(H.outAt, !inside, "Niko is out of it");
			if ((H.logT -= kTick) <= 0.0f) {
				H.logT = 0.5f;
				LC_LOG("DebugHarbour: %.1f s: in water %d, engine on %d (starting %d), driveable %d, engine %.0f, body %u, z %.2f (surface %.2f: %.2f under), "
				       "%.1f m/s, Niko inside %d%s",
					H.t, inWater, on, v->m_nVehicleFlags.bEngineStarting ? 1 : 0, driveable, v->m_fEngineHealth, CarHealth(H.car), p.z, w, sea ? w - p.z : 0.0f,
					speed, inside, S::IS_CAR_DEAD(H.car) ? ", WRECKED" : "");
			}
			if (H.t >= 60.0f) {
				LC_LOG("DebugHarbour: done: in the water at %.1f s; engine off %.1f s after (%.1f s after its centre went under); not driveable %.1f s after; "
				       "Niko out %.1f s after",
					H.inWaterAt, H.offAt >= 0.0f ? H.offAt - H.inWaterAt : -1.0f, H.offAt >= 0.0f && H.underAt >= 0.0f ? H.offAt - H.underAt : -1.0f,
					H.undriveableAt >= 0.0f ? H.undriveableAt - H.inWaterAt : -1.0f, H.outAt >= 0.0f ? H.outAt - H.inWaterAt : -1.0f);
				H.stage = 3;
			}
		}


		bool NearSections(const hazard::Grid& a_g, const hazard::LiquidGrid& a_l, double a_x, double a_y, double a_z)
		{
			const int sx = static_cast<int>(std::floor(a_x / 16.0)), sy = static_cast<int>(std::floor(a_y / 16.0)), sz = static_cast<int>(std::floor(a_z / 16.0));
			for (int dx = -1; dx <= 1; ++dx) {
				for (int dy = -1; dy <= 1; ++dy) {
					for (int dz = -1; dz <= 1; ++dz) {
						if (a_g.HasSection(sx + dx, sy + dy, sz + dz) || a_l.HasSection(sx + dx, sy + dy, sz + dz)) {
							return true;
						}
					}
				}
			}
			return false;
		}

		void SetHorizontalSpeed(CVehicle* a_v, int a_h, const CVector& a_vel, float a_scale)
		{
			CVector nv{ a_vel.x * a_scale, a_vel.y * a_scale, a_vel.z };
			if (velocityWorks != 0) {
				if (auto* collider = a_v->GetConstrainedCollider()) {
					collider->SetVelocity(&nv);
					if (velocityWorks == -1) {
						CVector back{};
						a_v->GetVelocity(&back);
						velocityWorks = std::hypot(back.x - nv.x, back.y - nv.y) < 0.05f + 0.1f * std::hypot(nv.x - a_vel.x, nv.y - a_vel.y) ? 1 : 0;
						LC_LOG("liquid drag through the vehicle's physics collider: %s", velocityWorks ? "works" : "doesn't take, using SET_CAR_FORWARD_SPEED");
					}
					if (velocityWorks == 1) {
						return;
					}
				}
			}
			const auto& fwd = a_v->m_pMatrix->up;  // IV-SDK's "up" row is the forward axis
			S::SET_CAR_FORWARD_SPEED(a_h, nv.x * fwd.x + nv.y * fwd.y + nv.z * fwd.z);
		}

		// A vehicle's engine in Minecraft water (hazard::StallStep): over a metre deep it runs on for a few
		// seconds, as in GTA's own water, then dies and is held off (every frame: GTA would start it again
		// under the throttle) while it's that deep; out of it, it starts again. Never through engine
		// health: at 0 or below GTA starts an engine fire and the car blows up, and water must never burn
		// a car.
		void EngineInWater(VehicleState& a_st, int a_h, bool a_water, float a_depth, float a_dt, bool a_mine)
		{
			const char* who = a_mine ? "the player's vehicle" : "vehicle";
			switch (hazard::StallStep(a_st.stall, a_water, a_depth, a_dt)) {
			case hazard::StallEvent::kDeep:
				LC_LOG("%s %d: %.1f m into Minecraft water, its engine runs on (for %.0f s under)", who, a_h, a_depth, hazard::StallTuning{}.after);
				break;
			case hazard::StallEvent::kStalls:
				++counters.stalled;
				LC_LOG("%s %d: %.1f s under %.1f m of Minecraft water, the engine stalls", who, a_h, a_st.stall.under, a_depth);
				S::SET_CAR_ENGINE_ON(a_h, false, true);
				break;
			case hazard::StallEvent::kOff:
				S::SET_CAR_ENGINE_ON(a_h, false, true);
				if (a_st.stall.under > hazard::StallTuning{}.after + 4.0f && !a_st.drowned) {
					a_st.drowned = true;
					++counters.drowned;
					LC_LOG("%s %d is drowned in Minecraft water: its engine stays off while it's under", who, a_h);
				}
				break;
			case hazard::StallEvent::kRestarts:
				S::SET_CAR_ENGINE_ON(a_h, true, false);
				a_st.drowned = false;
				a_st.restartLog = a_mine ? 3.0f : 0.0f;
				LC_LOG("%s %d is out of deep Minecraft water: its engine starts again", who, a_h);
				break;
			case hazard::StallEvent::kNone:
				break;
			}
		}

		// Every frame: vehicles in fire, lava, magma or water.
		void Vehicles(float a_dt, const CVector& a_here)
		{
			const bool burn = Cfg().hazardsBurnVehicles && !grid.Empty(), slow = Cfg().liquidsSlowVehicles && !liquids.Empty();
			auto*      pool = CPools::ms_pVehiclePool;
			if ((!burn && !slow) || !pool) {
				vehicles.clear();
				return;
			}
			for (auto& [h, st] : vehicles) {
				st.seen = false;
			}
			CPed* player = FindPlayerPed();
			int   playerCar = 0;
			if (player && S::IS_CHAR_IN_ANY_CAR(static_cast<int>(CPools::ms_pPedPool->GetIndex(player)))) {
				S::GET_CAR_CHAR_IS_USING(static_cast<int>(CPools::ms_pPedPool->GetIndex(player)), &playerCar);
			}
			for (int i = pool->FindNextUsed(0); i >= 0; i = pool->FindNextUsed(i + 1)) {
				CVehicle* v = pool->Get(i);
				if (!v || !v->m_pMatrix) {
					continue;
				}
				const auto& m = *v->m_pMatrix;
				if ((m.pos.x - a_here.x) * (m.pos.x - a_here.x) + (m.pos.y - a_here.y) * (m.pos.y - a_here.y) > kRange * kRange) {
					continue;
				}
				if (!NearSections(grid, liquids, m.pos.x, m.pos.z, -m.pos.y)) {
					continue;
				}
				const int h = static_cast<int>(pool->GetIndex(v));
				hazard::Footprint f;
				f.pos[0] = m.pos.x, f.pos[1] = m.pos.y, f.pos[2] = m.pos.z;
				f.right[0] = m.right.x, f.right[1] = m.right.y, f.right[2] = m.right.z;
				f.fwd[0] = m.up.x, f.fwd[1] = m.up.y, f.fwd[2] = m.up.z;  // IV-SDK's rows: "up" = forward, "at" = up
				f.up[0] = m.at.x, f.up[1] = m.at.y, f.up[2] = m.at.z;
				NpcBlocks::ModelBox(h, v->m_nModelIndex, f.lo, f.hi);  // (a default box if not known)
				// GTA's ground under it (the box's bottom is the body): probed 4 times a second.
				auto& st0 = vehicles[h];
				st0.seen = true;  // (keeps its ground probe while near Minecraft's blocks)
				if (slow && ((st0.groundT -= a_dt) <= 0.0f || !st0.haveGround)) {
					st0.groundT = 0.25f;
					float gz = -1000.0f;
					const float top = static_cast<float>(m.pos.z) + f.hi[2];
					S::GET_GROUND_Z_FOR_3D_COORD(m.pos.x, m.pos.y, top, &gz);
					st0.haveGround = gz > top - 6.0f && gz < top;
					st0.ground = gz;
				}
				if (st0.haveGround) {
					f.ground = st0.ground;
				}
				static const hazard::Grid       kNoHazards;
				static const hazard::LiquidGrid kNoLiquids;
				const auto c = hazard::Touch(burn ? grid : kNoHazards, slow ? liquids : kNoLiquids, f);  // (references: no copies)
				if (c.hazard == proto::kHazardNone && c.liquid == proto::kLiquidNone) {
					st0.lastSpeed = -1.0f;  // out of it: no push to hold back when it comes in again
					EngineInWater(st0, h, false, 0.0f, a_dt, h == playerCar);
					if (st0.restartLog > 0.0f) {
						const float before = st0.restartLog;
						st0.restartLog -= a_dt;
						if (std::floor(before * 2.0f) != std::floor(st0.restartLog * 2.0f)) {
							float speed = 0.0f;
							S::GET_CAR_SPEED(h, &speed);
							LC_LOG("the player's vehicle %d out of the water: engine %s, %.1f m/s", h, v->m_nVehicleFlags.bEngineOn ? "running" : "off", speed);
						}
					}
					if (Cfg().debugHazards && h == playerCar) {
						st0.dry[0] = m.pos.x, st0.dry[1] = m.pos.y, st0.dry[2] = m.pos.z;
						S::GET_CAR_HEADING(h, &st0.dry[3]);
						st0.haveDry = true;
					}
					continue;
				}
				if (!S::DOES_VEHICLE_EXIST(h) || S::IS_CAR_DEAD(h)) {
					if (st0.wet && !st0.lit && !st0.wreckLogged) {
						st0.wreckLogged = true;  // (diagnostics: water must never wreck a car)
						LC_LOG("WARNING: vehicle %d was wrecked in Minecraft water (no fire or lava under it)", h);
					}
					continue;
				}
				auto& st = vehicles[h];
				st.wet = c.liquid == proto::kLiquidWater;
				st.seen = true;
				const bool mine = h == playerCar;
				// ---- fire, lava, magma
				const bool lava = c.hazard == proto::kHazardLava || c.liquid == proto::kLiquidLava;
				if (burn && lava && !st.lit) {
					st.lit = true;
					NativeInvoke::Invoke<int>(NATIVE_START_CAR_FIRE, h);
					S::SET_ENGINE_HEALTH(h, -100.0f);
					++counters.cars;
					LC_LOG("%s %d in Minecraft lava: set alight", mine ? "the player's vehicle" : "vehicle", h);
				} else if (burn && (c.hazard == proto::kHazardFire || c.hazard == proto::kHazardMagma)) {
					const float engine = v->m_fEngineHealth;
					const float rate = c.hazard == proto::kHazardFire ? 250.0f : 50.0f;
					if (engine > -50.0f) {
						S::SET_ENGINE_HEALTH(h, engine - rate * a_dt);
						if (engine > 0.0f && engine - rate * a_dt <= 0.0f) {
							LC_LOG("%s %d: its engine caught fire in Minecraft %s", mine ? "the player's vehicle" : "vehicle", h, Name(c.hazard));
						}
						++counters.carsScorched;
					}
				}
				// ---- water and lava: the vehicle struggles
				if (slow && c.liquid != proto::kLiquidNone && c.depth > 0.0f) {
					CVector vel{};
					v->GetVelocity(&vel);
					const float speed = std::hypot(vel.x, vel.y);
					const float next = hazard::Drag(speed, st.lastSpeed, c.depth, c.liquid, a_dt);
					if (speed > 0.05f && next < speed) {
						SetHorizontalSpeed(v, h, vel, next / speed);
					}
					st.lastSpeed = next;
					++counters.carFrames;
					counters.slowedBy += speed - next;
					EngineInWater(st, h, c.liquid == proto::kLiquidWater, c.depth, a_dt, mine);
					if (Cfg().debugHazards && mine && st.stall.off && st.stall.under >= hazard::StallTuning{}.after + 2.0f && st.haveDry && st.pulls < 2) {
						++st.pulls;
						S::SET_CAR_COORDINATES(h, st.dry[0], st.dry[1], st.dry[2]);
						S::SET_CAR_HEADING(h, st.dry[3]);
						st.lastSpeed = -1.0f;
						LC_LOG("DebugHazards: the player's stalled vehicle %d pulled out of the water, back to %.1f %.1f %.1f", h, st.dry[0], st.dry[1], st.dry[2]);
					}
					if (mine || Cfg().debugHazards) {
						if ((st.logT -= a_dt) <= 0.0f) {
							st.logT = 0.5f;
							LC_LOG("%s %d in Minecraft %s: %.2f m deep (%.0f%% of it wet), %.1f -> %.1f m/s (top %.1f); engine %.0f (%s), body %u%s%s",
								mine ? "the player's vehicle" : "vehicle", h, c.liquid == proto::kLiquidLava ? "lava" : "water", c.depth, c.wet * 100.0f, speed, next,
								hazard::TopSpeed(c.depth, c.liquid), v->m_fEngineHealth, v->m_nVehicleFlags.bEngineOn ? "running" : "off", CarHealth(h),
								S::IS_CAR_ON_FIRE(h) ? ", ON FIRE" : "", S::IS_CAR_DEAD(h) ? ", WRECKED" : "");
						}
					}
				} else {
					st.lastSpeed = -1.0f;
					EngineInWater(st, h, false, 0.0f, a_dt, mine);
				}
			}
			for (auto it = vehicles.begin(); it != vehicles.end();) {
				it = it->second.seen ? std::next(it) : vehicles.erase(it);
			}
		}

		void LogStats()
		{
			const auto now = ::GetTickCount64();
			if (now < nextStats) {
				return;
			}
			nextStats = now + 10000;
			const auto& c = counters;
			const auto calls = waterCalls.exchange(0), hits = waterHits.exchange(0);
			if (hits) {
				LC_LOG("stats 10s: GTA's water level asked %u times, %u of them answered with Minecraft water", calls, hits);
				if (Cfg().debugHazards) {
					char line[1600];
					int  n = std::snprintf(line, sizeof(line), "  callers (exe address: answered/asked):");
					for (int k = 0; k < kCallerSlots && n > 0 && n < int(sizeof(line)) - 40; ++k) {
						const auto a = callerAddr[k].load();
						const auto c2 = callerCalls[k].exchange(0), h2 = callerHits[k].exchange(0);
						if (a && c2) {
							const float* l = callerLast[k];
							n += std::snprintf(line + n, sizeof(line) - n, " %06X:%u/%u(%.1f %.1f %.1f->%.1f)", static_cast<unsigned>(a), h2, c2, l[0], l[1], l[2], l[3]);
						}
					}
					LC_LOG("%s", line);
				}
			}
			if (c.ignited || c.hurtPeds || c.putOut || c.cars || c.igniteFailed || c.carsScorched || c.carFrames || c.wading) {
				LC_LOG("stats 10s: %zu hazard blocks, %zu liquid blocks; peds set alight %u (%u didn't catch), hurt %u times (%u health), put out %u, died %u; "
					   "vehicles set alight %u, scorched %u, in liquid %u frames (%.1f m/s taken off on average), stalled %u, drowned %u; peds wading %u",
					grid.Cells(), liquids.Cells(), c.ignited, c.igniteFailed, c.hurtPeds, c.health, c.putOut, c.died, c.cars, c.carsScorched, c.carFrames,
					c.carFrames ? c.slowedBy / static_cast<float>(c.carFrames) : 0.0f, c.stalled, c.drowned, c.wading);
			}
			counters = Counters{};
		}
	}

	void OnLights(const std::uint8_t* a_data, std::uint32_t a_bytes)
	{
		if (!a_data || a_bytes < sizeof(proto::RenLights)) {
			return;
		}
		proto::RenLights hdr{};
		std::memcpy(&hdr, a_data, sizeof(hdr));
		PendingLights p;
		p.sx = hdr.sx, p.sy = hdr.sy, p.sz = hdr.sz;
		const std::uint32_t fits = (a_bytes - static_cast<std::uint32_t>(sizeof(proto::RenLights))) / sizeof(proto::RenLight);
		const std::uint32_t count = std::min(hdr.count, fits);
		// Only the hazards (most lights are torches and lamps).
		for (std::uint32_t i = 0; i < count; ++i) {
			proto::RenLight l;
			std::memcpy(&l, a_data + sizeof(proto::RenLights) + i * sizeof(proto::RenLight), sizeof(l));
			if (hazard::HazardOf(l.color) != proto::kHazardNone) {
				p.lights.push_back(l);
			}
		}
		std::lock_guard lock(pendingLock);
		if (pending.size() >= kMaxPending) {
			++pendingDropped;
			return;
		}
		pending.push_back(std::move(p));
	}

	void OnLiquids(const std::uint8_t* a_data, std::uint32_t a_bytes)
	{
		if (!a_data || a_bytes < sizeof(proto::RenLiquids)) {
			return;
		}
		proto::RenLiquids hdr{};
		std::memcpy(&hdr, a_data, sizeof(hdr));
		PendingLiquids p;
		p.sx = hdr.sx, p.sy = hdr.sy, p.sz = hdr.sz;
		const std::uint32_t fits = (a_bytes - static_cast<std::uint32_t>(sizeof(proto::RenLiquids))) / sizeof(proto::RenLiquid);
		p.liquids.resize(std::min(hdr.count, fits));
		if (!p.liquids.empty()) {
			std::memcpy(p.liquids.data(), a_data + sizeof(proto::RenLiquids), p.liquids.size() * sizeof(proto::RenLiquid));
		}
		std::lock_guard lock(pendingLock);
		if (pendingLiquids.size() >= kMaxPending) {
			++pendingDropped;
			return;
		}
		pendingLiquids.push_back(std::move(p));
	}

	void Clear()
	{
		std::lock_guard lock(pendingLock);
		pending.clear();
		pendingLiquids.clear();
		pendingClear = true;
	}

	void Tick(const Combat::Frame& a_f)
	{
		if (waterHook < 0) {
			HookWaterLevel();
		}
		ApplyPending();
		// Around the puppeted player the game's own water answers (Minecraft swims its player).
		waterPuppet.store(a_f.puppeting, std::memory_order_relaxed);
		if (CPed* pp = FindPlayerPed(); pp && pp->m_pMatrix) {
			waterPuppetX.store(pp->m_pMatrix->pos.x, std::memory_order_relaxed);
			waterPuppetY.store(pp->m_pMatrix->pos.y, std::memory_order_relaxed);
		}
		if (a_f.loading || !a_f.exists) {
			burning.clear();
			vehicles.clear();
			return;
		}
		CPed* player = FindPlayerPed();
		auto* pool = CPools::ms_pPedPool;
		if (!player || !player->m_pMatrix || !pool) {
			return;
		}
		const CVector here = player->m_pMatrix->pos;
		Vehicles(a_f.dt, here);  // every frame: the drag must be smooth
		if (!Cfg().hazardsBurnPeds) {
			burning.clear();
		}
		if ((tickT += a_f.dt) < kTick) {
			return;
		}
		tickT = 0.0f;
		tuning.damageScale = Cfg().pedDamageScale;
		DebugToStreet(a_f);
		DebugInject(a_f, here);
		DebugHarbour(a_f, here);
		// DebugHazards: every 15 s the nearest walking ped is put into Minecraft water 5 m from the player
		// (the water must be there), to see it swim.
		static float dunkT = 10.0f, dunkLogT = 0.0f;
		static int   dunked = 0;
		if (Cfg().debugHazards && !liquids.Empty() && (dunkT -= kTick) <= 0.0f) {
			dunkT = 15.0f;
			float       s = 0.0f, tx = 0.0f, ty = 0.0f;
			bool        found = false;
			for (int k = 0; k < 24 && !found; ++k) {
				const float r = 5.0f + 5.0f * static_cast<float>(k / 8);
				tx = here.x + r * std::cos(k * 0.785f);
				ty = here.y + r * std::sin(k * 0.785f);
				std::uint8_t kind = proto::kLiquidNone;
				const McVec  mc = GtaToMc(tx, ty, here.z - Cfg().rootToFeet);
				const float  d = liquids.DepthAt(mc.x, mc.y, mc.z, kind);
				found = kind == proto::kLiquidWater && d > 0.5f;
				s = mc.y + d;
			}
			int   best = 0;
			float bestD2 = 40.0f * 40.0f;
			for (int i = found ? pool->FindNextUsed(0) : -1; i >= 0; i = pool->FindNextUsed(i + 1)) {
				CPed* p = pool->Get(i);
				if (!p || p == player || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float d2 = (m.x - here.x) * (m.x - here.x) + (m.y - here.y) * (m.y - here.y);
				const int   h = static_cast<int>(pool->GetIndex(p));
				if (d2 < bestD2 && S::DOES_CHAR_EXIST(h) && !S::IS_CHAR_DEAD(h) && !S::IS_CHAR_IN_ANY_CAR(h)) {
					bestD2 = d2;
					best = h;
				}
			}
			if (best) {
				float gz = here.z;
				S::GET_GROUND_Z_FOR_3D_COORD(tx, ty, here.z + 2.0f, &gz);
				S::SET_CHAR_COORDINATES(best, tx, ty, gz + 1.0f);
				LC_LOG("DebugHazards: ped %d put into Minecraft water at %.1f %.1f (ground %.1f, surface %.1f)", best, tx, ty, gz, s);
				dunked = best;
				dunkLogT = 0.0f;
			}
		}

		// DebugHazards: the dunked ped's speed, every second for 8 s (wading).
		if (dunked && (dunkLogT += kTick) <= 8.0f && std::fmod(dunkLogT, 1.0f) < kTick && S::DOES_CHAR_EXIST(dunked)) {
			float speed = 0.0f, mult = 1.0f, x = 0, y = 0, z = 0;
			S::GET_CHAR_SPEED(dunked, &speed);
			S::GET_CHAR_MOVE_ANIM_SPEED_MULTIPLIER(dunked, &mult);
			S::GET_CHAR_COORDINATES(dunked, &x, &y, &z);
			LC_LOG("DebugHazards: ped %d at %.1f %.1f %.1f, %.2f m/s, move speed x %.2f", dunked, x, y, z, speed, mult);
		}

		// DebugHazards: who swims (GTA's water level now follows Minecraft's water).
		static float swimLogT = 0.0f;
		if (Cfg().debugHazards && waterHook == 1 && waterPublished.load() && (swimLogT -= kTick) <= 0.0f) {
			swimLogT = 2.0f;
			int  swimming = 0, inWater = 0, example = 0;
			CVector at{};
			for (int i = pool->FindNextUsed(0); i >= 0; i = pool->FindNextUsed(i + 1)) {
				CPed* p = pool->Get(i);
				if (!p || !p->m_pMatrix) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				if ((m.x - here.x) * (m.x - here.x) + (m.y - here.y) * (m.y - here.y) > kRange * kRange) {
					continue;
				}
				const int h = static_cast<int>(pool->GetIndex(p));
				if (S::IS_CHAR_SWIMMING(h)) {
					++swimming;
					example = h;
					at = m;
				}
				inWater += S::IS_CHAR_IN_WATER(h) ? 1 : 0;
			}
			if (swimming || inWater) {
				LC_LOG("DebugHazards: %d peds in GTA water, %d swimming (e.g. ped %d at %.1f %.1f %.1f)", inWater, swimming, example, at.x, at.y, at.z);
			}
		}

		const bool burnPeds = Cfg().hazardsBurnPeds && (!grid.Empty() || !burning.empty());
		const bool wadePeds = Cfg().liquidsSlowPeds && (!liquids.Empty() || !wading.empty());
		if (!burnPeds && !wadePeds) {
			LogStats();
			return;
		}

		// Peds on foot near the player.
		std::unordered_map<int, bool> seen, wadeSeen;
		const float feetDrop = Cfg().rootToFeet;
		for (int i = pool->FindNextUsed(0); i >= 0; i = pool->FindNextUsed(i + 1)) {
			CPed* p = pool->Get(i);
			if (!p || !p->m_pMatrix) {
				continue;
			}
			const auto& m = p->m_pMatrix->pos;
			if ((m.x - here.x) * (m.x - here.x) + (m.y - here.y) * (m.y - here.y) > kRange * kRange) {
				continue;
			}
			if (p == player && a_f.puppeting) {
				continue;  // Minecraft's player: Minecraft burns it itself
			}
			const int  h = static_cast<int>(pool->GetIndex(p));
			const McVec feet = GtaToMc(m.x, m.y, m.z - feetDrop);
			// Wading in Minecraft water: slower the deeper (GTA's peds only swim in GTA's own water).
			if (wadePeds) {
				float mult = 1.0f;
				if (!liquids.Empty()) {
					std::uint8_t kind = proto::kLiquidNone;
					const float  depth = liquids.DepthAt(feet.x, feet.y, feet.z, kind);
					if (kind == proto::kLiquidWater) {
						mult = hazard::WadeSpeed(depth);
					}
				}
				const auto w = wading.find(h);
				if (mult < 0.999f) {
					wadeSeen[h] = true;
					if ((w == wading.end() || std::fabs(w->second - mult) > 0.05f) && S::DOES_CHAR_EXIST(h) && !S::IS_CHAR_IN_ANY_CAR(h)) {
						if (w == wading.end()) {
							++counters.wading;
						}
						S::SET_CHAR_MOVE_ANIM_SPEED_MULTIPLIER(h, mult);
						wading[h] = mult;
					}
				} else if (w != wading.end()) {
					if (S::DOES_CHAR_EXIST(h)) {
						S::SET_CHAR_MOVE_ANIM_SPEED_MULTIPLIER(h, 1.0f);
					}
					wading.erase(w);
				}
			}
			if (!burnPeds) {
				continue;
			}
			const auto hz = grid.Touching(feet.x, feet.y, feet.z, kPedRadius, kPedHeight);
			auto       it = burning.find(h);
			if (hz == proto::kHazardNone && it == burning.end()) {
				continue;
			}
			if (!S::DOES_CHAR_EXIST(h) || S::IS_CHAR_IN_ANY_CAR(h)) {
				continue;
			}
			if (S::IS_CHAR_DEAD(h)) {
				if (it != burning.end()) {
					++counters.died;
					burning.erase(it);
				}
				continue;
			}
			seen[h] = true;
			auto&      b = it != burning.end() ? it->second : burning[h];
			const bool alight = S::IS_CHAR_ON_FIRE(h);
			const auto a = hazard::Step(b, hz, alight, kTick, tuning);
			if (a.ignite) {
				NativeInvoke::Invoke<int>(NATIVE_START_CHAR_FIRE, h);
				if (S::IS_CHAR_ON_FIRE(h)) {
					++counters.ignited;
					LC_LOG_EVERY(1000, "ped %d in Minecraft %s at GTA %.1f %.1f %.1f: set alight", h, Name(hz), m.x, m.y, m.z);
				} else {
					++counters.igniteFailed;
				}
			}
			if (a.damage) {
				HurtPed(h, a.damage);
			}
			if (a.extinguish) {
				S::EXTINGUISH_CHAR_FIRE(h);
				++counters.putOut;
			}
			if (a.done) {
				burning.erase(h);
			}
		}
		for (auto it = burning.begin(); it != burning.end();) {
			it = seen.count(it->first) ? std::next(it) : burning.erase(it);  // gone, in a car, out of range
		}
		for (auto it = wading.begin(); it != wading.end();) {
			if (wadeSeen.count(it->first)) {
				++it;
				continue;
			}
			if (S::DOES_CHAR_EXIST(it->first)) {
				S::SET_CHAR_MOVE_ANIM_SPEED_MULTIPLIER(it->first, 1.0f);  // (out of range, in a car, or the puppet now)
			}
			it = wading.erase(it);
		}

		LogStats();
	}
}
