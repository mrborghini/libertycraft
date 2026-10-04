// Unity-built into dllmain.cpp (needs IV-SDK). See Doors.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "doors"
#include "Doors.h"

#include "Config.h"
#include "Log.h"
#include "collision/Objects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace lc::Doors
{
	namespace
	{
		namespace S = ::Scripting;

		constexpr float kScanRadius = 6.0f;     // doors tracked within this of the feet (m)
		constexpr int   kScanEvery = 4;         // frames between object pool scans
		constexpr float kPlayerRadius = 0.35f;  // the player seen from above (Minecraft: 0.3), plus a little
		constexpr float kPlayerHeight = 1.8f;
		constexpr float kMinPush = 0.5f;        // degrees: a smaller cut into the player is left alone
		constexpr float kMaxSwing = 100.0f;     // degrees: never push a door further than this from shut
		constexpr float kEndAfter = 0.25f;      // s without a push: the push is over (the door is GTA's again)
		constexpr float kTrialTime = 0.3f;      // s of pushing that must turn the door (by kTrialDeg) for a way to count as working
		constexpr float kTrialDeg = 4.0f;
		constexpr float kGain = 30.0f;          // angular velocity = gain x the turn still needed (1/s)
		constexpr float kMaxOmega = 15.0f;      // rad/s
		constexpr float kForceGain = 60.0f;     // force way: impulse per radian still needed
		constexpr float kMaxForce = 40.0f;
		constexpr float kRatioDeg = 81.0f;      // door state: ratio 1 turns a door this far (measured: 81 deg counter-clockwise)
		constexpr float kJump = 5.0f;           // the feet moved this far in a frame: a teleport
		constexpr float kRad = 3.14159265f / 180.0f;

		// How a push turns the door; the first that moves one is used from then on (Doors.h).
		enum Way : int
		{
			kSpin = 0,   // SET_OBJECT_INITIAL_ROTATION_VELOCITY: angular velocity about z
			kForce = 1,  // APPLY_FORCE_TO_OBJECT across the leaf at the contact point
			kState = 2,  // SET_STATE_OF_CLOSEST_DOOR_OF_TYPE locked at the open ratio, every frame
			kWays = 3,
		};
		const char* kWayNames[kWays] = { "angular velocity", "force", "door state" };

		// A door is remembered by its pool handle (slot << 8 | the slot's generation byte, what
		// GetIndex returns and object natives take) and found again every frame (Resolve): never by a
		// pointer kept from an earlier frame, which may be a streamed-out or recreated object.
		struct Door
		{
			int           handle = 0;
			int           model = -1;
			unsigned      modelHash = 0;
			float         shut[12]{};  // GTA matrix rows right, up, at, pos as first seen
			float         lo[3]{}, hi[3]{};
			int           wide = 0;         // local axis along the leaf (0 x, 1 y); the other one is across it
			float         leafSign = 1.0f;  // the leaf runs from the hinge towards +wide (1) or -wide (-1)
			bool          gameChecked = false;  // asked GTA's door state (the first time the player touched it)
			bool          gameDoor = false;     // GTA's door system knows it (GET_STATE_OF_CLOSEST_DOOR_OF_TYPE answered)
			int           gameState = 0;
			float         gameRatio = 0.0f;
			bool          gameLocked = false;   // GTA keeps it locked: never pushed
			float         side = 0.0f;          // which side of the player the leaf is on (0: not near)
			bool          pushing = false;      // a push in progress
			float         sincePush = 0.0f;     // s since the leaf last cut into the player
			float         pushTime = 0.0f;      // s of this push with a turn still needed
			float         trialFrom = 0.0f;     // turned (deg) when this way's trial began
			int           way = kSpin;
			bool          stateLocked = false;  // we hold GTA's door state (way kState)
			float         lastRatio = 0.0f;
			float         maxLag = 0.0f;        // deg: the most the door lagged behind the player this push
			float         traceT = -1.0f;       // >= 0: logging its angle over time (s since the push began)
			float         traceNext = 0.0f;
			float         traceEnd = 0.0f;      // stop tracing at this traceT (3 s after the push)
			float         lastDist = 0.0f;      // m from the hinge, last frame
			float         diagT = 0.0f;         // Diagnostics=1: s until the next "near the door" line
			bool          proven = false;       // this door has turned by its way
			std::uint32_t seen = 0;
		};

		std::vector<Door> doors;
		std::uint32_t     frameNo = 0, scanNo = 0;
		float             lastFeet[3]{};
		bool              haveLast = false;
		int               preferred = kSpin;
		bool              settled = false;  // a way has moved a door
		float             stateSign = 1.0f;  // which way a positive door-state ratio turns a door (learned)
		int               logged = 0, traces = 0;
		bool              handleChecked = false;
		struct Counters
		{
			unsigned pushes = 0, frames = 0, locked = 0, gone = 0, jumps = 0, failedWays = 0;
		} counters;

		float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		void ReadMatrix(const CMatrix& a_m, float* a_out)
		{
			const CVector* rows[4] = { &a_m.right, &a_m.up, &a_m.at, &a_m.pos };
			for (int r = 0; r < 4; ++r) {
				a_out[r * 3] = rows[r]->x;
				a_out[r * 3 + 1] = rows[r]->y;
				a_out[r * 3 + 2] = rows[r]->z;
			}
		}

		int HandleOf(const CPool<CObject>& a_pool, int a_slot) { return (a_slot << 8) | a_pool.m_pFlags[a_slot]; }

		// The door's object this frame, or null if it's gone: its slot is free or holds another
		// generation (streamed out, deleted, recreated), another model, or it isn't where the door
		// was (a door turns about its origin, the hinge: that never moves). Only valid until the
		// game runs again: never keep it.
		CObject* Resolve(const Door& a_d)
		{
			auto* pool = CPools::ms_pObjectPool;
			if (!pool || !pool->m_pFlags || a_d.handle <= 0) {
				return nullptr;
			}
			const int slot = a_d.handle >> 8;
			if (slot < 0 || slot >= static_cast<int>(pool->m_nCount) || (pool->m_pFlags[slot] & 0x80) || HandleOf(*pool, slot) != a_d.handle) {
				return nullptr;
			}
			CObject* obj = pool->Get(slot);
			if (!obj || obj->m_nModelIndex != a_d.model || !obj->m_pMatrix) {
				return nullptr;
			}
			const auto& p = obj->m_pMatrix->pos;
			const float dx = p.x - a_d.shut[9], dy = p.y - a_d.shut[10], dz = p.z - a_d.shut[11];
			if (!(dx * dx + dy * dy + dz * dz < 0.5f * 0.5f)) {
				return nullptr;  // (NaN fails too)
			}
			return obj;
		}

		// Signed turn (degrees, + counter-clockwise seen from above) of the door from its shut pose.
		float Turned(const Door& a_d, CObject* a_obj)
		{
			float m[12];
			ReadMatrix(*a_obj->m_pMatrix, m);
			const float* r0 = a_d.shut;
			const float* z = a_d.shut + 6;
			const float  c[3] = { r0[1] * m[2] - r0[2] * m[1], r0[2] * m[0] - r0[0] * m[2], r0[0] * m[1] - r0[1] * m[0] };
			return std::atan2(Dot3(c, z), Dot3(r0, m)) / kRad;
		}

		float Heading(const float* a_m)  // GTA heading (degrees) of a matrix's up (forward) row
		{
			return std::atan2(-a_m[3], a_m[4]) / kRad;
		}

		// The shut leaf's direction from the hinge (plan view, radians) and its length.
		float LeafAngle0(const Door& a_d)
		{
			const float* axis = a_d.wide == 0 ? a_d.shut : a_d.shut + 3;
			return std::atan2(axis[1] * a_d.leafSign, axis[0] * a_d.leafSign);
		}

		float LeafLength(const Door& a_d) { return a_d.leafSign > 0.0f ? a_d.hi[a_d.wide] : -a_d.lo[a_d.wide]; }

		float HalfThickness(const Door& a_d) { return 0.5f * (a_d.hi[1 - a_d.wide] - a_d.lo[1 - a_d.wide]); }

		void Scan(const float* a_feet)
		{
			auto* pool = CPools::ms_pObjectPool;
			if (!pool) {
				return;
			}
			++scanNo;
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CObject* obj = pool->Get(slot);
				if (!obj || !obj->m_pMatrix) {
					continue;
				}
				const auto& p = obj->m_pMatrix->pos;
				const float dx = p.x - a_feet[0], dy = p.y - a_feet[1], dz = p.z - a_feet[2];
				if (dx * dx + dy * dy > kScanRadius * kScanRadius || std::fabs(dz) > 4.0f) {
					continue;
				}
				const int handle = HandleOf(*pool, slot);
				auto      it = std::find_if(doors.begin(), doors.end(), [&](const Door& d) { return d.handle == handle; });
				if (it != doors.end() && it->model == obj->m_nModelIndex) {
					it->seen = scanNo;
					continue;
				}
				if (it != doors.end()) {
					doors.erase(it);  // that slot holds something else now
				}
				const int       model = obj->m_nModelIndex;
				CBaseModelInfo* mi = model >= 0 && model < 31000 ? CModelInfo::ms_modelInfoPtrs[model] : nullptr;
				if (!mi || obj->m_pAttachedToEntity) {
					continue;
				}
				float m[12];
				ReadMatrix(*obj->m_pMatrix, m);
				const float lo[3] = { mi->m_vMinBounds.x, mi->m_vMinBounds.y, mi->m_vMinBounds.z };
				const float hi[3] = { mi->m_vMaxBounds.x, mi->m_vMaxBounds.y, mi->m_vMaxBounds.z };
				const col::ObjectBox box = col::MakeObjectBox(m, m + 3, m + 6, m + 9, lo, hi);
				if (col::Classify(box) != col::ObjClass::kDoor) {
					continue;
				}
				if (!handleChecked) {
					handleChecked = true;
					const int gameHandle = static_cast<int>(pool->GetIndex(obj));
					LC_LOG("doors: object handle of slot %d: ours 0x%X, the game's 0x%X (%s)", slot, handle, gameHandle, gameHandle == handle ? "same" : "DIFFERENT");
				}
				Door d;
				d.handle = handle;
				d.model = model;
				d.seen = scanNo;
				std::memcpy(d.shut, m, sizeof(m));
				std::memcpy(d.lo, box.lo, sizeof(d.lo));
				std::memcpy(d.hi, box.hi, sizeof(d.hi));
				d.wide = (box.hi[0] - box.lo[0]) >= (box.hi[1] - box.lo[1]) ? 0 : 1;
				d.leafSign = std::fabs(box.lo[d.wide]) <= std::fabs(box.hi[d.wide]) ? 1.0f : -1.0f;
				d.modelHash = mi->m_nHash;  // (= GET_OBJECT_MODEL: checked in game)
				if (logged < 30) {
					++logged;
					LC_LOG("door model %d hash 0x%08X handle 0x%X at GTA (%.2f %.2f %.2f) heading %.1f: %.2f wide (axis %d, leaf %+.0f), local (%.2f %.2f %.2f)..(%.2f %.2f %.2f)",
						model, d.modelHash, handle, m[9], m[10], m[11], Heading(m), box.hi[d.wide] - box.lo[d.wide], d.wide, d.leafSign, lo[0], lo[1], lo[2], hi[0],
						hi[1], hi[2]);
				}
				doors.push_back(d);
			}
		}

		void SetState(const Door& a_d, int a_state, float a_ratio)
		{
			S::SET_STATE_OF_CLOSEST_DOOR_OF_TYPE(a_d.modelHash, a_d.shut[9], a_d.shut[10], a_d.shut[11], a_state, a_ratio);
		}

		// Is it one of GTA's doors, and does GTA keep it locked? Asked once, as the player first
		// touches it (the query only writes its answers if it found the door, the state as a byte).
		void CheckGameState(Door& a_d)
		{
			a_d.gameChecked = true;
			int   state = 0;
			float ratio = -12345.0f;
			S::GET_STATE_OF_CLOSEST_DOOR_OF_TYPE(a_d.modelHash, a_d.shut[9], a_d.shut[10], a_d.shut[11], &state, &ratio);
			a_d.gameDoor = ratio != -12345.0f;
			a_d.gameState = state & 0xFF;
			a_d.gameRatio = a_d.gameDoor ? ratio : 0.0f;
			a_d.gameLocked = a_d.gameDoor && a_d.gameState != 0;
			if (a_d.gameLocked) {
				++counters.locked;
				LC_LOG("door 0x%08X is locked by GTA (state %d, ratio %.2f): not pushing it", a_d.modelHash, a_d.gameState, a_d.gameRatio);
			}
		}

		// Turns the door towards clearing the player: a_need (radians, + counter-clockwise) is the
		// turn still needed, a_alpha the leaf's direction now, a_reach how far from the hinge the
		// player touches it.
		void Drive(Door& a_d, float a_turned, float a_need, float a_alpha, float a_reach)
		{
			switch (a_d.way) {
			case kSpin: {
				const float w = std::clamp(kGain * a_need, -kMaxOmega, kMaxOmega);
				S::SET_OBJECT_INITIAL_ROTATION_VELOCITY(a_d.handle, 0.0f, 0.0f, w);
				break;
			}
			case kForce: {
				// across the leaf, the way it has to turn, at the contact point (along the leaf in the
				// door's own frame: its origin is the hinge)
				const float dir = a_need > 0.0f ? 1.0f : -1.0f;
				const float f = std::min(kMaxForce, kForceGain * std::fabs(a_need));
				const float fx = -std::sin(a_alpha) * dir * f, fy = std::cos(a_alpha) * dir * f;
				float       off[3] = { 0.0f, 0.0f, 0.0f };
				off[a_d.wide] = a_d.leafSign * std::min(a_reach, LeafLength(a_d));
				S::APPLY_FORCE_TO_OBJECT(a_d.handle, 3, fx, fy, 0.0f, off[0], off[1], off[2], 0, 0, 1, 1);
				break;
			}
			default: {
				const float target = std::clamp(a_turned + a_need / kRad, -kMaxSwing, kMaxSwing);
				const float ratio = std::clamp(target / kRatioDeg * stateSign, -1.0f, 1.0f);
				if (!a_d.stateLocked || std::fabs(ratio - a_d.lastRatio) > 0.005f) {
					SetState(a_d, 1, ratio);
					a_d.stateLocked = true;
					a_d.lastRatio = ratio;
				}
				break;
			}
			}
		}

		// The push is over: the door is GTA's again (the state way unlocks it: its spring swings it shut).
		void EndPush(Door& a_d, float a_turned, const char* a_why)
		{
			if (a_d.stateLocked) {
				SetState(a_d, a_d.gameDoor ? a_d.gameState : 0, 0.0f);
				a_d.stateLocked = false;
			}
			if (a_d.traceT >= 0.0f) {
				LC_LOG("door 0x%08X push over after %.2f s (%s): turned %.0f deg, lagged behind the player by %.0f deg at most (%s)", a_d.modelHash, a_d.traceT, a_why,
					a_turned, a_d.maxLag, kWayNames[a_d.way]);
				a_d.traceEnd = a_d.traceT + 3.0f;  // and watch it swing back
			}
			a_d.pushing = false;
		}

		void Trace(Door& a_d, float a_turned, float a_need, float a_dist, float a_dt)
		{
			if (a_d.traceT < 0.0f) {
				return;
			}
			a_d.traceT += a_dt;
			if (a_d.traceT >= a_d.traceNext) {
				LC_LOG("door 0x%08X %5.2f s: turned %6.1f deg, %s %+5.1f deg, player %.2f m from the hinge", a_d.modelHash, a_d.traceT, a_turned,
					a_d.pushing ? "pushed, needs" : "free,  needs", a_need / kRad, a_dist);
				a_d.traceNext += a_d.pushing ? 0.1f : 0.25f;
			}
			if (!a_d.pushing && a_d.traceEnd > 0.0f && a_d.traceT >= a_d.traceEnd) {
				a_d.traceT = -1.0f;
			}
		}

		void Step(Door& a_d, CObject* a_obj, const float* a_feet, bool a_puppeting, float a_dt)
		{
			const float turned = Turned(a_d, a_obj);
			const float alpha = LeafAngle0(a_d) + turned * kRad;
			const float px = a_feet[0] - a_d.shut[9], py = a_feet[1] - a_d.shut[10];
			const float dist = std::sqrt(px * px + py * py);
			const float bottom = a_d.shut[11] + a_d.lo[2], top = a_d.shut[11] + a_d.hi[2];
			const bool  level = a_feet[2] < top && a_feet[2] + kPlayerHeight > bottom;
			float       need = 0.0f;
			if (a_puppeting && level && !a_d.gameLocked) {
				const float r = kPlayerRadius + HalfThickness(a_d);
				if (a_d.side == 0.0f) {
					a_d.side = LeafSide(alpha, px, py);
				}
				need = PushRotation(alpha, px, py, LeafLength(a_d), r, a_d.side);
				if (need == 0.0f) {
					a_d.side = LeafSide(alpha, px, py);  // not touching: whichever side it is on now
				} else {
					// never further than kMaxSwing from shut
					const float target = std::clamp(turned + need / kRad, -kMaxSwing, kMaxSwing);
					need = (target - turned) * kRad;
				}
			}
			if (Config::Get().diagnostics && dist < LeafLength(a_d) + 1.0f && (a_d.diagT -= a_dt) <= 0.0f) {
				a_d.diagT = 0.25f;
				const float sep = WrapPi(alpha - std::atan2(py, px)) / kRad;
				LC_LOG("door 0x%08X near: player %.2f m from the hinge, %.1f deg from the leaf (clear at %.1f), feet z %.2f (door %.2f..%.2f), side %+.0f, "
					   "turned %.1f, needs %+.1f, puppeting %d",
					a_d.modelHash, dist, sep, ClearAngle(dist, LeafLength(a_d), kPlayerRadius + HalfThickness(a_d)) / kRad, a_feet[2], bottom, top, a_d.side, turned,
					need / kRad, a_puppeting);
			}
			if (need != 0.0f && !a_d.gameChecked) {
				CheckGameState(a_d);
				if (a_d.gameLocked) {
					need = 0.0f;
				}
			}
			if (need != 0.0f) {
				if (!a_d.pushing) {
					a_d.pushing = true;
					a_d.pushTime = 0.0f;
					a_d.trialFrom = turned;
					a_d.maxLag = 0.0f;
					++counters.pushes;
					if (!a_d.proven) {
						a_d.way = preferred;
					}
					if (traces < 8) {
						++traces;
						a_d.traceT = 0.0f;
						a_d.traceNext = 0.0f;
						a_d.traceEnd = 0.0f;
						LC_LOG("door 0x%08X: the player walks into it (%s side, %.2f m from the hinge, door at %.0f deg); pushing by %s", a_d.modelHash,
							a_d.side > 0.0f ? "counter-clockwise" : "clockwise", dist, turned, kWayNames[a_d.way]);
					}
				}
				a_d.sincePush = 0.0f;
				if (std::fabs(need) > kMinPush * kRad) {
					Drive(a_d, turned, need, alpha, dist);
					++counters.frames;
					a_d.pushTime += a_dt;
					a_d.maxLag = std::max(a_d.maxLag, std::fabs(need) / kRad);
				}
				// Does this way turn doors? Judged on the first pushes of each door.
				if (!a_d.proven && a_d.pushTime >= kTrialTime) {
					if (std::fabs(turned - a_d.trialFrom) >= kTrialDeg) {
						a_d.proven = true;
						if (!settled) {
							settled = true;
							preferred = a_d.way;
							LC_LOG("doors: pushing doors by %s (turned one %.0f deg in %.2f s of pushing)", kWayNames[a_d.way], turned - a_d.trialFrom, a_d.pushTime);
						}
						if (a_d.way == kState && turned * a_d.lastRatio < 0.0f) {
							stateSign = -stateSign;  // the ratio turned it the other way
							LC_LOG("doors: a positive door-state ratio turns a door clockwise here: flipped");
						}
					} else if (a_d.way + 1 < kWays) {
						LC_LOG("doors: %s didn't turn door 0x%08X (%.1f deg in %.2f s of pushing); trying %s", kWayNames[a_d.way], a_d.modelHash, turned - a_d.trialFrom,
							a_d.pushTime, kWayNames[a_d.way + 1]);
						++counters.failedWays;
						++a_d.way;
						if (!settled) {
							preferred = a_d.way;
						}
						a_d.pushTime = 0.0f;
						a_d.trialFrom = turned;
					}
				}
			} else if (a_d.pushing) {
				a_d.sincePush += a_dt;
				if (!a_puppeting) {
					EndPush(a_d, turned, "Minecraft let go of the player");
				} else if (a_d.sincePush >= kEndAfter) {
					EndPush(a_d, turned, "nothing pushes it");
				}
			}
			a_d.lastDist = dist;
			Trace(a_d, turned, need, dist, a_dt);
		}
	}

	void Tick(const Frame& a_frame)
	{
		if (!a_frame.ped || a_frame.loading) {
			doors.clear();  // the game is (re)loading: its doors are going away
			haveLast = false;
			return;
		}
		++frameNo;
		const float dt = std::max(a_frame.dt, 1e-3f);
		// A jump (teleport, world change, warp): pushes end now, while their doors are certainly still there.
		const float jx = a_frame.feet[0] - lastFeet[0], jy = a_frame.feet[1] - lastFeet[1], jz = a_frame.feet[2] - lastFeet[2];
		const bool  jumped = haveLast && jx * jx + jy * jy + jz * jz > kJump * kJump;
		if (jumped) {
			++counters.jumps;
		}
		std::memcpy(lastFeet, a_frame.feet, sizeof(lastFeet));
		haveLast = true;
		const bool busy = std::any_of(doors.begin(), doors.end(), [](const Door& d) { return d.pushing || d.traceT >= 0.0f; });
		if (!a_frame.puppeting && !busy) {
			doors.clear();  // GTA drives: its doors work for Niko by themselves
			return;
		}
		if (a_frame.puppeting && frameNo % kScanEvery == 0) {
			Scan(a_frame.feet);
		}
		for (auto it = doors.begin(); it != doors.end();) {
			CObject* obj = Resolve(*it);
			if (!obj) {
				if (it->pushing) {
					++counters.gone;
					LC_LOG_EVERY(5000, "door 0x%08X went away while pushed (streamed out or deleted): forgetting it", it->modelHash);
				}
				it = doors.erase(it);  // nothing left to call a native on
				continue;
			}
			Step(*it, obj, a_frame.feet, a_frame.puppeting && !jumped, dt);
			const bool idle = !it->pushing && it->traceT < 0.0f;
			if (idle && (it->seen + 4 < scanNo || it->lastDist > kScanRadius + 2.0f)) {
				it = doors.erase(it);  // out of range
			} else {
				++it;
			}
		}
		LC_LOG_EVERY(60000, "doors: %zu near, %u pushes (%u frames), %u ways that didn't move a door, %u locked by GTA, %u gone while pushed, %u jumps; pushing by %s%s",
			doors.size(), counters.pushes, counters.frames, counters.failedWays, counters.locked, counters.gone, counters.jumps, kWayNames[preferred],
			settled ? "" : " (not tried yet)");
	}
}
