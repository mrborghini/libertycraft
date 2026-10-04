// Unity-built into dllmain.cpp (needs IV-SDK). See NpcBlocks.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "blocks"
#include "NpcBlocks.h"

#include "Combat.h"
#include "Config.h"
#include "Coords.h"
#include "Link.h"
#include "Log.h"
#include "combat/BlockPush.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lc::NpcBlocks
{
	namespace
	{
		namespace S = ::Scripting;
		namespace proto = ::libertycraft::proto;

		constexpr float  kPedRange = 120.0f;  // metres from the player
		constexpr float  kCarRange = 160.0f;
		constexpr double kPedRadius = 0.35;   // GTA IV's ped capsule, metres = blocks
		constexpr double kPedTouch = 0.08;    // a wall this close still counts as walked into
		constexpr double kPedHeight = 1.8;
		constexpr double kLift = 0.3;         // feet this far into a block's top still stand on it
		constexpr double kSlideSpeed = 2.0;   // m/s along a wall
		constexpr float  kDetourSeconds = 1.5f;
		constexpr double kCarMargin = 0.05;   // blocks closer than this (m) already stop a vehicle
		constexpr double kCarBounce = 0.05;
		constexpr double kCarPushOut = 0.03;  // a vehicle this far (m) into blocks is moved back out
		constexpr std::size_t kMaxPending = 65536;

		// ---- render thread -> game thread ----------------------------------------------------------
		struct PendingSection
		{
			std::int32_t                  sx, sy, sz;
			bool                          remove;
			std::array<std::uint8_t, 512> bits;
		};
		std::mutex                  pendingLock;
		std::vector<PendingSection> pending;
		bool                        pendingClear = false;
		std::uint32_t               pendingDropped = 0;

		// ---- game thread -------------------------------------------------------------------------------
		blocks::SolidGrid           grid;
		std::recursive_mutex        gridLock;  // the grid: the game thread, and GTA's bullets (BulletProbe)
		std::atomic<bool>           anySolids{ false };
		std::vector<PendingSection> applying;

		struct Detour
		{
			int   side = 0;
			float time = 0.0f;
		};
		std::unordered_map<std::uint32_t, Detour> detours;  // ped handle

		struct Dims
		{
			bool  ok = false;
			float min[3]{}, max[3]{};
		};
		std::unordered_map<std::int32_t, Dims> dimsByModel;  // CEntity::m_nModelIndex

		struct Counters
		{
			std::uint32_t frames = 0, sectionsIn = 0, clears = 0;
			std::uint32_t pedsPushed = 0, pedsDetoured = 0, pedFrames = 0;
			std::uint32_t carsTouching = 0, carsSlowed = 0, carsPushedOut = 0, velocityFallbacks = 0;
			float         fastestStopped = 0.0f;  // m/s into the blocks, before
			double        us = 0.0;
		} counters;
		std::uint64_t lastStatsMs = 0;
		bool          loggedFirstSolids = false;
		int           velocityWorks = -1;  // -1 unknown, 1 the collider's SetVelocity took, 0 it didn't

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

		void ApplyPending()
		{
			bool clear = false;
			{
				std::lock_guard lock(pendingLock);
				applying.swap(pending);
				clear = pendingClear;
				pendingClear = false;
			}
			if (clear) {
				grid.Clear();
				anySolids = false;
				detours.clear();
				++counters.clears;
			}
			for (const auto& p : applying) {
				grid.Set(p.sx, p.sy, p.sz, p.remove ? nullptr : p.bits.data());
			}
			anySolids = !grid.Empty();
			counters.sectionsIn += static_cast<std::uint32_t>(applying.size());
			applying.clear();
			if (!loggedFirstSolids && !grid.Empty()) {
				loggedFirstSolids = true;
				LC_LOG("Minecraft's solid blocks arrived (%zu sections so far): peds and vehicles collide with them", grid.Sections());
			}
		}

		// ---- peds -----------------------------------------------------------------------------------
		void MovePed(CPed* a_ped, std::uint32_t a_handle, float a_x, float a_y, float a_z)
		{
			if (Config::Get().npcPushMethod == 1) {
				S::SET_CHAR_COORDINATES_NO_OFFSET(static_cast<int>(a_handle), a_x, a_y, a_z);
				return;
			}
			CVector v{ a_x, a_y, a_z };
			a_ped->Teleport(&v, false, true);
		}

		void PushPeds(const Combat::Frame& a_frame, float a_px, float a_py)
		{
			auto* pool = CPools::ms_pPedPool;
			if (!pool) {
				return;
			}
			CPed*       playerPed = FindPlayerPed();
			const float feetDrop = Config::Get().rootToFeet;
			const double r = kPedRadius + kPedTouch;
			for (int i = 0; i < static_cast<int>(pool->m_nCount); ++i) {
				CPed* p = pool->Get(i);
				if (!p || !p->m_pMatrix || (p == playerPed && a_frame.puppeting)) {
					continue;
				}
				const auto& m = p->m_pMatrix->pos;
				const float dx = m.x - a_px, dy = m.y - a_py;
				if (dx * dx + dy * dy > kPedRange * kPedRange) {
					continue;
				}
				const McVec  feet = GtaToMc(m.x, m.y, m.z - feetDrop);
				std::int32_t y0 = 0, y1 = 0;
				blocks::BodyRows(feet.y, kPedHeight, kLift, y0, y1);
				double px = 0.0, pz = 0.0;
				if (!blocks::CirclePush(grid, feet.x, feet.z, r, y0, y1, px, pz)) {
					continue;  // the common case: no natives for peds away from blocks
				}
				const auto handle = pool->GetIndex(p);
				const int  h = static_cast<int>(handle);
				if (!S::DOES_CHAR_EXIST(h) || S::IS_CHAR_DEAD(h) || S::IS_CHAR_IN_ANY_CAR(h) || S::IS_PED_RAGDOLL(h)) {
					continue;  // the dead and the ragdolled are physics' business, the seated their car's
				}
				double cx = feet.x, cz = feet.z;
				if (!blocks::PushCircleOut(grid, cx, cz, r, y0, y1)) {
					continue;
				}
				++counters.pedsPushed;
				auto&        detour = detours[handle];
				const double ox = cx - feet.x, oz = cz - feet.z, olen = std::sqrt(ox * ox + oz * oz);
				detour.time -= a_frame.dt;
				if (olen > 1e-4) {
					// Walking into the wall? Follow it round, toward its nearer end.
					const double nx = ox / olen, nz = oz / olen;
					const double hd = p->m_fCurrentHeading;  // radians; GTA forward (-sin h, cos h) = MC (-sin h, -cos h)
					const double gx = -std::sin(hd), gz = -std::cos(hd);
					if (detour.time <= 0.0f || detour.side == 0) {
						detour.side = blocks::DetourSide(grid, cx, cz, nx, nz, gx, gz, y0, y1);
					}
					if (detour.side != 0 && gx * -nx + gz * -nz > 0.2) {
						detour.time = kDetourSeconds;
						const double step = kSlideSpeed * a_frame.dt;
						cx += -nz * detour.side * step;
						cz += nx * detour.side * step;
						++counters.pedsDetoured;
					}
				}
				const GtaVec to = McToGta(cx, feet.y, cz);
				MovePed(p, handle, static_cast<float>(to.x), static_cast<float>(to.y), m.z);
			}
			if (detours.size() > 256) {
				detours.clear();
			}
		}

		// ---- vehicles -------------------------------------------------------------------------------
		const Dims& DimsOf(std::int32_t a_modelIndex, int a_handle)
		{
			auto [it, fresh] = dimsByModel.try_emplace(a_modelIndex);
			if (fresh) {
				unsigned model = 0;
				S::GET_CAR_MODEL(a_handle, &model);
				struct
				{
					S::Vector3 v;
					float      pad[4];
				} lo{}, hi{};
				S::GET_MODEL_DIMENSIONS(model, &lo.v, &hi.v);
				Dims& d = it->second;
				d.min[0] = lo.v.x, d.min[1] = lo.v.y, d.min[2] = lo.v.z;
				d.max[0] = hi.v.x, d.max[1] = hi.v.y, d.max[2] = hi.v.z;
				d.ok = d.max[0] > d.min[0] + 0.2f && d.max[1] > d.min[1] + 0.2f && d.max[2] > d.min[2] + 0.2f && d.max[1] - d.min[1] < 40.0f;
				if (Config::Get().diagnostics || !d.ok) {
					LC_LOG("vehicle model %08X (%s): %.2f x %.2f x %.2f m%s", model, S::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model), d.max[0] - d.min[0],
						d.max[1] - d.min[1], d.max[2] - d.min[2], d.ok ? "" : " (implausible: ignored)");
				}
			}
			return it->second;
		}

		CVector VelocityOf(CVehicle* a_veh)
		{
			CVector v{};
			a_veh->GetVelocity(&v);
			return v;
		}

		void SetVelocity(CVehicle* a_veh, int a_handle, const CVector& a_vel, const CVector& a_before)
		{
			if (velocityWorks != 0) {
				if (auto* collider = a_veh->GetConstrainedCollider()) {
					CVector v = a_vel;
					collider->SetVelocity(&v);
					if (velocityWorks == -1) {
						const CVector back = VelocityOf(a_veh);
						const float   err = std::hypot(back.x - a_vel.x, back.y - a_vel.y);
						velocityWorks = err < 0.05f + 0.1f * std::hypot(a_vel.x - a_before.x, a_vel.y - a_before.y) ? 1 : 0;
						LC_LOG("vehicle velocity through its physics collider: %.2f %.2f -> asked %.2f %.2f, reads back %.2f %.2f: %s", a_before.x, a_before.y,
							a_vel.x, a_vel.y, back.x, back.y, velocityWorks ? "works" : "doesn't take, using SET_CAR_FORWARD_SPEED");
					}
					if (velocityWorks == 1) {
						return;
					}
				}
			}
			// What's left of the speed along the vehicle's axis.
			const auto& fwd = a_veh->m_pMatrix->up;  // (see BlockVehicles: "up" is the forward axis)
			S::SET_CAR_FORWARD_SPEED(a_handle, a_vel.x * fwd.x + a_vel.y * fwd.y + a_vel.z * fwd.z);
			++counters.velocityFallbacks;
		}

		void BlockVehicles(const Combat::Frame& a_frame, float a_px, float a_py)
		{
			auto* pool = CPools::ms_pVehiclePool;
			if (!pool) {
				return;
			}
			const float dt = std::clamp(a_frame.dt, 0.0f, 0.1f);
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CVehicle* veh = pool->Get(slot);
				if (!veh || !veh->m_pMatrix || (veh->m_nVehicleType != VEHICLE_TYPE_AUTOMOBILE && veh->m_nVehicleType != VEHICLE_TYPE_BIKE)) {
					continue;
				}
				const CMatrix& mat = *veh->m_pMatrix;
				const float    dx = mat.pos.x - a_px, dy = mat.pos.y - a_py;
				if (dx * dx + dy * dy > kCarRange * kCarRange) {
					continue;
				}
				const int   handle = static_cast<int>(pool->GetIndex(veh));
				const Dims& d = DimsOf(veh->m_nModelIndex, handle);
				if (!d.ok) {
					continue;
				}
				// The box's centre in the world, its axis seen from above, and its height range.
				const float lx = (d.min[0] + d.max[0]) * 0.5f, ly = (d.min[1] + d.max[1]) * 0.5f, lz = (d.min[2] + d.max[2]) * 0.5f;
				// IV-SDK's CMatrix names its rows after GTA SA's: "up" is the forward (y) axis, "at" the up (z) axis.
				const auto& fwd = mat.up;
				const auto& upv = mat.at;
				const float gx = mat.pos.x + mat.right.x * lx + fwd.x * ly + upv.x * lz;
				const float gy = mat.pos.y + mat.right.y * lx + fwd.y * ly + upv.y * lz;
				const float flen = std::hypot(fwd.x, fwd.y);
				if (flen < 0.2f) {
					continue;  // standing on its nose or tail
				}
				const double fx = fwd.x / flen, fz = -fwd.y / flen;  // MC
				const double halfL = (d.max[1] - d.min[1]) * 0.5 + kCarMargin, halfW = (d.max[0] - d.min[0]) * 0.5 + kCarMargin;
				const McVec  c = GtaToMc(gx, gy, mat.pos.z + d.min[2]);
				std::int32_t y0 = 0, y1 = 0;
				blocks::BodyRows(c.y, d.max[2] - d.min[2], kLift, y0, y1);
				const auto now = blocks::VehicleContact(grid, c.x, c.z, fx, fz, halfL, halfW, y0, y1);
				const CVector vel = VelocityOf(veh);
				blocks::Contact ahead;
				if (!now.hit) {
					const double sx = vel.x * dt * 1.5, sz = -vel.y * dt * 1.5;
					if (sx * sx + sz * sz < 1e-6) {
						continue;
					}
					ahead = blocks::VehicleContact(grid, c.x + sx, c.z + sz, fx, fz, halfL, halfW, y0, y1);
					if (!ahead.hit) {
						continue;
					}
				}
				const auto& hit = now.hit ? now : ahead;
				++counters.carsTouching;
				double vx = vel.x, vz = -vel.y;
				const double into = -(vx * hit.nx + vz * hit.nz);
				if (blocks::BlockVelocity(vx, vz, hit.nx, hit.nz, kCarBounce)) {
					CVector nv{ static_cast<float>(vx), static_cast<float>(-vz), vel.z };
					SetVelocity(veh, handle, nv, vel);
					++counters.carsSlowed;
					counters.fastestStopped = std::max(counters.fastestStopped, static_cast<float>(into));
					if (into > 3.0) {
						LC_LOG_EVERY(2000, "vehicle %d stopped by blocks at GTA %.1f %.1f %.1f: %.1f m/s into them, speed %.1f -> %.1f m/s", handle, mat.pos.x,
							mat.pos.y, mat.pos.z, into, std::hypot(vel.x, vel.y), std::hypot(nv.x, nv.y));
					}
				}
				const double inside = now.hit ? now.depth - kCarMargin : 0.0;
				if (inside > kCarPushOut) {
					const double back = inside - kCarPushOut * 0.5;
					CVector      to{ static_cast<float>(mat.pos.x + now.nx * back), static_cast<float>(mat.pos.y - now.nz * back), mat.pos.z };
					veh->Teleport(&to, false, true);
					++counters.carsPushedOut;
				}
			}
		}

		// ---- bullets --------------------------------------------------------------------------------------
		// Minecraft's blocks stop GTA IV's gunfire. GTA IV 1.0.8.0 traces every instant-hit shot (and
		// the delayed-hit ones) through 0x92DAA0 (cdecl, 13 arguments: the shot's start and end
		// first, the end in the caller's frame; returns how many things it hit, 0: nothing). Its first
		// instruction is a jump to BulletProbe, which marches the shot through Minecraft's solid blocks
		// first (blocks::RayFirstSolid): when a block comes first, the shot's end is moved to that
		// block's face before GTA traces it, so GTA only finds what lies in front of the blocks (no
		// damage behind them, for the player, peds and vehicles alike) and its tracer and end point
		// stop at the wall. If nothing is in front, an impact effect is queued at the face (spawned
		// by Tick: TRIGGER_PTFX imp_bullet_concrete, GTA's own concrete bullet impact).
		using BulletProbeFn = int(__cdecl*)(float*, float*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
			std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
		BulletProbeFn bulletOriginal = nullptr;
		int           bulletHook = -1;  // -1 not tried, 0 not hooked, 1 hooked

		struct Impact
		{
			float at[3], n[3];
			int   face;  // Minecraft's Direction (0 down, 1 up, 2 north, 3 south, 4 west, 5 east), -1 none
		};
		std::mutex          impactLock;
		std::vector<Impact> impacts;

		struct BulletCounters
		{
			std::atomic<std::uint32_t> probes{ 0 }, stopped{ 0 }, nearerHit{ 0 }, forPlayer{ 0 }, inside{ 0 };
		} bullets;
		std::uint32_t impactsSpawned = 0;

		// GTA space. True if a solid block comes first on a_from -> a_to: where (a_at, nudged a
		// little out of the face), its face's normal and the fraction along the shot.
		bool ClipShot(const float a_from[3], const float a_to[3], float a_at[3], float a_n[3], float& a_t, int& a_face)
		{
			if (!anySolids.load(std::memory_order_relaxed)) {
				return false;
			}
			const McVec f = GtaToMc(a_from[0], a_from[1], a_from[2]), t = GtaToMc(a_to[0], a_to[1], a_to[2]);
			const double from[3] = { f.x, f.y, f.z }, to[3] = { t.x, t.y, t.z };
			double frac = 0.0;
			int    axis = -1, sign = 0;
			{
				std::lock_guard lock(gridLock);
				if (!blocks::RayFirstSolid(grid, from, to, frac, axis, sign)) {
					return false;
				}
			}
			const double len = std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]) + (to[2] - from[2]) * (to[2] - from[2]));
			const double back = len > 1e-6 ? std::min(frac, 0.02 / len) : 0.0;  // 2 cm in front of the face
			a_t = static_cast<float>(frac);
			double mc[3], mn[3] = { 0.0, 0.0, 0.0 };
			for (int k = 0; k < 3; ++k) {
				mc[k] = from[k] + (to[k] - from[k]) * (frac - back);
			}
			a_face = axis < 0 ? -1 : axis == 1 ? (sign > 0 ? 1 : 0) : axis == 2 ? (sign > 0 ? 3 : 2) : (sign > 0 ? 5 : 4);
			if (axis >= 0) {
				mn[axis] = sign;
			} else {
				// Started inside a block: facing back along the shot.
				for (int k = 0; k < 3; ++k) {
					mn[k] = len > 1e-6 ? -(to[k] - from[k]) / len : 0.0;
				}
			}
			const GtaVec g = McToGta(mc[0], mc[1], mc[2]), gn = McToGta(mn[0], mn[1], mn[2]);
			a_at[0] = static_cast<float>(g.x), a_at[1] = static_cast<float>(g.y), a_at[2] = static_cast<float>(g.z);
			a_n[0] = static_cast<float>(gn.x), a_n[1] = static_cast<float>(gn.y), a_n[2] = static_cast<float>(gn.z);
			return true;
		}

		// How close (m) the part of a shot from a_a to a_b passes to the player's chest.
		float MissDistanceToPlayer(const float a_a[3], const float a_b[3])
		{
			CPed* p = FindPlayerPed();
			if (!p || !p->m_pMatrix) {
				return 1e9f;
			}
			const float c[3] = { p->m_pMatrix->pos.x, p->m_pMatrix->pos.y, p->m_pMatrix->pos.z + 0.3f };
			float       d[3], w[3], dd = 0.0f, wd = 0.0f;
			for (int k = 0; k < 3; ++k) {
				d[k] = a_b[k] - a_a[k];
				w[k] = c[k] - a_a[k];
				dd += d[k] * d[k];
				wd += w[k] * d[k];
			}
			const float u = dd > 1e-8f ? std::clamp(wd / dd, 0.0f, 1.0f) : 0.0f;
			float       r = 0.0f;
			for (int k = 0; k < 3; ++k) {
				const float e = a_a[k] + d[k] * u - c[k];
				r += e * e;
			}
			return std::sqrt(r);
		}

		int __cdecl BulletProbe(float* a_from, float* a_to, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7, std::uint32_t a8,
			std::uint32_t a9, std::uint32_t a10, std::uint32_t a11, std::uint32_t a12, std::uint32_t a13)
		{
			bullets.probes.fetch_add(1, std::memory_order_relaxed);
			float at[3], n[3], t = 1.0f, end[3] = { 0.0f, 0.0f, 0.0f };
			int   face = -1;
			const bool blocked = a_from && a_to && Config::Get().npcBlocks && ClipShot(a_from, a_to, at, n, t, face);
			if (blocked) {
				std::copy(a_to, a_to + 3, end);
				std::copy(at, at + 3, a_to);  // GTA traces (and draws) the shot only up to the blocks
			}
			const int hits = bulletOriginal(a_from, a_to, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
			if (blocked) {
				if (hits > 0) {
					bullets.nearerHit.fetch_add(1, std::memory_order_relaxed);  // something in front of the blocks took it
				} else {
					bullets.stopped.fetch_add(1, std::memory_order_relaxed);
					if (t <= 0.0f) {
						bullets.inside.fetch_add(1, std::memory_order_relaxed);
					}
					const float miss = MissDistanceToPlayer(at, end);
					if (miss < 0.6f) {
						bullets.forPlayer.fetch_add(1, std::memory_order_relaxed);
					}
					if (Config::Get().diagnostics) {
						LC_LOG_EVERY(250, "a GTA bullet stopped at a Minecraft block: from %.2f %.2f %.2f toward %.2f %.2f %.2f, at %.2f %.2f %.2f (face %+.0f %+.0f %+.0f), "
										  "%.1f m short of its end%s",
							a_from[0], a_from[1], a_from[2], end[0], end[1], end[2], at[0], at[1], at[2], n[0], n[1], n[2],
							std::sqrt((end[0] - at[0]) * (end[0] - at[0]) + (end[1] - at[1]) * (end[1] - at[1]) + (end[2] - at[2]) * (end[2] - at[2])),
							miss < 0.6f ? "; it was headed for the player" : "");
					}
					std::lock_guard lock(impactLock);
					if (impacts.size() < 64) {
						impacts.push_back({ { at[0], at[1], at[2] }, { n[0], n[1], n[2] }, face });
					}
				}
			}
			return hits;
		}

		void HookBullets()
		{
			if (bulletHook >= 0) {
				return;
			}
			bulletHook = 0;
			if (plugin::gameVer != plugin::VERSION_1080) {
				LC_LOG("bullets: not GTA IV 1.0.8.0: Minecraft's blocks don't stop GTA's gunfire");
				return;
			}
			auto* at = reinterpret_cast<std::uint8_t*>(AddressSetter::gBaseAddress) + (0x92DAA0 - 0x400000);
			// push ebp; mov ebp, esp; and esp, -16; mov eax, 0x87D4; call (the stack probe)
			static constexpr std::uint8_t kExpect[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xB8, 0xD4, 0x87, 0x00, 0x00, 0xE8 };
			if (std::memcmp(at, kExpect, sizeof(kExpect)) != 0) {
				LC_LOG("bullets: GTA's bullet trace isn't as expected: Minecraft's blocks don't stop GTA's gunfire");
				return;
			}
			auto* tramp = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!tramp) {
				LC_LOG("bullets: can't allocate the trampoline (error %lu)", ::GetLastError());
				return;
			}
			const auto abs32 = [](const void* a_p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_p)); };
			// The first 11 bytes (no relative addresses among them), then on into the original.
			std::memcpy(tramp, at, 11);
			tramp[11] = 0xE9;
			const std::uint32_t back = abs32(at + 11) - (abs32(tramp) + 16u);
			std::memcpy(tramp + 12, &back, 4);
			::FlushInstructionCache(::GetCurrentProcess(), tramp, 16);
			bulletOriginal = reinterpret_cast<BulletProbeFn>(tramp);
			std::uint8_t patch[11];
			std::memset(patch, 0x90, sizeof(patch));
			patch[0] = 0xE9;
			const std::uint32_t to = abs32(reinterpret_cast<const void*>(&BulletProbe)) - (abs32(at) + 5u);
			std::memcpy(patch + 1, &to, 4);
			DWORD old = 0;
			if (!::VirtualProtect(at, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
				LC_LOG("bullets: can't hook GTA's bullet trace (VirtualProtect error %lu)", ::GetLastError());
				return;
			}
			std::memcpy(at, patch, sizeof(patch));
			::VirtualProtect(at, sizeof(patch), old, &old);
			::FlushInstructionCache(::GetCurrentProcess(), at, sizeof(patch));
			bulletHook = 1;
			LC_LOG("bullets: Minecraft's solid blocks stop GTA's gunfire (bullet trace hooked)");
		}

		// Game thread (script): the impacts the bullets left on the blocks since last frame.
		void SpawnImpacts()
		{
			std::vector<Impact> now;
			{
				std::lock_guard lock(impactLock);
				now.swap(impacts);
			}
			for (const Impact& i : now) {
				// The effect's up axis along the face's normal: pitched up 90 degrees and turned to face
				// it for a wall, upright on a top face, upside down under a block.
				float rx = 0.0f, rz = 0.0f;
				if (i.n[2] > 0.5f) {
					rx = 0.0f;
				} else if (i.n[2] < -0.5f) {
					rx = 180.0f;
				} else {
					rx = -90.0f;
					rz = std::atan2(-i.n[0], i.n[1]) * kRadToDeg;
				}
				const float scale = 1.0f;  // (the native's last argument is a float scale; the SDK types it unsigned)
				unsigned    scaleBits = 0;
				std::memcpy(&scaleBits, &scale, sizeof(scaleBits));
				S::TRIGGER_PTFX("imp_bullet_concrete", i.at[0], i.at[1], i.at[2], rx, 0.0f, rz, scaleBits);
				// GTA's effects are drawn before Minecraft's blocks (and don't write depth), so the block
				// itself would hide this one: Minecraft shows the block's own hit particles too.
				if (i.face >= 0) {
					const McVec m = GtaToMc(i.at[0], i.at[1], i.at[2]);
					Link::Get().PushInput(proto::kInBulletImpact, static_cast<std::uint16_t>(i.face), static_cast<std::int32_t>(std::lround(m.x * 256.0)),
						static_cast<std::int32_t>(std::lround(m.y * 256.0)), static_cast<std::int32_t>(std::lround(m.z * 256.0)));
				}
				++impactsSpawned;
			}
		}

		// DebugBulletWall=N (not in the default ini): N s into play, a ped with a pistol stands 1.5 m to
		// the player's right and another 6 m ahead of the player; every 1.2 s the first shoots at the
		// second (tools/fake_minecraft.py --wall-ring puts a wall of blocks between them): its shots
		// must stop at the wall's inner face, in front of the player's camera, and the second ped
		// must stay unhurt (its health is logged).
		struct WallShooter
		{
			float timer = 0.0f, shootTimer = 0.0f;
			int   stage = 0;  // 0 waiting, 1 standing there, 2 gone
			int   ped = 0, target = 0;
		} wallShooter;

		int PlacePed(float a_x, float a_y, float a_z, float a_heading)
		{
			int ped = 0;
			S::CREATE_RANDOM_CHAR(a_x, a_y, a_z, &ped);
			if (!ped) {
				return 0;
			}
			float ground = a_z - 1.0f;
			S::GET_GROUND_Z_FOR_3D_COORD(a_x, a_y, a_z + 1.0f, &ground);
			S::SET_CHAR_COORDINATES(ped, a_x, a_y, ground);
			S::SET_CHAR_HEADING(ped, a_heading);
			S::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, true);
			return ped;
		}

		void WallShooterHook(const Combat::Frame& a_frame)
		{
			auto& w = wallShooter;
			if (Config::Get().debugBulletWall <= 0 || w.stage >= 2 || !a_frame.exists || a_frame.loading || a_frame.dead || !a_frame.mc) {
				return;
			}
			w.timer += a_frame.dt;
			if (w.stage == 0) {
				if (w.timer < static_cast<float>(Config::Get().debugBulletWall)) {
					return;
				}
				float x = 0, y = 0, z = 0;
				S::GET_CHAR_COORDINATES(a_frame.ped, &x, &y, &z);
				const float hd = McYawToGtaHeading(a_frame.mc->yaw), h = hd * kDegToRad;
				const float fx = -std::sin(h), fy = std::cos(h), rx = std::cos(h), ry = std::sin(h);
				w.ped = PlacePed(x + rx * 1.5f, y + ry * 1.5f, z, hd);
				w.target = PlacePed(x + fx * 6.0f, y + fy * 6.0f, z, hd + 180.0f);
				if (!w.ped || !w.target) {
					LC_LOG("DebugBulletWall: CREATE_RANDOM_CHAR failed");
					w.stage = 2;
					return;
				}
				S::FREEZE_CHAR_POSITION(w.target, true);
				S::GIVE_WEAPON_TO_CHAR(w.ped, 7, 1000, false);  // a pistol
				S::SET_CURRENT_CHAR_WEAPON(w.ped, 7, true);
				S::SET_CHAR_ACCURACY(w.ped, 100);
				LC_LOG("DebugBulletWall: ped %d with a pistol 1.5 m right of the player, ped %d 6 m ahead (player at GTA %.2f %.2f %.2f, heading %.0f)", w.ped,
					w.target, x, y, z, hd);
				w.stage = 1;
				w.shootTimer = 1.0f;
				return;
			}
			if (!S::DOES_CHAR_EXIST(w.ped) || S::IS_CHAR_DEAD(w.ped) || !S::DOES_CHAR_EXIST(w.target)) {
				LC_LOG("DebugBulletWall: a ped is gone");
				w.stage = 2;
				return;
			}
			if ((w.shootTimer -= a_frame.dt) > 0.0f) {
				return;
			}
			w.shootTimer = 1.2f;
			unsigned health = 0;
			S::GET_CHAR_HEALTH(w.target, &health);
			S::TASK_SHOOT_AT_CHAR(w.ped, w.target, 2000, 2);
			LC_LOG("DebugBulletWall: ped %d shoots at ped %d (health %u%s)", w.ped, w.target, health, S::IS_CHAR_DEAD(w.target) ? ", dead" : "");
		}

		void LogStats()
		{
			const auto now = ::GetTickCount64();
			if (lastStatsMs == 0) {
				lastStatsMs = now;
				return;
			}
			const bool diag = Config::Get().diagnostics;
			if (now - lastStatsMs < (diag ? 2000ull : 10000ull)) {
				return;
			}
			const double secs = double(now - lastStatsMs) / 1000.0;
			lastStatsMs = now;
			const auto& c = counters;
			const std::uint32_t shots = bullets.probes.exchange(0), stopped = bullets.stopped.exchange(0), nearer = bullets.nearerHit.exchange(0),
								forPlayer = bullets.forPlayer.exchange(0), inside = bullets.inside.exchange(0);
			if (stopped || (diag && shots)) {
				LC_LOG("bullets %.0fs: %u traced, %u stopped by Minecraft's blocks (%u headed for the player, %u fired from inside one), %u hit something in "
					   "front of them first; %u impacts shown",
					secs, shots, stopped, forPlayer, inside, nearer, impactsSpawned);
			}
			impactsSpawned = 0;
			if (diag || c.pedsPushed || c.carsTouching || c.clears) {
				std::uint32_t dropped = 0;
				{
					std::lock_guard lock(pendingLock);
					dropped = pendingDropped;
					pendingDropped = 0;
				}
				LC_LOG("stats %.0fs: %zu solid sections (%u in, %u clears%s); peds pushed out %u times (%u detour steps), vehicles touching blocks %u "
					   "frames, slowed %u (fastest %.1f m/s into them, %u by forward speed), moved back out %u; %.0f us/frame",
					secs, grid.Sections(), c.sectionsIn, c.clears, dropped ? ", some dropped: render ring burst" : "", c.pedsPushed, c.pedsDetoured,
					c.carsTouching, c.carsSlowed, c.fastestStopped, c.velocityFallbacks, c.carsPushedOut, c.frames ? c.us / c.frames : 0.0);
			}
			counters = {};
		}
	}

	void OnSolids(const std::uint8_t* a_data, std::uint32_t a_bytes)
	{
		if (!a_data || a_bytes < sizeof(proto::RenSolids)) {
			return;
		}
		proto::RenSolids hdr{};
		std::memcpy(&hdr, a_data, sizeof(hdr));
		PendingSection p{};
		p.sx = hdr.sx, p.sy = hdr.sy, p.sz = hdr.sz;
		p.remove = hdr.count == 0 || a_bytes < sizeof(proto::RenSolids) + 512;
		if (!p.remove) {
			std::memcpy(p.bits.data(), a_data + sizeof(proto::RenSolids), 512);
		}
		std::lock_guard lock(pendingLock);
		if (pending.size() >= kMaxPending) {
			++pendingDropped;
			return;
		}
		pending.push_back(p);
	}

	void Clear()
	{
		std::lock_guard lock(pendingLock);
		pending.clear();
		pendingClear = true;
	}

	bool ModelBox(int a_vehicle, std::int32_t a_modelIndex, float a_min[3], float a_max[3])
	{
		const Dims& d = DimsOf(a_modelIndex, a_vehicle);
		std::copy(std::begin(d.min), std::end(d.min), a_min);
		std::copy(std::begin(d.max), std::end(d.max), a_max);
		return d.ok;
	}

	void Tick(const Combat::Frame& a_frame)
	{
		std::lock_guard lock(gridLock);
		ApplyPending();
		if (Config::Get().npcBlocks) {
			HookBullets();
			SpawnImpacts();
		}
		WallShooterHook(a_frame);
		if (!Config::Get().npcBlocks || grid.Empty() || !a_frame.exists || a_frame.loading) {
			LogStats();
			return;
		}
		const auto t0 = Qpc();
		++counters.frames;
		float px = 0, py = 0, pz = 0;
		S::GET_CHAR_COORDINATES(a_frame.ped, &px, &py, &pz);
		PushPeds(a_frame, px, py);
		BlockVehicles(a_frame, px, py);
		counters.us += double(Qpc() - t0) / QpcPerUs();
		LogStats();
	}
}
