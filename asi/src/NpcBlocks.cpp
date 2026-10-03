// Unity-built into dllmain.cpp (needs IV-SDK). See NpcBlocks.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "blocks"
#include "NpcBlocks.h"

#include "Combat.h"
#include "Config.h"
#include "Coords.h"
#include "Log.h"
#include "combat/BlockPush.h"

#include <algorithm>
#include <array>
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
		blocks::SolidGrid grid;
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
				detours.clear();
				++counters.clears;
			}
			for (const auto& p : applying) {
				grid.Set(p.sx, p.sy, p.sz, p.remove ? nullptr : p.bits.data());
			}
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
		ApplyPending();
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
