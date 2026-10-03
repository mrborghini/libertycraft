// Unity-built into dllmain.cpp (needs IV-SDK). See Doors.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "doors"
#include "Doors.h"

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

		constexpr float kScanRadius = 6.0f;      // doors tracked within this of the feet (m)
		constexpr int   kScanEvery = 4;          // frames between object pool scans
		constexpr float kOpenReach = 1.2f;       // open a door when the player is this close to its leaf...
		constexpr float kOpenTowards = 0.3f;     // ...walking towards it at least this fast (m/s)
		constexpr float kOpenAlways = 0.45f;     // ...or this close, whatever they do (standing in the doorway)
		constexpr float kCloseDist = 2.5f;       // shut it once the player is this far from the leaf...
		constexpr float kCloseAfter = 1.0f;      // ...for this long (s)
		constexpr float kVerifyAfter = 0.7f;     // an opening that hasn't turned the door by then: try the next way
		constexpr float kOpenedDeg = 25.0f;      // turned this far from shut: it's open
		constexpr float kOpenDeg = 85.0f;        // how far we swing a door (heading method)
		constexpr float kUnlockAfter = 1.0f;     // after shutting a door we held, hand it back to GTA this much later (s)
		constexpr float kPushForce = 6.0f;       // impulse method, per frame while pushing
		constexpr float kPushFor = 0.5f;         // s
		constexpr float kRad = 3.14159265f / 180.0f;

		// How a door is opened. The first that turns a door is used for the next ones.
		enum Method : int
		{
			kByState = 0,    // SET_STATE_OF_CLOSEST_DOOR_OF_TYPE(locked at a fully open ratio)
			kByHeading = 1,  // SET_OBJECT_HEADING about the hinge (door models have their origin there)
			kByPush = 2,     // APPLY_FORCE_TO_OBJECT across the leaf
			kMethods = 3,
		};
		const char* kMethodNames[kMethods] = { "door state", "heading", "push" };

		enum class Phase
		{
			kShut,
			kOpening,  // asked to open (method `method`), waiting to see it turn
			kOpen,     // held open while the player is near
			kShutting, // shut by us, handed back to GTA after kUnlockAfter
			kStuck,    // no way of ours turns it: leave it
		};

		struct Door
		{
			CObject*    obj = nullptr;
			int         slot = -1;
			int         handle = 0;
			int         model = -1;
			unsigned    modelHash = 0;
			float       shut[12]{};     // GTA matrix rows right, up, at, pos as first seen
			float       lo[3]{}, hi[3]{};
			int         wide = 0;       // local axis along the leaf (0 x, 1 y); the other one is across it
			float       leafSign = 1.0f;  // the leaf runs from the hinge towards +wide (1) or -wide (-1)
			bool        gameDoor = false;  // GTA's door system knows it (GET_STATE_OF_CLOSEST_DOOR_OF_TYPE answered)
			int         gameState = 0;
			float       gameRatio = 0.0f;
			Phase       phase = Phase::kShut;
			int         method = kByState;
			float       want = 1.0f;   // rotation we asked for (+: counter-clockwise about local z)
			float       timer = 0.0f, away = 0.0f, reasserted = 0.0f;
			std::uint32_t seen = 0;
		};

		std::vector<Door> doors;
		std::uint32_t     frameNo = 0, scanNo = 0;
		float             lastFeet[3]{};
		bool              haveLast = false;
		float             vel[3]{};
		int               preferred = kByState;
		float             stateSign = 1.0f;  // which way a positive door-state ratio turns a door (learned)
		bool              stateSignKnown = false;
		int               logged = 0, traced = 0, opensTraced = 0;
		struct Counters
		{
			unsigned opened = 0, shut = 0, failed = 0, reasserted = 0, locked = 0;
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

		bool Alive(const Door& a_d)
		{
			auto* pool = CPools::ms_pObjectPool;
			return pool && a_d.slot >= 0 && a_d.slot < static_cast<int>(pool->m_nCount) && pool->Get(a_d.slot) == a_d.obj && a_d.obj->m_nModelIndex == a_d.model &&
			       a_d.obj->m_pMatrix;
		}

		// Signed turn (degrees, + counter-clockwise seen from above) of the door from its shut pose.
		float Turned(const Door& a_d)
		{
			float m[12];
			ReadMatrix(*a_d.obj->m_pMatrix, m);
			const float* r0 = a_d.shut;
			const float* z = a_d.shut + 6;
			const float  c[3] = { r0[1] * m[2] - r0[2] * m[1], r0[2] * m[0] - r0[0] * m[2], r0[0] * m[1] - r0[1] * m[0] };
			return std::atan2(Dot3(c, z), Dot3(r0, m)) / kRad;
		}

		float Heading(const float* a_m)  // GTA heading (degrees) of a matrix's up (forward) row
		{
			return std::atan2(-a_m[3], a_m[4]) / kRad;
		}

		// Where the player is relative to the shut leaf: distance to it, which side, speed towards it.
		struct Rel
		{
			float dist, side, towards, lz;
		};

		Rel Relate(const Door& a_d, const float* a_feet)
		{
			const float* right = a_d.shut;
			const float* up = a_d.shut + 3;
			const float* at = a_d.shut + 6;
			const float* pos = a_d.shut + 9;
			const float  d[3] = { a_feet[0] - pos[0], a_feet[1] - pos[1], a_feet[2] - pos[2] };
			const float  l[3] = { Dot3(d, right), Dot3(d, up), Dot3(d, at) };
			const int    w = a_d.wide, t = 1 - a_d.wide;
			const float  ow = std::max({ a_d.lo[w] - l[w], l[w] - a_d.hi[w], 0.0f });
			const float  mid = 0.5f * (a_d.lo[t] + a_d.hi[t]), half = 0.5f * (a_d.hi[t] - a_d.lo[t]);
			const float  ot = std::max(std::fabs(l[t] - mid) - half, 0.0f);
			const float* across = t == 0 ? right : up;
			Rel r{};
			r.dist = std::sqrt(ow * ow + ot * ot);
			r.side = l[t] >= mid ? 1.0f : -1.0f;
			r.towards = -r.side * Dot3(vel, across);
			r.lz = l[2];
			return r;
		}

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
				auto it = std::find_if(doors.begin(), doors.end(), [&](const Door& d) { return d.obj == obj && d.model == obj->m_nModelIndex; });
				if (it != doors.end()) {
					it->seen = scanNo;
					it->slot = slot;
					continue;
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
				Door d;
				d.obj = obj;
				d.slot = slot;
				d.model = model;
				d.seen = scanNo;
				std::memcpy(d.shut, m, sizeof(m));
				std::memcpy(d.lo, box.lo, sizeof(d.lo));
				std::memcpy(d.hi, box.hi, sizeof(d.hi));
				d.wide = (box.hi[0] - box.lo[0]) >= (box.hi[1] - box.lo[1]) ? 0 : 1;
				d.leafSign = std::fabs(box.lo[d.wide]) <= std::fabs(box.hi[d.wide]) ? 1.0f : -1.0f;
				d.handle = static_cast<int>(pool->GetIndex(obj));
				unsigned hash = 0;
				if (d.handle && S::DOES_OBJECT_EXIST(d.handle)) {
					S::GET_OBJECT_MODEL(d.handle, &hash);
				}
				d.modelHash = hash ? hash : mi->m_nHash;
				// Is it one of GTA's doors? The query only writes its answers if it found one (and the
				// state as a single byte: seen in game).
				int   state = 0;
				float ratio = -12345.0f;
				S::GET_STATE_OF_CLOSEST_DOOR_OF_TYPE(d.modelHash, m[9], m[10], m[11], &state, &ratio);
				d.gameDoor = ratio != -12345.0f;
				d.gameState = state & 0xFF;
				d.gameRatio = d.gameDoor ? ratio : 0.0f;
				d.method = preferred;
				if (logged < 30) {
					++logged;
					LC_LOG("door model %d hash 0x%08X (GET_OBJECT_MODEL 0x%08X, model info 0x%08X) handle %d at GTA (%.2f %.2f %.2f) heading %.1f: "
						   "%.2f wide (axis %d, leaf %+.0f), local (%.2f %.2f %.2f)..(%.2f %.2f %.2f); door state: %s %d ratio %.2f",
						model, d.modelHash, hash, mi->m_nHash, d.handle, m[9], m[10], m[11], Heading(m), box.hi[d.wide] - box.lo[d.wide], d.wide, d.leafSign, lo[0], lo[1],
						lo[2], hi[0], hi[1], hi[2], d.gameDoor ? "known" : "unknown", state, ratio);
				}
				doors.push_back(d);
			}
		}

		void SetHeading(Door& a_d, float a_turnDeg)
		{
			S::SET_OBJECT_HEADING(a_d.handle, Heading(a_d.shut) + a_turnDeg);
		}

		void Push(Door& a_d)
		{
			const float* across = a_d.wide == 0 ? a_d.shut + 3 : a_d.shut;
			// a turn by +want moves the leaf's tip along (z x leaf) * want
			const float* along = a_d.wide == 0 ? a_d.shut : a_d.shut + 3;
			const float  leaf[3] = { along[0] * a_d.leafSign, along[1] * a_d.leafSign, 0.0f };
			const float  dir[3] = { -leaf[1] * a_d.want, leaf[0] * a_d.want, 0.0f };
			const float  s = Dot3(dir, across) >= 0.0f ? 1.0f : -1.0f;
			S::APPLY_FORCE_TO_OBJECT(a_d.handle, 3, across[0] * s * kPushForce, across[1] * s * kPushForce, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1, 1, 1);
		}

		void Open(Door& a_d, float a_side)
		{
			// Swing the leaf to the side away from the player: a turn by +90 about local z takes the
			// leaf (along +-wide) to +-across.
			const float acrossOfTurn = a_d.wide == 0 ? a_d.leafSign : -a_d.leafSign;  // across-side the tip goes to for a + turn
			a_d.want = -a_side * acrossOfTurn;
			a_d.timer = 0.0f;
			a_d.away = 0.0f;
			a_d.phase = Phase::kOpening;
			switch (a_d.method) {
			case kByState:
				S::SET_STATE_OF_CLOSEST_DOOR_OF_TYPE(a_d.modelHash, a_d.shut[9], a_d.shut[10], a_d.shut[11], 1, a_d.want * stateSign);
				break;
			case kByHeading:
				SetHeading(a_d, a_d.want * kOpenDeg);
				break;
			default:
				Push(a_d);
				break;
			}
		}

		// Shut a door we opened (and, by state, hand it back to GTA a moment later).
		void Shut(Door& a_d)
		{
			switch (a_d.method) {
			case kByState:
				S::SET_STATE_OF_CLOSEST_DOOR_OF_TYPE(a_d.modelHash, a_d.shut[9], a_d.shut[10], a_d.shut[11], 1, 0.0f);
				break;
			case kByHeading:
				SetHeading(a_d, 0.0f);
				break;
			default:
				break;  // pushed: it swings back by itself (or stays; GTA's door)
			}
			a_d.phase = Phase::kShutting;
			a_d.timer = 0.0f;
			++counters.shut;
			if (traced < 12) {
				++traced;
				LC_LOG("door 0x%08X: the player is clear, shutting it (%s; turned %.0f deg)", a_d.modelHash, kMethodNames[a_d.method], Turned(a_d));
			}
		}

		void Release(Door& a_d)
		{
			if (traced < 12 && a_d.phase == Phase::kShutting) {
				++traced;
				LC_LOG("door 0x%08X handed back to GTA (turned %.0f deg from shut)", a_d.modelHash, Turned(a_d));
			}
			if (a_d.method == kByState) {
				// as GTA had it (unlocked, usually)
				S::SET_STATE_OF_CLOSEST_DOOR_OF_TYPE(a_d.modelHash, a_d.shut[9], a_d.shut[10], a_d.shut[11], a_d.gameDoor ? a_d.gameState : 0,
					a_d.gameDoor ? a_d.gameRatio : 0.0f);
			}
			a_d.phase = Phase::kShut;
		}

		void Step(Door& a_d, const Rel& a_r, bool a_puppeting, float a_dt)
		{
			const bool inReach = a_r.lz > a_d.lo[2] - 1.0f && a_r.lz < a_d.hi[2];
			const bool want = a_puppeting && inReach && (a_r.dist < kOpenAlways || (a_r.dist < kOpenReach && a_r.towards > kOpenTowards));
			switch (a_d.phase) {
			case Phase::kShut:
				if (want && a_d.gameDoor && a_d.gameState != 0) {
					// GTA keeps it locked (a mission door, a place not open yet): so do we
					a_d.phase = Phase::kStuck;
					++counters.locked;
					LC_LOG("door 0x%08X is locked by GTA (state %d, ratio %.2f): leaving it shut", a_d.modelHash, a_d.gameState, a_d.gameRatio);
				} else if (want) {
					Open(a_d, a_r.side);
				}
				break;
			case Phase::kOpening: {
				const float before = a_d.timer;
				a_d.timer += a_dt;
				const float turned = Turned(a_d);
				if (traced < 6 && before < 0.25f && a_d.timer >= 0.25f) {
					++traced;
					LC_LOG("door 0x%08X opening by %s (asked %+.0f): turned %.1f deg after %.2f s", a_d.modelHash, kMethodNames[a_d.method], a_d.want, turned, a_d.timer);
				}
				if (std::fabs(turned) >= kOpenedDeg) {
					a_d.phase = Phase::kOpen;
					a_d.timer = 0.0f;
					a_d.reasserted = 0.0f;
					++counters.opened;
					if (a_d.method == kByState && !stateSignKnown) {
						stateSignKnown = true;
						if (turned * a_d.want < 0.0f) {
							stateSign = -stateSign;  // it swung towards the player: the ratio's sign is the other way round
							S::SET_STATE_OF_CLOSEST_DOOR_OF_TYPE(a_d.modelHash, a_d.shut[9], a_d.shut[10], a_d.shut[11], 1, a_d.want * stateSign);
						}
						LC_LOG("doors: a positive door-state ratio turns a door %s", stateSign > 0 ? "counter-clockwise" : "clockwise");
					}
					if (preferred != a_d.method) {
						LC_LOG("doors: opening doors by %s from now on", kMethodNames[a_d.method]);
					}
					preferred = a_d.method;
					LC_LOG("door 0x%08X opened by %s: turned %.0f deg in %.2f s (asked %+.0f)", a_d.modelHash, kMethodNames[a_d.method], turned, a_d.timer, a_d.want);
				} else if (a_d.method == kByPush && a_d.timer < kPushFor) {
					Push(a_d);
				} else if (a_d.timer > kVerifyAfter) {
					LC_LOG("door 0x%08X (%s door) didn't open by %s (turned %.1f deg in %.2f s)", a_d.modelHash, a_d.gameDoor ? "GTA" : "not a GTA", kMethodNames[a_d.method],
						turned, a_d.timer);
					if (a_d.method == kByState) {
						Release(a_d);
					}
					if (++a_d.method >= kMethods) {
						a_d.method = preferred;
						a_d.phase = Phase::kStuck;
						++counters.failed;
					} else {
						a_d.phase = Phase::kShut;  // the next frame that wants it tries the next way
					}
				}
				break;
			}
			case Phase::kOpen: {
				const float before = a_d.timer;
				a_d.timer += a_dt;
				const float turned = Turned(a_d);
				if (opensTraced < 3 && ((before < 0.5f && a_d.timer >= 0.5f) || (before < 2.0f && a_d.timer >= 2.0f) || (before < 4.0f && a_d.timer >= 4.0f))) {
					LC_LOG("door 0x%08X open %.1f s: turned %.0f deg, player %.1f m from the leaf", a_d.modelHash, a_d.timer, turned, a_r.dist);
					opensTraced += a_d.timer >= 4.0f ? 1 : 0;
				}
				a_d.away = (!a_puppeting || a_r.dist > kCloseDist) ? a_d.away + a_dt : 0.0f;
				if (a_d.away >= kCloseAfter || (!a_puppeting && a_d.away > 0.0f)) {
					Shut(a_d);
				} else if (std::fabs(turned) < kOpenedDeg && a_d.timer - a_d.reasserted > 0.5f) {
					// GTA swung it back while we hold it open: again
					a_d.reasserted = a_d.timer;
					++counters.reasserted;
					if (a_d.method == kByState) {
						S::SET_STATE_OF_CLOSEST_DOOR_OF_TYPE(a_d.modelHash, a_d.shut[9], a_d.shut[10], a_d.shut[11], 1, a_d.want * stateSign);
					} else if (a_d.method == kByHeading) {
						SetHeading(a_d, a_d.want * kOpenDeg);
					}
				}
				break;
			}
			case Phase::kShutting:
				a_d.timer += a_dt;
				if (a_d.timer >= kUnlockAfter) {
					Release(a_d);
				}
				break;
			case Phase::kStuck:
				break;
			}
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
		if (haveLast) {
			const float k = std::min(1.0f, dt / 0.1f);
			for (int i = 0; i < 3; ++i) {
				const float v = std::clamp((a_frame.feet[i] - lastFeet[i]) / dt, -20.0f, 20.0f);
				vel[i] += (v - vel[i]) * k;
			}
		}
		std::memcpy(lastFeet, a_frame.feet, sizeof(lastFeet));
		haveLast = true;
		const bool anyBusy = std::any_of(doors.begin(), doors.end(), [](const Door& d) { return d.phase != Phase::kShut && d.phase != Phase::kStuck; });
		if (!a_frame.puppeting && !anyBusy) {
			doors.clear();  // GTA drives: its doors work for Niko by themselves
			return;
		}
		if (a_frame.puppeting && frameNo % kScanEvery == 0) {
			Scan(a_frame.feet);
		}
		for (auto it = doors.begin(); it != doors.end();) {
			if (!Alive(*it)) {
				it = doors.erase(it);  // gone (streamed out, deleted): nothing to hand back
				continue;
			}
			const Rel r = Relate(*it, a_frame.feet);
			Step(*it, r, a_frame.puppeting, dt);
			const bool idle = it->phase == Phase::kShut || it->phase == Phase::kStuck;
			if (idle && it->seen + 4 < scanNo) {
				it = doors.erase(it);  // out of range
			} else {
				++it;
			}
		}
		LC_LOG_EVERY(60000, "doors: %zu near, %u opened, %u shut, %u would not open, %u locked by GTA, %u held open again", doors.size(), counters.opened,
			counters.shut, counters.failed, counters.locked, counters.reasserted);
	}
}
