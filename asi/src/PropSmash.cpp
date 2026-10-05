// Unity-built into dllmain.cpp (needs IV-SDK). See PropSmash.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "smash"
#include "PropSmash.h"

#include "Collision.h"
#include "Config.h"
#include "Coords.h"
#include "Game.h"
#include "HostDrive.h"
#include "Link.h"
#include "Log.h"
#include "collision/Objects.h"
#include "collision/Rays.h"
#include "drive/PropHit.h"
#include "drive/VehicleHit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace lc::PropSmash
{
	namespace
	{
		namespace S = ::Scripting;
		namespace proto = ::libertycraft::proto;
		namespace P = ::lc::drive::prop;
		static_assert(sizeof(P::Box) == sizeof(col::OBox), "drive/PropHit.h Box must match collision/Objects.h OBox");

		constexpr float         kHorizon = 0.45f;   // s: a prop the mover will run into this soon leaves Minecraft's collision
		constexpr float         kMargin = 0.05f;    // m around the prop's boxes
		constexpr float         kContactLead = 0.04f;  // s: contact this close counts as now (the event is a tick old)
		constexpr std::uint64_t kMoverFreshMs = 160;   // a mover event older than this: the mover stopped (or Minecraft did)
		constexpr std::uint64_t kArmedMaxMs = 1500, kArmedLostMs = 300;
		constexpr float         kMinPropHeight = 0.4f;  // m: lower things (debris, kerb stones) are stepped over

		struct MoverState
		{
			bool          valid = false;
			std::uint64_t atMs = 0;
			P::Mover      m;
			float         speed = 0.0f;
			std::uint32_t flags = 0;
		} mover;

		struct PropInfo
		{
			int          handle = 0;
			std::int32_t model = -1;
			float        mass = 0.0f;
			bool         physics = false;
			bool         slowLogged = false;
		};
		std::unordered_map<const void*, PropInfo> infos;

		struct Armed
		{
			std::uint64_t atMs = 0, seenMs = 0;
			float         t = -1.0f;  // the last contact time ahead (s; log)
		};
		std::unordered_map<const void*, Armed>         armed;
		std::unordered_map<const void*, std::uint64_t> broken;  // when (ms)

		// A broken prop's flight, logged for a few seconds.
		struct Watch
		{
			int           handle = 0;
			std::uint64_t atMs = 0;
			float         start[3]{};
			int           logs = 0;
			std::string   what;
		};
		std::vector<Watch> watching;

		struct Counters
		{
			std::uint32_t movers = 0, armed = 0, disarmed = 0, broken = 0, tooSlow = 0, crashes = 0, knocks = 0;
		} counters;
		std::uint64_t nextStats = 0;

		float FloatBits(std::uint32_t a_u)
		{
			float f;
			std::memcpy(&f, &a_u, sizeof f);
			return f;
		}

		// Where a prop stands: the centre of its lowest box (MC x/z), and its height range (MC y).
		void Foot(const P::Box* a_b, std::size_t a_n, float& a_x, float& a_z, float& a_lo, float& a_hi)
		{
			std::size_t low = 0;
			a_lo = 1e9f, a_hi = -1e9f;
			for (std::size_t i = 0; i < a_n; ++i) {
				if (a_b[i].y0 < a_lo) {
					a_lo = a_b[i].y0;
					low = i;
				}
				a_hi = std::max(a_hi, a_b[i].y1);
			}
			const P::Box& b = a_b[low];
			const float   a = (b.a0 + b.a1) * 0.5f, c = (b.b0 + b.b1) * 0.5f;
			a_x = b.o[0] + a * b.u[0] + c * b.v[0];
			a_z = b.o[1] + a * b.u[1] + c * b.v[1];
		}

		// The pool handle and physics of a tracked object (the key is its CObject).
		PropInfo& Info(const void* a_key, std::int32_t a_model)
		{
			auto [it, fresh] = infos.try_emplace(a_key);
			PropInfo& pi = it->second;
			if (!fresh && (pi.model != a_model || (pi.handle && !S::DOES_OBJECT_EXIST(pi.handle)))) {
				pi = PropInfo{};  // (another object in that slot now)
				fresh = true;
			}
			if (fresh) {
				pi.model = a_model;
				auto*    pool = CPools::ms_pObjectPool;
				CObject* obj = static_cast<CObject*>(const_cast<void*>(a_key));
				if (pool && obj->m_nModelIndex == a_model) {
					const int handle = static_cast<int>(pool->GetIndex(obj));
					if (handle && S::DOES_OBJECT_EXIST(handle)) {
						pi.handle = handle;
						pi.physics = S::DOES_OBJECT_HAVE_PHYSICS(handle);
						S::GET_OBJECT_MASS(handle, &pi.mass);
					}
				}
			}
			return pi;
		}

		// The object's physics state (log): its class bits ((+0x28 >> 6) & 15) and its physics level state (0
		// active, 1 inactive, 2 fixed), read as 1.0.8.0 keeps them; -1: unknown.
		void PhysicsState(int a_handle, int& a_class, int& a_level)
		{
			a_class = a_level = -1;
			CObject* obj = CPools::ms_pObjectPool ? CPools::ms_pObjectPool->GetAt(static_cast<std::uint32_t>(a_handle)) : nullptr;
			if (!obj || plugin::gameVer != plugin::VERSION_1080) {
				return;
			}
			__try {
				const auto* o = reinterpret_cast<const std::uint8_t*>(obj);
				a_class = static_cast<int>((*reinterpret_cast<const std::uint32_t*>(o + 0x28) >> 6) & 15u);
				const auto* inst = *reinterpret_cast<const std::uint8_t* const*>(o + 0x38);
				if (inst) {
					const std::uint16_t idx = *reinterpret_cast<const std::uint16_t*>(inst + 8);
					const auto*         level = *reinterpret_cast<const std::uint8_t* const*>(AddressSetter::gBaseAddress + 0xE75AA8);
					if (idx != 0xFFFF && level) {
						const auto* states = *reinterpret_cast<const std::uint8_t* const*>(level + 0x70);
						a_level = static_cast<int>(*reinterpret_cast<const std::uint32_t*>(states + 8u * idx + 4) & 3u);
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_class = a_level = -1;
			}
		}

		// CPhysical +0x24 (1.0.8.0): bit 0 lets the physics wake it (an activation is refused without it, and with
		// any of the freeze bits 0x1C); read and written guarded. -1: unknown.
		std::int64_t PhysicalFlags(int a_handle, std::uint32_t a_set = 0, std::uint32_t a_clear = 0)
		{
			CObject* obj = CPools::ms_pObjectPool ? CPools::ms_pObjectPool->GetAt(static_cast<std::uint32_t>(a_handle)) : nullptr;
			if (!obj || plugin::gameVer != plugin::VERSION_1080) {
				return -1;
			}
			std::int64_t was = -1;
			__try {
				auto* f = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(obj) + 0x24);
				was = *f;
				*f = (*f | a_set) & ~a_clear;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				was = -1;
			}
			return was;
		}

		// A prop tipping over about its base (its origin), as a rod falls: angular acceleration 3 g sin(angle) /
		// (2 length), from the spin the mover's hit gave it, until it lies (kToppledAngle).
		constexpr float kToppledAngle = 1.48f;  // rad (85 degrees: the lamp's head or the light's arm meets the ground)
		struct Topple
		{
			int   handle = 0;
			float q0[4]{};    // its orientation before (x, y, z, w)
			float pos[3]{};
			float axis[2]{};  // horizontal rotation axis (GTA x, y): up x the tip direction
			float length = 1.0f, angle = 0.02f, spin = 1.0f;
			std::uint64_t atMs = 0;
		};
		std::vector<Topple> topples;
		struct
		{
			int   handle = 0;
			float dir[2]{};  // GTA x/y: which way it tipped over
			float length = 0.0f;
		} lastTopple;  // (DebugPropSmash "view")

		void StartTopple(int a_h, float a_dx, float a_dy, float a_length, float a_spin)
		{
			Topple t;
			t.handle = a_h;
			S::GET_OBJECT_QUATERNION(a_h, &t.q0[0], &t.q0[1], &t.q0[2], &t.q0[3]);
			S::GET_OBJECT_COORDINATES(a_h, &t.pos[0], &t.pos[1], &t.pos[2]);
			t.axis[0] = -a_dy, t.axis[1] = a_dx;  // up x (dx, dy, 0)
			t.length = a_length;
			t.spin = a_spin;
			t.atMs = ::GetTickCount64();
			topples.push_back(t);
		}

		void ToppleTick(float a_dt)
		{
			for (auto it = topples.begin(); it != topples.end();) {
				Topple& t = *it;
				if (!S::DOES_OBJECT_EXIST(t.handle) || ::GetTickCount64() - t.atMs > 6000) {
					it = topples.erase(it);
					continue;
				}
				const float dt = std::min(a_dt, 0.05f);
				t.spin += 1.5f * 9.81f / t.length * std::sin(t.angle) * dt;
				t.angle = std::min(t.angle + t.spin * dt, kToppledAngle);
				// q = rotation(axis, angle) * q0
				const float h = t.angle * 0.5f, sn = std::sin(h), cs = std::cos(h);
				const float a[4] = { t.axis[0] * sn, t.axis[1] * sn, 0.0f, cs };
				const float* b = t.q0;
				const float q[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
					a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
				S::SET_OBJECT_QUATERNION(t.handle, q[0], q[1], q[2], q[3]);
				S::SET_OBJECT_COORDINATES(t.handle, t.pos[0], t.pos[1], t.pos[2]);
				if (t.angle >= kToppledAngle) {
					float up[3]{};
					if (auto* obj = CPools::ms_pObjectPool ? CPools::ms_pObjectPool->GetAt(static_cast<std::uint32_t>(t.handle)) : nullptr; obj && obj->m_pMatrix) {
						up[0] = obj->m_pMatrix->at.x, up[1] = obj->m_pMatrix->at.y, up[2] = obj->m_pMatrix->at.z;
					}
					LC_LOG("prop %d lies: tipped over in %.2f s, its up axis now %.2f %.2f %.2f (tipped toward %.2f %.2f)", t.handle,
						(::GetTickCount64() - t.atMs) / 1000.0, up[0], up[1], up[2], t.axis[1], -t.axis[0]);
					it = topples.erase(it);
					continue;
				}
				++it;
			}
		}

		void Disarm(const void* a_key, const char* a_why)
		{
			if (armed.erase(a_key)) {
				Collision::Get().SetObjectPassable(a_key, false);
				++counters.disarmed;
				LC_LOG_EVERY(1000, "prop %p back in Minecraft's collision (%s)", a_key, a_why);
			}
		}

		void DisarmAll(const char* a_why)
		{
			while (!armed.empty()) {
				Disarm(armed.begin()->first, a_why);
			}
		}

		void Break(const void* a_key, const PropInfo& a_pi, const P::Mover& a_m, const P::Box* a_boxes, std::size_t a_count, std::uint32_t a_hash)
		{
			const int h = a_pi.handle;
			if (!h || !S::DOES_OBJECT_EXIST(h)) {
				Disarm(a_key, "gone");
				return;
			}
			const bool  riding = (mover.flags & proto::kMoverRiding) != 0;
			const float moverMass = riding ? P::kMountMass : P::kPlayerMass;
			const float keep = P::Keep(moverMass, a_pi.mass);
			const float vProp = P::PropSpeed(moverMass, a_pi.mass, mover.speed, keep);
			// Along the mover's way (GTA axes), tipping over about its base where the mover struck it.
			float       gx = a_m.vel[0], gy = -a_m.vel[2];
			const float len = std::max(std::hypot(gx, gy), 1e-3f);
			gx /= len, gy /= len;
			float base = 1e9f, top = -1e9f, cx = 0.0f, cz = 0.0f;
			for (std::size_t i = 0; i < a_count; ++i) {
				base = std::min(base, a_boxes[i].y0);
				top = std::max(top, a_boxes[i].y1);
			}
			cx = a_boxes[0].o[0], cz = a_boxes[0].o[1];
			const float hit = std::clamp(a_m.bottom[1] + a_m.height * 0.5f - base, 0.5f, std::max(top - base, 0.5f));
			const float w = vProp / hit;
			// Street furniture GTA IV keeps in place until a vehicle uproots it (the object class 4 of lamp posts and
			// traffic lights: no script push moves it, measured) tips over about its base along the mover's way,
			// as a rod falls (Topple); anything else (bins, cones, small signs) goes to GTA IV's physics: dynamic,
			// and an impulse at the height it was struck (APPLY_FORCE_TO_OBJECT, 1.0.8.0 read from its code: the
			// 9th argument is the fragment component, the 10th turns the force into the object's frame, the 11th
			// the offset, the 12th multiplies the force by the object's mass: a change of speed; type 3 an impulse).
			int cls = -1, lvl = -1;
			PhysicsState(h, cls, lvl);
			const bool topples = cls == 4 && top - base >= 1.5f;
			if (topples) {
				StartTopple(h, gx, gy, std::max(top - base, 1.0f), std::clamp(w, 0.6f, 3.0f));
				lastTopple.handle = h, lastTopple.dir[0] = gx, lastTopple.dir[1] = gy, lastTopple.length = top - base;
			} else {
				S::FREEZE_OBJECT_POSITION(h, false);
				S::SET_OBJECT_DYNAMIC(h, true);
				S::APPLY_FORCE_TO_OBJECT(h, 3, gx * vProp, gy * vProp, 0.25f * vProp, 0.0f, 0.0f, hit, 0, 0, 1, 1);
			}
			Link::Get().PushInput(proto::kInPropHit, static_cast<std::uint16_t>(std::lround(keep * 100.0f)) & proto::kPropKeepMask,
				static_cast<std::int32_t>(std::lround(cx * 256.0f)), static_cast<std::int32_t>(std::lround(base * 256.0f)),
				static_cast<std::int32_t>(std::lround(cz * 256.0f)));
			armed.erase(a_key);
			broken[a_key] = ::GetTickCount64();
			++counters.broken;
			char what[160];
			std::snprintf(what, sizeof what, "%s at %.1f m/s broke prop %d (hash 0x%08X, %.0f kg, %.1f m tall)",
				riding ? "a Minecraft mount" : (mover.flags & proto::kMoverFlying) ? "elytra flight" : "a sprint", mover.speed, h, a_hash, a_pi.mass, top - base);
			LC_LOG("%s: %s at %.1f m/s (%.1f rad/s, struck %.1f m up; object class %d), the mover keeps %.0f%% of its speed", what,
				topples ? "tips over" : "pushed off", vProp, w, hit, cls, keep * 100.0f);
			Watch wt;
			wt.handle = h;
			wt.atMs = ::GetTickCount64();
			S::GET_OBJECT_COORDINATES(h, &wt.start[0], &wt.start[1], &wt.start[2]);
			wt.what = what;
			watching.push_back(wt);
		}

		// Where a broken prop went (log): 0.25, 0.5, 1 and 2.5 s on, how far and how tipped over.
		void WatchTick()
		{
			const auto now = ::GetTickCount64();
			for (auto it = watching.begin(); it != watching.end();) {
				const std::uint64_t marks[4] = { 250, 500, 1000, 2500 };
				if (it->logs < 4 && now - it->atMs >= marks[it->logs]) {
					++it->logs;
					float p[3]{}, v[3]{};
					float upZ = 1.0f;
					if (S::DOES_OBJECT_EXIST(it->handle)) {
						S::GET_OBJECT_COORDINATES(it->handle, &p[0], &p[1], &p[2]);
						S::GET_OBJECT_VELOCITY(it->handle, &v[0], &v[1], &v[2]);
						if (auto* obj = CPools::ms_pObjectPool ? CPools::ms_pObjectPool->GetAt(static_cast<std::uint32_t>(it->handle)) : nullptr; obj && obj->m_pMatrix) {
							upZ = obj->m_pMatrix->at.z;  // (IV-SDK's rows: "at" is up)
						}
					}
					LC_LOG("%s: %.2f s on it is %.1f m from where it stood, tipped %.0f degrees, moving %.1f m/s", it->what.c_str(), (now - it->atMs) / 1000.0,
						std::sqrt((p[0] - it->start[0]) * (p[0] - it->start[0]) + (p[1] - it->start[1]) * (p[1] - it->start[1]) + (p[2] - it->start[2]) * (p[2] - it->start[2])),
						std::acos(std::clamp(upZ, -1.0f, 1.0f)) * kRadToDeg, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
				}
				it = it->logs >= 4 ? watching.erase(it) : std::next(it);
			}
		}

		void Stats()
		{
			const auto now = ::GetTickCount64();
			if (now < nextStats) {
				return;
			}
			nextStats = now + 10000;
			if (counters.movers || counters.crashes) {
				LC_LOG("stats 10s: %u fast mover ticks; props let through %u (put back %u), knocked over %u, too slow for %u; elytra crashes %u (knocked over %u)",
					counters.movers, counters.armed, counters.disarmed, counters.broken, counters.tooSlow, counters.crashes, counters.knocks);
			}
			counters = {};
		}

		// ---- DebugPropSmash (test hook) ---------------------------------------------------------------
		// "SECONDS:KIND|..." (seconds after puppet mode first starts): KIND pole (an elytra flight into the
		// nearest tall prop with a clear run up: the player is put 5.5 m up, 22 m from it, facing it, falls,
		// opens the elytra and fires a rocket), light (the same into a traffic light; none here: the player is
		// put next to the nearest one GTA IV has streamed first), wall (the same into a building's wall), fall
		// (a plain fall from 10 m, no elytra), break (topples the nearest tall props, no mover), horse (put 30 m
		// from a tall prop, facing it; the autorun has him mount a fast horse, then W is held for 4 s), view (a
		// camera of ours on the last prop knocked over, for 4 s).
		struct TestStep
		{
			float       at = 0.0f;
			std::string kind;
		};
		struct Test
		{
			bool                  parsed = false;
			std::vector<TestStep> steps;
			std::size_t           next = 0;
			float                 t = -1.0f;  // since puppet mode first started
			int                   phase = 0;  // 0 waiting, 1 put there, 2 launching
			float                 phaseT = 0.0f;
			int                   launch = 0;
			float                 launchT = 0.0f;
			bool                  relocated = false;  // "light": put next to a traffic light farther away once
			float                 retryAt = 0.0f;
		} test;

		bool ClearLine(const float a_from[3], const float a_to[3])
		{
			tLineOfSightResults res;
			--col::rayCounters.rays;
			return !col::CastGta(a_from, a_to, res, col::kLosFlags | OBJECTS | VEHICLES);
		}

		// GTA IV's traffic lights (their model hashes, from the game's item definitions): "light" aims at one.
		constexpr std::uint32_t kTrafficLights[] = { 0x64B0E81F, 0x5A3C4365, 0x274E5EAE, 0xC95D9FD2, 0x675A5BCD, 0x957E3818, 0x3C670D6F, 0x124628BC,
			0x4C60AD62, 0xE0494133, 0x91604D17 };
		bool IsTrafficLight(std::uint32_t a_hash)
		{
			return std::find(std::begin(kTrafficLights), std::end(kTrafficLights), a_hash) != std::end(kTrafficLights);
		}

		// The nearest traffic light in the object pool (anywhere GTA IV has streamed) farther than a_min metres
		// from a_at: its position (GTA).
		bool NearestPoolLight(const float a_at[3], float a_min, float a_out[3])
		{
			auto* pool = CPools::ms_pObjectPool;
			if (!pool) {
				return false;
			}
			float best = 1e18f;
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CObject* obj = pool->Get(slot);
				if (!obj || !obj->m_pMatrix || obj->m_nModelIndex < 0 || obj->m_nModelIndex >= 31000) {
					continue;
				}
				CBaseModelInfo* mi = CModelInfo::ms_modelInfoPtrs[obj->m_nModelIndex];
				if (!mi || !IsTrafficLight(mi->m_nHash)) {
					continue;
				}
				const auto& p = obj->m_pMatrix->pos;
				const float d2 = (p.x - a_at[0]) * (p.x - a_at[0]) + (p.y - a_at[1]) * (p.y - a_at[1]);
				if (d2 > a_min * a_min && d2 < best) {
					best = d2;
					a_out[0] = p.x, a_out[1] = p.y, a_out[2] = p.z;
				}
			}
			return best < 1e17f;
		}

		// A tall prop near a_at (a_lights: traffic lights only) with a clear, level run up of a_run metres: where
		// to stand and which way. Clear: no building along the way, and none of Collision's objects in the way
		// of a mover a_halfWidth wide between a_lo and a_hi metres over the ground.
		bool FindPole(const float a_at[3], float a_run, bool a_lights, float a_halfWidth, float a_lo, float a_hi, float a_out[4], std::string& a_what)
		{
			struct Cand
			{
				float d2;
				float base[3];  // GTA
				float top;
				const void* key;
				std::int32_t model;
				std::uint32_t hash;
			};
			struct Other
			{
				const void*         key;
				std::vector<P::Box> boxes;
			};
			std::vector<Cand>  cands;
			std::vector<Other> others;
			Collision::Get().ForEachSolidObject([&](const Collision::ObjectView& a_v) {
				const auto* b = static_cast<const P::Box*>(a_v.boxes);
				others.push_back({ a_v.key, std::vector<P::Box>(b, b + a_v.count) });
				float fx = 0.0f, fz = 0.0f, lo = 0.0f, hi = 0.0f;
				Foot(b, a_v.count, fx, fz, lo, hi);
				if (hi - lo < 2.5f || broken.count(a_v.key) || (a_lights && !IsTrafficLight(a_v.hash))) {
					return;
				}
				const GtaVec g = McToGta(fx, lo, fz);
				const float  dx = float(g.x) - a_at[0], dy = float(g.y) - a_at[1];
				cands.push_back({ dx * dx + dy * dy, { float(g.x), float(g.y), float(g.z) }, hi - lo, a_v.key, a_v.model, a_v.hash });
			});
			std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
			int why[3]{};  // ground, a building, an object (log)
			for (const auto& c : cands) {
				const PropInfo& pi = Info(c.key, c.model);
				if (!pi.handle || P::BreakSpeed(pi.mass) > 20.0f) {
					continue;
				}
				for (int k = 0; k < 16; ++k) {
					const float a = static_cast<float>(k) * kPi / 8.0f, ox = std::cos(a), oy = std::sin(a);
					const float sx = c.base[0] + ox * a_run, sy = c.base[1] + oy * a_run;
					float       ground = 0.0f;
					S::GET_GROUND_Z_FOR_3D_COORD(sx, sy, c.base[2] + 3.0f, &ground);
					if (!(std::fabs(ground - c.base[2]) < 1.0f)) {
						++why[0];
						continue;
					}
					// no building along the way, also a metre and a half to either side (no alleys)
					bool clear = true;
					for (const float side : { 0.0f, -1.5f, 1.5f }) {
						const float px = -oy * side, py = ox * side;
						for (const float h : { 0.3f, 0.6f, 1.6f, 3.0f, 4.5f }) {
							const float from[3] = { sx + px, sy + py, ground + h }, to[3] = { c.base[0] + ox * 0.9f + px, c.base[1] + oy * 0.9f + py, c.base[2] + h };
							clear = clear && ClearLine(from, to);
						}
					}
					if (!clear) {
						++why[1];
						continue;
					}
					// no other object in the mover's way (up to a metre and a half short of the prop)
					P::Mover    mv;
					const McVec s = GtaToMc(sx, sy, ground + a_lo);
					mv.bottom[0] = float(s.x), mv.bottom[1] = float(s.y), mv.bottom[2] = float(s.z);
					mv.vel[0] = -ox, mv.vel[1] = 0.0f, mv.vel[2] = oy;  // GTA (-ox, -oy) -> MC (x, -y)
					mv.halfWidth = a_halfWidth + 0.3f;
					mv.height = a_hi - a_lo;
					for (const auto& o : others) {
						if (o.key != c.key && P::FirstContact(mv, o.boxes.data(), o.boxes.size(), a_run - 1.5f, 0.1f) >= 0.0f) {
							clear = false;
							break;
						}
					}
					if (!clear) {
						++why[2];
						continue;
					}
					a_out[0] = sx, a_out[1] = sy, a_out[2] = ground;
					a_out[3] = std::atan2(ox, -oy) * kRadToDeg;  // facing the prop: along (-ox, -oy)
					char buf[220];
					std::snprintf(buf, sizeof buf, "%s %d (hash 0x%08X, %.0f kg, %.1f m tall, physics %d) at GTA %.1f %.1f %.1f",
						IsTrafficLight(c.hash) ? "traffic light" : "prop", pi.handle, c.hash, pi.mass, c.top, pi.physics ? 1 : 0, c.base[0], c.base[1], c.base[2]);
					a_what = buf;
					return true;
				}
			}
			LC_LOG("DebugPropSmash: no clear run up to any of %zu %s (directions: %d no level ground, %d a building in the way, %d an object in the way)", cands.size(),
				a_lights ? "traffic lights" : "tall props", why[0], why[1], why[2]);
			return false;
		}

		// A building's wall 18 to 45 m from a_at with a clear run up: where to stand (22 m from it).
		bool FindWall(const float a_at[3], float a_out[4], std::string& a_what)
		{
			int why[6]{};  // no hit, distance or facing, ground, low, not clear (log)
			for (int k = 0; k < 24; ++k) {
				const float a = static_cast<float>(k) * kPi / 12.0f, dx = std::cos(a), dy = std::sin(a);
				tLineOfSightResults res;
				const float         from[3] = { a_at[0], a_at[1], a_at[2] + 1.0f }, to[3] = { a_at[0] + dx * 45.0f, a_at[1] + dy * 45.0f, a_at[2] + 1.0f };
				--col::rayCounters.rays;
				if (!col::CastGta(from, to, res)) {
					++why[0];
					continue;
				}
				const float* p = &res.m_vEndPosition.x;
				const float* n = &res.m_vUnk.x;
				const float  dist = std::hypot(p[0] - a_at[0], p[1] - a_at[1]);
				if (!(dist > 12.0f && dist < 50.0f) || !(n[0] * dx + n[1] * dy < -0.6f)) {
					++why[1];
					continue;
				}
				const float sx = p[0] - dx * 22.0f, sy = p[1] - dy * 22.0f;
				float       ground = 0.0f;
				S::GET_GROUND_Z_FOR_3D_COORD(sx, sy, a_at[2] + 5.0f, &ground);
				if (!(ground > a_at[2] - 6.0f && ground < a_at[2] + 3.0f)) {
					++why[2];
					continue;
				}
				const float up[3] = { sx, sy, ground + 4.0f }, wall[3] = { p[0] - dx * 0.5f, p[1] - dy * 0.5f, ground + 4.0f };
				tLineOfSightResults high;
				--col::rayCounters.rays;
				if (!col::CastGta(up, wall, high)) {
					++why[3];
					continue;  // not a wall up there (a low one)
				}
				const float low[3] = { sx, sy, ground + 1.0f }, lowTo[3] = { p[0] - dx * 1.0f, p[1] - dy * 1.0f, ground + 1.0f };
				if (!ClearLine(low, lowTo)) {
					++why[4];
					continue;
				}
				a_out[0] = sx, a_out[1] = sy, a_out[2] = ground;
				a_out[3] = std::atan2(-dx, dy) * kRadToDeg;
				char buf[160];
				std::snprintf(buf, sizeof buf, "a wall at GTA %.1f %.1f %.1f (normal %.2f %.2f %.2f)", p[0], p[1], p[2], n[0], n[1], n[2]);
				a_what = buf;
				return true;
			}
			LC_LOG("DebugPropSmash: no wall: 24 directions, %d no hit within 45 m, %d too near/far or not facing back, %d no ground at the start, %d low, %d not clear",
				why[0], why[1], why[2], why[3], why[4]);
			return false;
		}

		// "break": every 3 s the nearest tall prop within 30 m not tried yet gets one way of knocking it over (log).
		void BreakTry(int a_ped, int a_way)
		{
			float p[3]{};
			S::GET_CHAR_COORDINATES(a_ped, &p[0], &p[1], &p[2]);
			const void*  best = nullptr;
			float        bestD2 = 50.0f * 50.0f, top = 0.0f;
			std::int32_t model = -1;
			std::uint32_t hash = 0;
			Collision::Get().ForEachSolidObject([&](const Collision::ObjectView& a_v) {
				const auto* b = static_cast<const P::Box*>(a_v.boxes);
				float lo = 1e9f, hi = -1e9f;
				for (std::size_t i = 0; i < a_v.count; ++i) {
					lo = std::min(lo, b[i].y0), hi = std::max(hi, b[i].y1);
				}
				const GtaVec g = McToGta(b[0].o[0], lo, b[0].o[1]);
				const float  d2 = (float(g.x) - p[0]) * (float(g.x) - p[0]) + (float(g.y) - p[1]) * (float(g.y) - p[1]);
				if (hi - lo >= 2.5f && !broken.count(a_v.key) && d2 < bestD2) {
					bestD2 = d2, best = a_v.key, top = hi - lo, model = a_v.model, hash = a_v.hash;
				}
			});
			if (!best) {
				LC_LOG("DebugPropSmash break %d: no tall prop within 50 m", a_way);
				return;
			}
			PropInfo& pi = Info(best, model);
			int       h = pi.handle;
			if (!h) {
				return;
			}
			broken[best] = ::GetTickCount64();
			int cls = -1, lvl = -1;
			PhysicsState(h, cls, lvl);
			const float v = 3.0f;
			const auto  flags0 = PhysicalFlags(h);
			const float a = static_cast<float>(a_way) * 1.3f;  // (a different way each time)
			StartTopple(h, std::cos(a), std::sin(a), std::max(top, 1.0f), 1.0f);
			int cls2 = -1, lvl2 = -1;
			PhysicsState(h, cls2, lvl2);
			char what[160];
			std::snprintf(what, sizeof what, "DebugPropSmash break %d: prop %d (hash 0x%08X, %.0f kg, %.1f m tall; class %d, level state %d -> %d, flags 0x%llX -> 0x%llX) "
				"pushed %.0f m/s %s", a_way, h, hash, pi.mass, top, cls, lvl, lvl2, static_cast<long long>(flags0), static_cast<long long>(PhysicalFlags(h)), v,
				"(tipped over)");
			LC_LOG("%s", what);
			Watch wt;
			wt.handle = h;
			wt.atMs = ::GetTickCount64();
			S::GET_OBJECT_COORDINATES(h, &wt.start[0], &wt.start[1], &wt.start[2]);
			wt.what = what;
			watching.push_back(wt);
		}

		void Key(int a_code, bool a_down) { Link::Get().PushInput(proto::kInKey, static_cast<std::uint16_t>(a_code), a_down ? 1 : 0); }
		void Mouse(int a_button, bool a_down) { Link::Get().PushInput(proto::kInMouseButton, static_cast<std::uint16_t>(a_button), a_down ? 1 : 0); }

		void TestTick(int a_ped, bool a_puppeting, float a_dt)
		{
			auto& t = test;
			const std::string& spec = Config::Get().debugPropSmash;
			if (spec.empty()) {
				return;
			}
			if (!t.parsed) {
				t.parsed = true;
				std::size_t at = 0;
				while (at < spec.size()) {
					const std::size_t end = std::min(spec.find('|', at), spec.size());
					const std::string item = spec.substr(at, end - at);
					const std::size_t colon = item.find(':');
					if (colon != std::string::npos) {
						t.steps.push_back({ static_cast<float>(std::atof(item.substr(0, colon).c_str())), item.substr(colon + 1) });
					}
					at = end + 1;
				}
			}
			if (t.t < 0.0f) {
				if (!a_puppeting) {
					return;
				}
				t.t = 0.0f;
			}
			t.t += a_dt;
			t.phaseT += a_dt;
			if (t.next >= t.steps.size() || !a_ped) {
				return;
			}
			const TestStep& step = t.steps[t.next];
			if (step.kind == "view") {
				// a camera of ours on the last prop knocked over, where it lies, for 4 s (puppet mode or not)
				static int   cam = 0;
				static float until = 0.0f;
				if (t.t < step.at) {
					return;
				}
				if (!cam) {
					const int h = lastTopple.handle;
					if (!h || !S::DOES_OBJECT_EXIST(h)) {
						LC_LOG("DebugPropSmash view: nothing knocked over to look at");
						++t.next;
						return;
					}
					float b[3]{};
					S::GET_OBJECT_COORDINATES(h, &b[0], &b[1], &b[2]);
					const float dx = lastTopple.dir[0], dy = lastTopple.dir[1], half = lastTopple.length * 0.5f;
					const float mid[3] = { b[0] + dx * half, b[1] + dy * half, b[2] + 0.5f };
					S::CREATE_CAM(14, &cam);
					if (cam) {
						S::SET_CAM_POS(cam, mid[0] - dy * 7.0f - dx * 3.0f, mid[1] + dx * 7.0f - dy * 3.0f, mid[2] + 4.0f);
						S::POINT_CAM_AT_COORD(cam, mid[0], mid[1], mid[2]);
						S::SET_CAM_ACTIVE(cam, true);
						S::SET_CAM_PROPAGATE(cam, true);
						S::ACTIVATE_SCRIPTED_CAMS(true, true);
					}
					until = t.t + 4.0f;
					LC_LOG("DebugPropSmash view: a camera on prop %d where it lies (its foot at GTA %.1f %.1f %.1f, tipped toward %.2f %.2f)", h, b[0], b[1], b[2], dx, dy);
				} else if (t.t >= until) {
					S::SET_CAM_ACTIVE(cam, false);
					S::ACTIVATE_SCRIPTED_CAMS(false, false);
					S::DESTROY_CAM(cam);
					cam = 0;
					++t.next;
					LC_LOG("DebugPropSmash view: done");
				}
				return;
			}
			switch (t.phase) {
			case 0: {
				if (t.t < step.at || !a_puppeting) {
					return;
				}
				if (step.kind == "break") {
					static int   way = 0;
					static float next = 0.0f;
					if (t.t >= next) {
						next = t.t + 3.0f;
						BreakTry(a_ped, way++);
					}
					if (way >= 2) {
						++t.next;
					}
					return;
				}
				float p[3]{};
				S::GET_CHAR_COORDINATES(a_ped, &p[0], &p[1], &p[2]);
				float       start[4]{};
				std::string what;
				bool found = false;
				if (step.kind == "fall") {
					// where he stands, 9 m up
					float ground = p[2] - 1.0f, heading = 0.0f;
					S::GET_GROUND_Z_FOR_3D_COORD(p[0], p[1], p[2] + 1.0f, &ground);
					S::GET_CHAR_HEADING(a_ped, &heading);
					start[0] = p[0], start[1] = p[1], start[2] = ground, start[3] = heading;
					what = "a plain fall";
					found = true;
				} else if (step.kind == "wall") {
					found = FindWall(p, start, what);
				} else if (t.t < t.retryAt) {
					return;  // (put next to a traffic light: GTA IV and Collision catch up)
				} else if (step.kind == "horse") {
					found = FindPole(p, 30.0f, false, 0.7f, 0.15f, 2.6f, start, what);  // (a horse and its rider)
				} else {
					// the elytra flight: from 5.5 m up it glides on about 4 m over the ground
					found = FindPole(p, 22.0f, step.kind == "light", 0.3f, 2.0f, 6.0f, start, what);
					float there[3]{};
					if (!found && step.kind == "light" && !t.relocated && NearestPoolLight(p, 20.0f, there)) {
						t.relocated = true;
						t.retryAt = t.t + 6.0f;
						S::SET_CHAR_COORDINATES(a_ped, there[0] + 2.0f, there[1] + 2.0f, there[2] + 1.0f);
						LC_LOG("DebugPropSmash light: none with a clear run up here; the player put next to the traffic light at GTA %.1f %.1f %.1f", there[0], there[1],
							there[2]);
						return;
					}
				}
				if (!found) {
					LC_LOG("DebugPropSmash %s: nothing suitable near GTA %.1f %.1f %.1f; skipped", step.kind.c_str(), p[0], p[1], p[2]);
					++t.next;
					return;
				}
				// (elytra runs start 5.5 m up, the plain fall 10 m: the player falls, opens the elytra, fires a rocket)
				S::SET_CHAR_COORDINATES(a_ped, start[0], start[1], start[2] + 1.0f + (step.kind == "horse" ? 0.0f : step.kind == "fall" ? 9.0f : 4.5f));
				S::SET_CHAR_HEADING(a_ped, start[3]);
				Link::Get().PushInput(proto::kInRestore, 0);  // (full health for the crash)
				LC_LOG("DebugPropSmash %s: target %s; the player put at GTA %.1f %.1f %.1f facing it (heading %.0f)", step.kind.c_str(), what.c_str(), start[0],
					start[1], start[2], start[3]);
				t.phase = 1;
				t.phaseT = 0.0f;
				t.launch = 0;
				return;
			}
			case 1:
				// once Minecraft has the player again (the teleport handshake), and a moment more; up in the air
				// Niko is held there meanwhile (GTA would let him fall and tumble)
				if (!a_puppeting && step.kind != "horse") {
					S::FREEZE_CHAR_POSITION(a_ped, true);
				}
				if (!a_puppeting) {
					t.phaseT = std::min(t.phaseT, 0.0f);
					return;
				}
				if (t.phaseT < (step.kind == "horse" ? 9.0f : 0.1f)) {
					return;
				}
				t.phase = 2;
				t.phaseT = 0.0f;
				return;
			default: {
				// the inputs, in order: (time from the phase start, what)
				if (step.kind == "horse") {
					if (t.launch == 0) {
						Key(26, true);  // W
						t.launch = 1;
						LC_LOG("DebugPropSmash horse: W held");
					} else if (t.launch == 1 && t.phaseT >= 4.0f) {
						Key(26, false);
						t.launch = 2;
					}
				} else if (step.kind != "fall") {
					// falling (Minecraft's mover events): space opens the elytra; flying: a rocket
					const bool now = mover.valid && ::GetTickCount64() - mover.atMs < 200;
					if (t.launch == 0 && now && mover.m.vel[1] < -4.0f && !(mover.flags & proto::kMoverFlying)) {
						Key(44, true);
						t.launch = 1;
						t.launchT = t.phaseT;
					} else if (t.launch == 1 && t.phaseT - t.launchT > 0.1f) {
						Key(44, false);
						t.launch = 2;
					} else if (t.launch == 2 && now && (mover.flags & proto::kMoverFlying)) {
						Mouse(3, true);
						t.launch = 3;
						t.launchT = t.phaseT;
					} else if (t.launch == 3 && t.phaseT - t.launchT > 0.1f) {
						Mouse(3, false);
						t.launch = 4;
						LC_LOG("DebugPropSmash %s: opened the elytra, fired a rocket", step.kind.c_str());
					} else if (t.launch == 2 && t.phaseT - t.launchT > 0.6f) {
						t.launch = 0;  // (the press didn't open it: again)
					}
				} else if (t.launch == 0) {
					t.launch = 1;
					LC_LOG("DebugPropSmash fall: falling (no elytra)");
				}
				if (t.phaseT >= 8.0f) {
					LC_LOG("DebugPropSmash %s: done", step.kind.c_str());
					++t.next;
					t.phase = 0;
				}
				return;
			}
			}
		}
	}

	void OnEvent(const proto::McEvent& a_ev)
	{
		if (a_ev.type == proto::kEvMover) {
			const float yaw = FloatBits(a_ev.flags) * kDegToRad, pitch = FloatBits(a_ev.weapon) * kDegToRad;
			const float s = a_ev.d;
			if (!std::isfinite(yaw + pitch + s + a_ev.a + a_ev.b + a_ev.c)) {
				return;
			}
			mover.valid = true;
			mover.atMs = ::GetTickCount64();
			mover.speed = s;
			mover.flags = a_ev.formId & proto::kMoverFlagsMask;
			mover.m.bottom[0] = a_ev.a, mover.m.bottom[1] = a_ev.b, mover.m.bottom[2] = a_ev.c;
			mover.m.vel[0] = -std::sin(yaw) * std::cos(pitch) * s;
			mover.m.vel[1] = -std::sin(pitch) * s;
			mover.m.vel[2] = std::cos(yaw) * std::cos(pitch) * s;
			mover.m.halfWidth = static_cast<float>((a_ev.formId >> proto::kMoverWidthShift) & 1023u) / 200.0f;
			mover.m.height = static_cast<float>((a_ev.formId >> proto::kMoverHeightShift) & 1023u) / 100.0f;
			++counters.movers;
			return;
		}
		if (a_ev.type == proto::kEvImpact) {
			const bool  wall = (a_ev.flags & proto::kImpactWall) != 0;
			const float dir[2] = { a_ev.b, a_ev.c };
			const auto  c = P::CrashOf(wall, a_ev.a, dir, a_ev.d);
			++counters.crashes;
			char what[128];
			std::snprintf(what, sizeof what, "elytra crash into %s at %.1f m/s (flying %.1f m/s)", wall ? "a wall" : "the ground", a_ev.a, a_ev.d);
			if (!c.knock) {
				LC_LOG("%s: not hard enough to knock the player over", what);
				return;
			}
			if (!Game::State().puppeting.load(std::memory_order_relaxed)) {
				LC_LOG("%s: not in Minecraft mode on foot (no knockdown)", what);
				return;
			}
			++counters.knocks;
			LC_LOG("%s: knocked over, thrown %s at %.1f m/s", what, wall ? "back off it" : "on along the flight", c.throwSpeed);
			HostDrive::KnockDown(c.dir[0], c.dir[1], drive::hit::ForceForThrow(c.throwSpeed), c.ms, what);
		}
	}

	void Tick(int a_ped, bool a_puppeting, bool a_paused, float a_dt)
	{
		Stats();
		if (a_paused) {
			return;
		}
		TestTick(a_ped, a_puppeting, a_dt);
		WatchTick();
		ToppleTick(a_dt);
		const auto now = ::GetTickCount64();
		for (auto it = broken.begin(); it != broken.end();) {
			it = now - it->second > 20000 ? broken.erase(it) : std::next(it);
		}
		const bool fresh = mover.valid && now - mover.atMs <= kMoverFreshMs && a_puppeting;
		if (!fresh || mover.speed < P::kLightSpeed) {
			DisarmAll(!fresh ? "the mover stopped" : "too slow now");
			return;
		}
		// Where the mover is now (the event is from Minecraft's last tick: half a tick old on average).
		const float age = std::min(static_cast<float>(now - mover.atMs) / 1000.0f + 0.025f, 0.2f);
		P::Mover    m = mover.m;
		for (int k = 0; k < 3; ++k) {
			m.bottom[k] += m.vel[k] * age;
		}
		const float reach = mover.speed * kHorizon + 12.0f;
		struct Hit
		{
			const void*        key;
			float              t;
			std::vector<P::Box> boxes;
			std::int32_t       model;
			std::uint32_t      hash;
		};
		std::vector<Hit> hits;
		static std::uint64_t debugLogAt = 0;
		const bool           debugLog = !Config::Get().debugPropSmash.empty() && now >= debugLogAt;
		float                nearestD2 = 1e18f;
		std::uint32_t        nearestHash = 0;
		std::size_t          seen = 0;
		Collision::Get().ForEachSolidObject([&](const Collision::ObjectView& a_v) {
			++seen;
			if (broken.count(a_v.key)) {
				return;
			}
			const auto* b = static_cast<const P::Box*>(a_v.boxes);
			const float dx = b[0].o[0] - m.bottom[0], dz = b[0].o[1] - m.bottom[2];
			if (dx * dx + dz * dz < nearestD2) {
				nearestD2 = dx * dx + dz * dz;
				nearestHash = a_v.hash;
			}
			if (dx * dx + dz * dz > reach * reach) {
				return;
			}
			float lo = 1e9f, hi = -1e9f;
			for (std::size_t i = 0; i < a_v.count; ++i) {
				lo = std::min(lo, b[i].y0), hi = std::max(hi, b[i].y1);
			}
			if (hi - lo < kMinPropHeight) {
				return;  // (debris, a kerb stone: stepped over anyway)
			}
			const float t = P::FirstContact(m, b, a_v.count, kHorizon, kMargin);
			if (t >= 0.0f) {
				hits.push_back({ a_v.key, t, std::vector<P::Box>(b, b + a_v.count), a_v.model, a_v.hash });
			}
		});
		if (debugLog) {
			debugLogAt = now + 250;
			LC_LOG("DebugPropSmash: mover%s%s%s at MC %.2f %.2f %.2f, %.2f x %.2f m, %.1f m/s along %.2f %.2f %.2f; %zu solid objects, the nearest (0x%08X) %.1f m; "
				   "%zu ahead (the first in %.2f s), %zu let through",
				(mover.flags & proto::kMoverFlying) ? " flying" : "", (mover.flags & proto::kMoverRiding) ? " riding" : "",
				(mover.flags & proto::kMoverSprinting) ? " sprinting" : "", m.bottom[0], m.bottom[1], m.bottom[2], m.halfWidth * 2.0f, m.height, mover.speed,
				m.vel[0] / mover.speed, m.vel[1] / mover.speed, m.vel[2] / mover.speed, seen, nearestHash, std::sqrt(nearestD2), hits.size(),
				hits.empty() ? -1.0f : std::min_element(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.t < b.t; })->t, armed.size());
		}
		for (const auto& h : hits) {
			PropInfo& pi = Info(h.key, h.model);
			if (!pi.handle) {
				continue;
			}
			const float need = pi.physics ? P::BreakSpeed(pi.mass) : 1e9f;
			if (mover.speed < need) {
				Disarm(h.key, "the mover is too slow for it");
				if (!pi.slowLogged) {
					pi.slowLogged = true;
					++counters.tooSlow;
					LC_LOG("prop %d (hash 0x%08X, %.0f kg, physics %d) ahead: %.1f m/s is too slow to knock it over (needs %.1f)", pi.handle, h.hash, pi.mass,
						pi.physics ? 1 : 0, mover.speed, need);
				}
				continue;
			}
			auto [it, fresh] = armed.try_emplace(h.key);
			if (fresh) {
				it->second.atMs = now;
				Collision::Get().SetObjectPassable(h.key, true);
				++counters.armed;
				float fx = 0.0f, fz = 0.0f, lo = 0.0f, hi = 0.0f;
				Foot(h.boxes.data(), h.boxes.size(), fx, fz, lo, hi);
				LC_LOG("prop %d (hash 0x%08X, %.0f kg) %.2f s ahead at %.1f m/s: out of Minecraft's collision (it stands at MC %.2f %.2f %.2f, %.1f m tall, "
					   "%zu boxes; the mover at MC %.2f %.2f %.2f, %.2f wide)",
					pi.handle, h.hash, pi.mass, h.t, mover.speed, fx, lo, fz, hi - lo, h.boxes.size(), m.bottom[0], m.bottom[1], m.bottom[2], m.halfWidth * 2.0f);
			}
			it->second.seenMs = now;
			it->second.t = h.t;
			if (h.t <= a_dt + kContactLead) {
				Break(h.key, pi, m, h.boxes.data(), h.boxes.size(), h.hash);
			}
		}
		for (auto it = armed.begin(); it != armed.end();) {
			if (now - it->second.seenMs > kArmedLostMs || now - it->second.atMs > kArmedMaxMs) {
				const void* key = it->first;
				++it;
				Disarm(key, "the mover went past it");
			} else {
				++it;
			}
		}
	}

	void OnIngameStartup()
	{
		mover = MoverState{};
		infos.clear();
		armed.clear();  // (Collision drops its objects with the old game)
		broken.clear();
		watching.clear();
		topples.clear();
	}
}
