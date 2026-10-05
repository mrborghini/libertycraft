// Unity-built into dllmain.cpp (needs IV-SDK). See Blasts.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "blasts"
#include "Blasts.h"

#include "Combat.h"
#include "Config.h"
#include "Coords.h"
#include "Link.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace lc::Blasts
{
	namespace
	{
		namespace S = ::Scripting;
		namespace proto = ::libertycraft::proto;

		using AddExplosionFn = std::uint32_t(__cdecl*)(void*, void*, std::uint32_t, std::uint32_t, const float*, std::uint32_t, std::uint32_t, std::uint32_t,
			std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
			std::uint32_t);
		AddExplosionFn original = nullptr;
		int            hooked = -1;  // -1 not tried, 0 not hooked, 1 hooked
		int            fromMinecraft = 0;  // game thread only

		struct Seen
		{
			std::uint32_t type;
			float         at[3];
			float         size;
			void*         creator;
			void*         victim;
			std::uint32_t result;
		};
		std::mutex        lock;
		std::vector<Seen> seen;
		std::uint32_t     sent = 0, skipped = 0, mirrored = 0;
		float             statsTimer = 0.0f;
		bool              tableLogged = false;

		std::uint32_t __cdecl AddExplosionHook(void* a_victim, void* a_creator, std::uint32_t a_type, std::uint32_t a_size, const float* a_at, std::uint32_t a6,
			std::uint32_t a7, std::uint32_t a8, std::uint32_t a9, std::uint32_t a10, std::uint32_t a11, std::uint32_t a12, std::uint32_t a13, std::uint32_t a14,
			std::uint32_t a15, std::uint32_t a16, std::uint32_t a17, std::uint32_t a18, std::uint32_t a19)
		{
			const std::uint32_t r = original(a_victim, a_creator, a_type, a_size, a_at, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19);
			if (fromMinecraft > 0) {
				++mirrored;
				return r;
			}
			if (a_at && a_type < 25) {
				float size = 0.0f;
				std::memcpy(&size, &a_size, sizeof(size));
				std::lock_guard g(lock);
				if (seen.size() < 32) {
					seen.push_back({ a_type, { a_at[0], a_at[1], a_at[2] }, size, a_creator, a_victim, r });
				}
			}
			return r;
		}

		void Hook()
		{
			if (hooked >= 0) {
				return;
			}
			hooked = 0;
			if (plugin::gameVer != plugin::VERSION_1080) {
				LC_LOG("GTA's explosions: not GTA IV 1.0.8.0: they don't reach Minecraft");
				return;
			}
			auto* at = reinterpret_cast<std::uint8_t*>(AddressSetter::gBaseAddress) + (0x9940D0 - 0x400000);
			// push ebp; mov ebp, esp; and esp, -16; sub esp, 0x64; push ebx; push esi; push edi
			static constexpr std::uint8_t kExpect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x64, 0x53, 0x56, 0x57 };
			if (std::memcmp(at, kExpect, sizeof(kExpect)) != 0) {
				LC_LOG("GTA's explosions: CExplosionManager::AddExplosion isn't as expected: they don't reach Minecraft");
				return;
			}
			auto* tramp = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!tramp) {
				LC_LOG("GTA's explosions: can't allocate the trampoline (error %lu)", ::GetLastError());
				return;
			}
			const auto abs32 = [](const void* a_p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_p)); };
			// The first 9 bytes (whole instructions, nothing relative), then on into the original.
			std::memcpy(tramp, at, 9);
			tramp[9] = 0xE9;
			const std::uint32_t back = abs32(at + 9) - (abs32(tramp) + 14u);
			std::memcpy(tramp + 10, &back, 4);
			::FlushInstructionCache(::GetCurrentProcess(), tramp, 14);
			original = reinterpret_cast<AddExplosionFn>(tramp);
			std::uint8_t patch[9];
			std::memset(patch, 0x90, sizeof(patch));
			patch[0] = 0xE9;
			const std::uint32_t to = abs32(reinterpret_cast<const void*>(&AddExplosionHook)) - (abs32(at) + 5u);
			std::memcpy(patch + 1, &to, 4);
			DWORD old = 0;
			if (!::VirtualProtect(at, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
				LC_LOG("GTA's explosions: can't hook AddExplosion (VirtualProtect error %lu)", ::GetLastError());
				return;
			}
			std::memcpy(at, patch, sizeof(patch));
			::VirtualProtect(at, sizeof(patch), old, &old);
			::FlushInstructionCache(::GetCurrentProcess(), at, sizeof(patch));
			hooked = 1;
			LC_LOG("GTA's explosions reach Minecraft (CExplosionManager::AddExplosion hooked)");
		}

		const char* EntityKind(const void* a_e)
		{
			if (!a_e) {
				return "none";
			}
			auto* pp = CPools::ms_pPedPool;
			auto* vp = CPools::ms_pVehiclePool;
			const auto in = [](auto* a_pool, const void* a_p) {
				if (!a_pool || !a_pool->m_pObjects || a_pool->m_nEntrySize == 0) {
					return false;
				}
				const auto base = reinterpret_cast<std::uintptr_t>(a_pool->m_pObjects), p = reinterpret_cast<std::uintptr_t>(a_p);
				return p >= base && p < base + static_cast<std::uintptr_t>(a_pool->m_nCount) * a_pool->m_nEntrySize && (p - base) % a_pool->m_nEntrySize == 0;
			};
			return in(pp, a_e) ? "ped" : in(vp, a_e) ? "vehicle" : "other";
		}
	}

	namespace
	{
		// DebugGtaBlast=N: N s into puppet mode, one of GTA IV's explosions every 6 s ahead of the camera:
		// a grenade 7 m ahead, a molotov 7 m ahead and 6 m left, a rocket 7 m ahead and 6 m right, a car blown
		// up 14 m ahead (EXPLODE_CAR: GTA's own car explosion), a grenade 3 m from the player, and 1.5 s
		// later a molotov on him (dead or not: GTA's damage to a dying player must not reach Minecraft). Each
		// logs where it is in Minecraft's coordinates.
		struct BlastTest
		{
			int   step = 0;
			float timer = 0.0f;
			int   car = 0;
			bool  requested = false;
			float last[3]{};  // where the player was at the last step (he may be dead at the next)
		} test;

		void TestHook(const Combat::Frame& a_frame)
		{
			auto& t = test;
			if (Config::Get().debugGtaBlast <= 0 || t.step >= 7 || !a_frame.mc || (t.step < 5 && (!a_frame.puppeting || !a_frame.exists))) {
				return;
			}
			t.timer += a_frame.dt;
			const float due = t.step == 0 ? static_cast<float>(Config::Get().debugGtaBlast) : t.step == 5 ? 1.5f : 6.0f;
			const unsigned carModel = S::GET_HASH_KEY("admiral");
			if (t.step == 3 && !t.requested) {
				t.requested = true;
				CStreaming::ScriptRequestModel(static_cast<std::int32_t>(carModel));
			}
			if (t.timer < due) {
				return;
			}
			float x = t.last[0], y = t.last[1], z = t.last[2];
			if (t.step < 5 && a_frame.ped && S::DOES_CHAR_EXIST(a_frame.ped) && !S::IS_CHAR_DEAD(a_frame.ped)) {
				S::GET_CHAR_COORDINATES(a_frame.ped, &x, &y, &z);
				t.last[0] = x, t.last[1] = y, t.last[2] = z;
			}
			const float h = McYawToGtaHeading(a_frame.mc->yaw) * kDegToRad, fx = -std::sin(h), fy = std::cos(h);
			const float ahead = t.step == 5 ? 0.0f : t.step == 4 ? 3.0f : t.step == 3 ? 14.0f : 7.0f;
			const float side = t.step == 1 ? -6.0f : t.step == 2 ? 6.0f : 0.0f;  // (right of the camera: positive)
			const float rx = std::cos(h), ry = std::sin(h);
			const float bx = x + fx * ahead + rx * side, by = y + fy * ahead + ry * side;
			float ground = z - 1.0f;
			S::GET_GROUND_Z_FOR_3D_COORD(bx, by, z + 1.0f, &ground);
			if (!std::isfinite(ground) || ground < z - 4.0f || ground > z + 2.0f) {
				ground = z - 1.0f;
			}
			const McVec m = GtaToMc(bx, by, ground);
			const char* what = "";
			switch (t.step) {
			case 0: what = "grenade"; S::ADD_EXPLOSION(bx, by, ground + 0.3f, 0, 1.0f, true, false, 1.0f); break;
			case 1: what = "molotov"; S::ADD_EXPLOSION(bx, by, ground + 0.3f, 1, 1.0f, true, false, 1.0f); break;
			case 2: what = "rocket"; S::ADD_EXPLOSION(bx, by, ground + 0.5f, 2, 1.0f, true, false, 1.0f); break;
			case 3:
				if (!S::HAS_MODEL_LOADED(carModel)) {
					return;
				}
				what = "car";
				S::CREATE_CAR(carModel, bx, by, ground + 0.5f, &t.car, true);
				S::MARK_MODEL_AS_NO_LONGER_NEEDED(carModel);
				if (t.car) {
					S::EXPLODE_CAR(t.car, true, false);
				}
				break;
			case 4: what = "grenade near the player"; S::ADD_EXPLOSION(bx, by, ground + 0.3f, 0, 1.0f, true, false, 1.0f); break;
			case 5: what = "molotov on the player"; S::ADD_EXPLOSION(bx, by, ground + 0.3f, 1, 1.0f, true, false, 1.0f); break;
			default: break;
			}
			if (t.step < 6) {
				LC_LOG("DebugGtaBlast: step %d: %s at GTA %.1f %.1f %.1f (MC %.1f %.1f %.1f), %.0f m ahead of the player", t.step + 1, what, bx, by, ground, m.x, m.y, m.z,
					ahead);
			} else if (t.car && S::DOES_VEHICLE_EXIST(t.car)) {
				S::MARK_CAR_AS_NO_LONGER_NEEDED(&t.car);
			}
			++t.step;
			t.timer = 0.0f;
		}
	}

	FromMinecraft::FromMinecraft() { ++fromMinecraft; }
	FromMinecraft::~FromMinecraft() { --fromMinecraft; }

	bool Tick(const Combat::Frame& a_frame, float a_px, float a_py, float a_pz)
	{
		Hook();
		TestHook(a_frame);
		std::vector<Seen> now;
		{
			std::lock_guard g(lock);
			now.swap(seen);
		}
		const tExplosionInfo* info = CExplosion::ms_ExplosionInfo;
		if (info && !tableLogged && Config::Get().diagnostics) {
			tableLogged = true;
			for (int i = 0; i < 25; ++i) {
				LC_LOG("explosion type %2d: radius %.1f damage %.0f..%.0f directed %.1f fires %u-%u", i, info[i].m_fEndRadius, info[i].m_fDamageAtCentre,
					info[i].m_fDamageAtEdge, info[i].m_fDirectedWidth, info[i].m_nNumFiresMin, info[i].m_nNumFiresMax);
			}
		}
		bool reachesPlayer = false;
		const bool playable = a_frame.exists && !a_frame.loading && a_frame.mcInWorld;
		CPed* playerPed = FindPlayerPed();
		for (const Seen& s : now) {
			if (!playable || !info) {
				++skipped;
				continue;
			}
			const tExplosionInfo& e = info[s.type];
			const float scale = s.size > 0.0f && std::isfinite(s.size) ? std::clamp(s.size, 0.01f, 1.0f) : 1.0f;
			const float radius = e.m_fEndRadius * scale;
			if (e.m_fDirectedWidth > 0.0f || e.m_fDamageAtCentre <= 0.0f || !(radius > 0.2f)) {
				++skipped;
				LC_LOG_EVERY(5000, "GTA explosion type %u at %.1f %.1f %.1f left out (directed %.1f, damage %.0f, radius %.1f)", s.type, s.at[0], s.at[1], s.at[2],
					e.m_fDirectedWidth, e.m_fDamageAtCentre, radius);
				continue;
			}
			const bool byPlayer = playerPed && (s.creator == playerPed || s.victim == playerPed);
			const bool fire = e.m_nNumFiresMax > 0;
			const std::uint16_t code = static_cast<std::uint16_t>((s.type & proto::kGtaBlastTypeMask)
				| (std::min(127u, static_cast<std::uint32_t>(std::lround(radius * 2.0f))) << proto::kGtaBlastRadiusShift)
				| (byPlayer ? proto::kGtaBlastByPlayer : 0u) | (fire ? proto::kGtaBlastFire : 0u));
			const McVec m = GtaToMc(s.at[0], s.at[1], s.at[2]);
			Link::Get().PushInput(proto::kInGtaExplosion, code, static_cast<std::int32_t>(std::lround(m.x * 256.0)), static_cast<std::int32_t>(std::lround(m.y * 256.0)),
				static_cast<std::int32_t>(std::lround(m.z * 256.0)));
			++sent;
			const float dist = std::sqrt((s.at[0] - a_px) * (s.at[0] - a_px) + (s.at[1] - a_py) * (s.at[1] - a_py) + (s.at[2] - a_pz) * (s.at[2] - a_pz));
			reachesPlayer |= dist < radius + 2.0f;
			LC_LOG("GTA explosion type %u (radius %.1f m, size %.2f%s%s) at GTA %.1f %.1f %.1f, %.1f m from the player -> Minecraft (caused by %s, victim %s, result %u)",
				s.type, radius, s.size, byPlayer ? ", the player's" : "", fire ? ", fire" : "", s.at[0], s.at[1], s.at[2], dist, EntityKind(s.creator),
				EntityKind(s.victim), s.result);
		}
		if ((statsTimer += a_frame.dt) >= 10.0f) {
			statsTimer = 0.0f;
			if (sent || skipped || mirrored) {
				LC_LOG("stats 10s: %u GTA explosions sent to Minecraft, %u left out, %u of Minecraft's own mirrored into GTA", sent, skipped, mirrored);
			}
			sent = skipped = mirrored = 0;
		}
		return reachesPlayer;
	}
}
