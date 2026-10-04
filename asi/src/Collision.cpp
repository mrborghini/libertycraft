// Unity-built into dllmain.cpp (needs IV-SDK). See Collision.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "collision"
#include "Collision.h"

#include "Link.h"
#include "Log.h"
#include "Perf.h"
#include "collision/Geometry.h"
#include "collision/Objects.h"
#include "collision/Rays.h"
#include "collision/Water.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lc
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;

		constexpr int  kRegionSize = col::kRegion;  // (= Collision::kRegionSize, for the helpers below)
		constexpr int  kRadius = 5;                // regions around the player horizontally
		constexpr int  kBelow = 3;                 // regions below the player
		constexpr int  kAbove = 2;                 // regions above the player
		constexpr int  kSpanExtra = 2;             // probe this many regions more above and below (jumps, stairs: no re-probe)
		constexpr auto kFrameBudget = 2500us;      // probing (and water) time per frame
		constexpr int  kMaxRegionsPerFrame = 48;   // regions handed to the worker per frame
		constexpr auto kRequestLead = 300ms;       // REQUEST_COLLISION_AT_POSN this long before probing a far column
		constexpr auto kRetryEmpty = 500ms;        // a column with no collision at all: try again after this
		constexpr int  kMaxEmptyRetries = 8;       // ...this often, then take it as empty (open air, deep water)
		constexpr auto kCheckNear = 1000ms;        // re-check the 3x3 columns around the player this often
		constexpr int  kWaterEvery = 3;            // water grid every n-th harvest frame
		// objects (collision/Objects.h)
		constexpr int   kObjRadius = 2;                // street furniture in the 5x5 columns around the player...
		constexpr int   kObjKeep = kObjRadius + 1;     // ...tracked (shapes kept) this far
		constexpr float kObjReach = 4.0f;              // an object this far outside a column can still reach into it
		constexpr int   kObjScanEvery = 8;             // frames between object pool scans
		constexpr int   kObjStillScans = 2;            // scans an object must keep still before it's probed
		constexpr auto  kObjReserve = 800us;           // of the frame budget, kept for object probes while columns are busy
		constexpr auto  kObjHoldMax = 2000ms;          // a newly seen object holds back its columns' update at most this long
		constexpr float kObjInside = 0.2f;             // the player's centre this close to a box: not solid (it would trap them)
		constexpr int   kObjLogFirst = 25;             // log the first n probed objects / door models in full

		struct Job
		{
			int                                  rx = 0, rz = 0;
			std::uint32_t                        epoch = 0;
			bool                                 clear = false;
			std::shared_ptr<const col::Column>   data;
			std::shared_ptr<const std::vector<col::Tri>> objTris;  // street furniture merged into the regions
			std::vector<int>                     rys;
		};

		struct ColumnState
		{
			std::shared_ptr<const col::Column> data;
			std::uint64_t                      hash = 0;
			std::vector<int>                   sent;  // regions (ry) sent from data
			Clock::time_point                  requested{}, retryAt{}, checked{};
			bool                               requestedOnce = false;
			bool                               stale = false;  // Refresh: probe again (data stays sent meanwhile)
			int                                emptyTries = 0;
			// objects merged into this column's regions (changed only by ScanObjects, which unsends
			// the regions they touch)
			std::shared_ptr<const std::vector<col::Tri>> objTris;
			std::uint64_t                      objHash = 0;
			float                              objYLo = col::kInf, objYHi = -col::kInf;
		};

		struct TrackedObject
		{
			std::int32_t                                 model = -1;
			float                                        m[12]{};  // GTA matrix rows right, up, at, pos
			col::ObjectBox                               box;
			col::ObjClass                                cls = col::ObjClass::kSolid;
			const char*                                  skip = nullptr;  // never solid: why (attached, no physics, ...)
			std::uint32_t                                hashKey = 0;     // model hash (logging)
			std::uint32_t                                seen = 0;
			int                                          still = 0;
			bool                                         movedOnce = false;  // moved since first seen (knocked over): holds nothing back
			bool                                         inside = false;     // the player is inside it: not solid for now
			Clock::time_point                            firstSeen{};
			float                                        lo[3]{}, hi[3]{};   // MC bounds of the grown model box
			std::shared_ptr<const std::vector<col::Tri>> tris;               // null: not probed (yet), or moving
			std::vector<col::OBox>                       boxes;
			std::uint64_t                                hash = 0;
		};

		std::mutex                                     mutex;
		std::condition_variable                        cv;
		std::deque<Job>                                queue;
		std::atomic<std::uint32_t>                     currentEpoch{ 0 };
		std::unordered_map<std::uint64_t, ColumnState> columns;  // (rx, rz)
		std::size_t                                    refreshLeft = 0, refreshChanged = 0;  // Refresh()'s columns
		int                                            lastPrx = 0, lastPrz = 0;            // the player's region column (Update)
		constexpr int                                  kRefreshRadius = 3;                  // Refresh: columns this near (7 x 7)
		std::vector<std::array<int, 3>>                offsets;
		bool                                           started = false;
		std::unique_ptr<col::ColumnProbe>              active;
		std::uint64_t                                  activeKey = 0;
		std::uint32_t                                  frameNo = 0;
		std::unordered_map<const void*, TrackedObject> objects;  // game thread only
		std::unique_ptr<col::ObjectProbe>              objProbe;
		const void*                                    objProbeKey = nullptr;
		Clock::time_point                              objProbeStart{};
		std::vector<const void*>                       objQueue;  // to probe, nearest first
		std::uint32_t                                  objScanNo = 0;
		float                                          objArea[4]{};  // x0, x1, z0, z1 of the object area at the last scan
		std::uint32_t                                  objLogged = 0;
		std::vector<std::int32_t>                      doorModelsLogged;
		bool                                           objListed = false;

		struct AtomicCounters
		{
			std::atomic<std::uint32_t> regions{ 0 }, tris{ 0 }, blocks{ 0 }, columns{ 0 }, columnFailures{ 0 }, collisionRequests{ 0 }, ringWaits{ 0 },
				dropped{ 0 }, clears{ 0 };
			std::atomic<std::uint64_t> buildUs{ 0 };
			std::atomic<std::uint32_t> builds{ 0 };
		} counters;

		// game-thread perf for the 10 s line
		struct Perf10
		{
			std::uint64_t start = 0;
			double        probeMs = 0.0, maxFrameMs = 0.0, waterMs = 0.0;
			std::uint32_t frames = 0, workFrames = 0, vertical = 0, wall = 0, checkRays = 0, bad = 0, checks = 0, changed = 0, probed = 0, regionsQueued = 0;
			std::uint32_t maxRaysFrame = 0;
			// objects
			double        objMs = 0.0, objMaxMs = 0.0, scanMs = 0.0;
			std::uint32_t scans = 0, objProbed = 0, objCells = 0, objEmpty = 0, objBoxes = 0, objMoved = 0, objAborted = 0, objColumns = 0, objMaxRays = 0;
		} perf;

		std::uint64_t Key2(int a_x, int a_z) { return (std::uint64_t(std::uint32_t(a_x)) << 32) | std::uint32_t(a_z); }

		bool Covers(const col::Column& a_c, int a_ry)
		{
			return float(a_ry * col::kRegion) >= a_c.yLo + 2.0f && float(a_ry * col::kRegion + col::kRegion) <= a_c.yHi - 2.0f;
		}

		void Send(const std::vector<std::uint8_t>& a_payload, proto::ColType a_type, std::uint32_t a_epoch)
		{
			auto& link = Link::Get();
			for (int attempt = 0; attempt < 2000; ++attempt) {
				if (link.WriteCollision(a_type, a_payload.data(), static_cast<std::uint32_t>(a_payload.size()))) {
					return;
				}
				if (!link.Valid() || (a_type != proto::kColClear && a_epoch != currentEpoch.load())) {
					break;  // nothing to wait for / stale
				}
				counters.ringWaits.fetch_add(1, std::memory_order_relaxed);
				std::this_thread::sleep_for(1ms);  // ring full: Minecraft is behind (or not running)
			}
			counters.dropped.fetch_add(1, std::memory_order_relaxed);
			LC_LOG_EVERY(5000, "collision ring stayed full (or the link went away); dropped a message (type %u)", static_cast<unsigned>(a_type));
		}

		proto::ColRegion RegionHeader(int a_rx, int a_ry, int a_rz, std::uint32_t a_epoch, std::size_t a_count)
		{
			proto::ColRegion h{};
			h.minX = a_rx * col::kRegion;
			h.minY = a_ry * col::kRegion;
			h.minZ = a_rz * col::kRegion;
			h.maxX = h.minX + col::kRegion - 1;
			h.maxY = h.minY + col::kRegion - 1;
			h.maxZ = h.minZ + col::kRegion - 1;
			h.epoch = a_epoch;
			h.count = static_cast<std::uint32_t>(a_count);
			return h;
		}

		template <class T>
		void SendRegion(proto::ColType a_type, const proto::ColRegion& a_header, const std::vector<T>& a_items, std::uint32_t a_epoch)
		{
			std::vector<std::uint8_t> payload(sizeof(a_header) + a_items.size() * sizeof(T));
			std::memcpy(payload.data(), &a_header, sizeof(a_header));
			if (!a_items.empty()) {
				std::memcpy(payload.data() + sizeof(a_header), a_items.data(), a_items.size() * sizeof(T));
			}
			Send(payload, a_type, a_epoch);
		}

		// A few probes against what a column's data says: did anything stream in or out?
		bool ColumnChanged(const col::Column& a_c)
		{
			for (int j = 0; j < col::kGrid; j += col::kGrid / 2) {
				for (int i = 0; i < col::kGrid; i += col::kGrid / 2) {
					const int        s = col::Column::Index(i, j);
					const col::Event* e = a_c.Ev(s);
					const int        n = a_c.Count(s);
					const col::Event* top = nullptr;
					const col::Event* bottom = nullptr;
					for (int k = 0; k < n; ++k) {
						if (e[k].top && !top) {
							top = &e[k];
						}
						if (!e[k].top) {
							bottom = &e[k];  // the lowest underside
						}
					}
					const float x = a_c.X(i), z = a_c.Z(j);
					for (int down = 0; down < 2; ++down) {
						const float     from[3] = { x, down ? a_c.yHi : a_c.yLo, z }, to[3] = { x, down ? a_c.yLo : a_c.yHi, z };
						col::Hit        h{};
						const bool      hit = col::CastMc(from, to, h);
						++perf.checkRays;
						const col::Event* want = down ? top : bottom;
						if (hit && !std::isfinite(h.pos[1])) {
							continue;  // junk: don't judge
						}
						if (hit != (want != nullptr) || (hit && std::fabs(h.pos[1] - want->y) > 0.05f)) {
							return true;
						}
					}
				}
			}
			return false;
		}

		void SelfTest(const McVec& a_c)
		{
			const float from[3] = { static_cast<float>(a_c.x), static_cast<float>(a_c.y) + 3.0f, static_cast<float>(a_c.z) };
			const float to[3] = { from[0], from[1] - 6.0f, from[2] };
			col::Hit    h{};
			const bool  hit = col::CastMc(from, to, h);
			LC_LOG("collision v2 self-test: probe down at MC (%.2f %.2f %.2f): hit %d at y %.3f normal (%.3f %.3f %.3f)", from[0], a_c.y, from[2], hit, h.pos[1],
				h.n[0], h.n[1], h.n[2]);
			// Is there water anywhere around? (GET_WATER_HEIGHT_NO_WAVES over +-600 m)
			int   wet = 0;
			float lo = 1e9f, hi = -1e9f, best = 1e9f, bx = 0, bz = 0;
			for (int dz = -6; dz <= 6; ++dz) {
				for (int dx = -6; dx <= 6; ++dx) {
					const float x = static_cast<float>(a_c.x) + dx * 100.0f, z = static_cast<float>(a_c.z) + dz * 100.0f;
					float       wh = 0.0f;
					if (::Scripting::GET_WATER_HEIGHT_NO_WAVES(x, -z, static_cast<float>(a_c.y) + 2.0f, &wh) && std::isfinite(wh)) {
						++wet;
						lo = std::min(lo, wh);
						hi = std::max(hi, wh);
						const float d = std::hypot(dx * 100.0f, dz * 100.0f);
						if (d < best) {
							best = d;
							bx = x;
							bz = z;
						}
					}
				}
			}
			LC_LOG("collision v2 self-test: water at %d of 169 points within 600 m (surface %.2f..%.2f), nearest at MC (%.0f, %.0f), %.0f m away", wet,
				wet ? lo : 0.0f, wet ? hi : 0.0f, bx, bz, wet ? best : 0.0f);
		}

		// ---- objects (collision/Objects.h) ------------------------------------------------------------

		std::uint64_t Mix64(std::uint64_t a_x)  // splitmix64: order-independent sums of object hashes
		{
			a_x += 0x9E3779B97F4A7C15ull;
			a_x = (a_x ^ (a_x >> 30)) * 0xBF58476D1CE4E5B9ull;
			a_x = (a_x ^ (a_x >> 27)) * 0x94D049BB133111EBull;
			return a_x ^ (a_x >> 31);
		}

		void ReadMatrix(const CMatrix& a_m, float* a_out)
		{
			const CVector* rows[4] = { &a_m.right, &a_m.up, &a_m.at, &a_m.pos };
			for (int r = 0; r < 4; ++r) {
				a_out[r * 3] = rows[r]->x;
				a_out[r * 3 + 1] = rows[r]->y;
				a_out[r * 3 + 2] = rows[r]->z;
			}
		}

		bool Moved(const float* a_was, const float* a_now)
		{
			for (int k = 0; k < 9; ++k) {
				if (std::fabs(a_was[k] - a_now[k]) > 0.01f) {
					return true;
				}
			}
			for (int k = 9; k < 12; ++k) {
				if (std::fabs(a_was[k] - a_now[k]) > 0.02f) {
					return true;
				}
			}
			return false;
		}

		// Pose and bounds from the object's matrix and model; first sight: what kind of object.
		void Describe(TrackedObject& a_t, CObject* a_obj, bool a_first)
		{
			CBaseModelInfo* mi = a_t.model >= 0 && a_t.model < 31000 ? CModelInfo::ms_modelInfoPtrs[a_t.model] : nullptr;
			if (!mi) {
				a_t.skip = "no model info";
				return;
			}
			const float lo[3] = { mi->m_vMinBounds.x, mi->m_vMinBounds.y, mi->m_vMinBounds.z };
			const float hi[3] = { mi->m_vMaxBounds.x, mi->m_vMaxBounds.y, mi->m_vMaxBounds.z };
			a_t.box = col::MakeObjectBox(a_t.m, a_t.m + 3, a_t.m + 6, a_t.m + 9, lo, hi);
			col::ObjectAabb(a_t.box, col::kObjMargin, a_t.lo, a_t.hi);
			if (!a_first) {
				return;
			}
			a_t.hashKey = mi->m_nHash;
			a_t.cls = col::Classify(a_t.box);
			const auto type = mi->GetModelType();
			if (type == MI_TYPE_WEAPON || type == MI_TYPE_VEHICLE || type == MI_TYPE_PED) {
				a_t.skip = type == MI_TYPE_WEAPON ? "weapon/pickup model" : "vehicle/ped part";
			} else if (!a_obj->m_pInstGta) {
				a_t.skip = "no physics";
			}
			if (a_t.cls == col::ObjClass::kDoor && objLogged < 200 &&
				std::find(doorModelsLogged.begin(), doorModelsLogged.end(), a_t.model) == doorModelsLogged.end() && doorModelsLogged.size() < kObjLogFirst * 2) {
				doorModelsLogged.push_back(a_t.model);
				const auto d = col::DoorMeasure(a_t.box);
				LC_LOG("objects: door (passable) model %d hash 0x%08X at MC (%.2f %.2f %.2f): %.2f wide, %.2f thick, %.2f tall, hinge at an edge %d, "
					   "local bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f)",
					a_t.model, a_t.hashKey, a_t.box.o[0], a_t.box.o[1], a_t.box.o[2], d.width, d.thick, d.height, d.hingeAtEdge, lo[0], lo[1], lo[2], hi[0], hi[1],
					hi[2]);
			}
		}

		bool Solid(const TrackedObject& a_t) { return !a_t.skip && a_t.cls == col::ObjClass::kSolid; }

		void AbortObjectProbe()
		{
			if (objProbe) {
				objProbe.reset();
				objProbeKey = nullptr;
				++perf.objAborted;
			}
		}

		// Commits each object-area column's current object shapes; regions whose objects changed
		// are unsent (the scan in Update sends them again).
		void UpdateObjectColumns(int a_prx, int a_prz)
		{
			const auto              now = Clock::now();
			std::vector<const TrackedObject*> in;
			for (int dz = -kObjRadius; dz <= kObjRadius; ++dz) {
				for (int dx = -kObjRadius; dx <= kObjRadius; ++dx) {
					const int   rx = a_prx + dx, rz = a_prz + dz;
					const float x0 = float(rx * kRegionSize) - 0.5f, x1 = float((rx + 1) * kRegionSize) + 0.5f;
					const float z0 = float(rz * kRegionSize) - 0.5f, z1 = float((rz + 1) * kRegionSize) + 0.5f;
					in.clear();
					std::uint64_t hash = 0;
					bool          held = false;
					for (const auto& [ptr, t] : objects) {
						if (!Solid(t) || t.hi[0] < x0 || t.lo[0] > x1 || t.hi[2] < z0 || t.lo[2] > z1) {
							continue;
						}
						if (!t.tris) {
							if (!t.movedOnce && now - t.firstSeen < kObjHoldMax) {
								held = true;  // not probed yet: don't send the column without it, then with it
								break;
							}
							continue;  // moving (or stuck): not solid for now
						}
						if (t.inside || t.tris->empty()) {
							continue;
						}
						hash += Mix64(t.hash);
						in.push_back(&t);
					}
					if (held) {
						continue;
					}
					auto& st = columns[Key2(rx, rz)];
					if (hash == st.objHash) {
						continue;
					}
					std::shared_ptr<std::vector<col::Tri>> tris;
					float                                  yLo = col::kInf, yHi = -col::kInf;
					if (!in.empty()) {
						tris = std::make_shared<std::vector<col::Tri>>();
						for (const auto* t : in) {
							tris->insert(tris->end(), t->tris->begin(), t->tris->end());
							yLo = std::min(yLo, t->lo[1]);
							yHi = std::max(yHi, t->hi[1]);
						}
					}
					// unsend the regions the old and the new shapes touch
					const float lo = std::min(yLo, st.objYLo), hi = std::max(yHi, st.objYHi);
					if (lo <= hi) {
						const int ry0 = static_cast<int>(std::floor((lo - 0.5f) / kRegionSize)), ry1 = static_cast<int>(std::floor((hi + 0.5f) / kRegionSize));
						st.sent.erase(std::remove_if(st.sent.begin(), st.sent.end(), [&](int ry) { return ry >= ry0 && ry <= ry1; }), st.sent.end());
					}
					st.objHash = hash;
					st.objTris = std::move(tris);
					st.objYLo = yLo;
					st.objYHi = yHi;
					++perf.objColumns;
				}
			}
		}

		// Walks the object pool: tracks the objects around the player, notices what moved or went,
		// queues what needs probing (nearest first) and commits the columns' object shapes.
		void ScanObjects(const McVec& a_c, int a_prx, int a_pry, int a_prz)
		{
			auto* pool = CPools::ms_pObjectPool;
			if (!pool) {
				return;
			}
			const auto t0 = Perf::Now();
			++objScanNo;
			++perf.scans;
			const auto  now = Clock::now();
			const float kx0 = float((a_prx - kObjKeep) * kRegionSize) - kObjReach, kx1 = float((a_prx + kObjKeep + 1) * kRegionSize) + kObjReach;
			const float kz0 = float((a_prz - kObjKeep) * kRegionSize) - kObjReach, kz1 = float((a_prz + kObjKeep + 1) * kRegionSize) + kObjReach;
			const float ky0 = float((a_pry - kBelow) * kRegionSize) - kObjReach, ky1 = float((a_pry + kAbove + 1) * kRegionSize) + kObjReach;
			const float px = static_cast<float>(a_c.x), py = static_cast<float>(a_c.y), pz = static_cast<float>(a_c.z);
			for (int slot = pool->FindNextUsed(0); slot >= 0; slot = pool->FindNextUsed(slot + 1)) {
				CObject* obj = pool->Get(slot);
				if (!obj || !obj->m_pMatrix) {
					continue;
				}
				float m[12];
				ReadMatrix(*obj->m_pMatrix, m);
				const float mx = m[9], my = m[11], mz = -m[10];  // MC
				if (!(mx >= kx0 && mx <= kx1 && mz >= kz0 && mz <= kz1 && my >= ky0 && my <= ky1)) {
					continue;
				}
				auto [it, fresh] = objects.try_emplace(obj);
				TrackedObject& t = it->second;
				if (fresh || t.model != obj->m_nModelIndex) {
					if (!fresh && objProbeKey == obj) {
						AbortObjectProbe();
					}
					t = TrackedObject{};
					t.model = obj->m_nModelIndex;
					std::memcpy(t.m, m, sizeof(m));
					t.firstSeen = now;
					Describe(t, obj, true);
				} else if (Moved(t.m, m) || (obj->m_pAttachedToEntity && !t.skip)) {
					std::memcpy(t.m, m, sizeof(m));
					Describe(t, obj, false);
					t.still = 0;
					t.movedOnce = true;
					if (t.tris) {
						t.tris.reset();
						t.boxes.clear();
						++perf.objMoved;
					}
					if (objProbeKey == obj) {
						AbortObjectProbe();
					}
				} else {
					++t.still;
				}
				t.seen = objScanNo;
				if (obj->m_pAttachedToEntity) {
					t.still = 0;  // held by a ped or hanging off a car: not until it's let go and still
				}
				bool inside = false;
				for (const auto& b : t.boxes) {
					if (col::OBoxNear(b, px, py + 0.1f, py + 1.7f, pz, kObjInside)) {
						inside = true;
						break;
					}
				}
				t.inside = inside;
			}
			for (auto it = objects.begin(); it != objects.end();) {
				if (it->second.seen != objScanNo) {
					if (objProbeKey == it->first) {
						AbortObjectProbe();
					}
					it = objects.erase(it);
				} else {
					++it;
				}
			}
			// what to probe: solid, still, not probed, in the object area; nearest first
			const float ax0 = float((a_prx - kObjRadius) * kRegionSize) - kObjReach, ax1 = float((a_prx + kObjRadius + 1) * kRegionSize) + kObjReach;
			const float az0 = float((a_prz - kObjRadius) * kRegionSize) - kObjReach, az1 = float((a_prz + kObjRadius + 1) * kRegionSize) + kObjReach;
			objArea[0] = ax0;
			objArea[1] = ax1;
			objArea[2] = az0;
			objArea[3] = az1;
			objQueue.clear();
			for (const auto& [ptr, t] : objects) {
				if (Solid(t) && !t.tris && t.still >= kObjStillScans && ptr != objProbeKey && t.hi[0] >= ax0 && t.lo[0] <= ax1 && t.hi[2] >= az0 && t.lo[2] <= az1) {
					objQueue.push_back(ptr);
				}
			}
			auto d2 = [&](const void* a_p) {
				const auto& t = objects[a_p];
				const float dx = t.box.o[0] - px, dy = t.box.o[1] - py, dz = t.box.o[2] - pz;
				return dx * dx + dy * dy + dz * dz;
			};
			std::sort(objQueue.begin(), objQueue.end(), [&](const void* a, const void* b) { return d2(a) > d2(b); });  // nearest at the back
			UpdateObjectColumns(a_prx, a_prz);

			if (!objListed && !objects.empty() && objScanNo > 4) {
				objListed = true;
				std::vector<std::pair<float, const void*>> nearest;
				for (const auto& [ptr, t] : objects) {
					nearest.push_back({ d2(ptr), ptr });
				}
				std::sort(nearest.begin(), nearest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				LC_LOG("objects: %zu tracked around MC (%.1f %.1f %.1f); the nearest:", objects.size(), px, py, pz);
				for (std::size_t i = 0; i < nearest.size() && i < 40; ++i) {
					const auto& t = objects[nearest[i].second];
					LC_LOG("objects:   model %5d hash 0x%08X at MC (%7.2f %6.2f %7.2f) %4.1f m: %s%s%s, local (%.2f %.2f %.2f)..(%.2f %.2f %.2f), up.y %.2f", t.model,
						t.hashKey, t.box.o[0], t.box.o[1], t.box.o[2], std::sqrt(nearest[i].first), col::ObjClassName(t.cls), t.skip ? ", skipped: " : "",
						t.skip ? t.skip : "", t.box.lo[0], t.box.lo[1], t.box.lo[2], t.box.hi[0], t.box.hi[1], t.box.hi[2], t.box.axes[2][1]);
				}
			}
			perf.scanMs += double(Perf::Now() - t0) / Perf::TicksPerMs();
		}

		// Probes queued objects until a_deadline.
		void ProbeObjects(Clock::time_point a_deadline)
		{
			auto outOfTime = [&] { return Clock::now() >= a_deadline; };
			while (!outOfTime()) {
				if (!objProbe) {
					while (!objQueue.empty()) {
						const void* key = objQueue.back();
						objQueue.pop_back();
						auto it = objects.find(key);
						if (it != objects.end() && Solid(it->second) && !it->second.tris && it->second.still >= kObjStillScans) {
							objProbe = std::make_unique<col::ObjectProbe>(it->second.box);
							objProbeKey = key;
							objProbeStart = Clock::now();
							break;
						}
					}
					if (!objProbe) {
						return;
					}
				}
				const void* key = objProbeKey;
				auto        ray = [key](const float* a_from, const float* a_to, col::Hit& a_hit) { return col::CastObjectMc(a_from, a_to, key, a_hit); };
				if (!objProbe->Run(ray, outOfTime)) {
					return;
				}
				// done
				auto it = objects.find(key);
				if (it != objects.end()) {
					auto& t = it->second;
					objProbe->Boxes(t.boxes);
					auto tris = std::make_shared<std::vector<col::Tri>>();
					col::BoxTris(t.boxes, *tris);
					t.hash = col::TrisHash(*tris) ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key));
					++perf.objProbed;
					perf.objCells += static_cast<std::uint32_t>(objProbe->Cells());
					perf.objBoxes += static_cast<std::uint32_t>(t.boxes.size());
					perf.objMaxRays = std::max(perf.objMaxRays, objProbe->rays);
					if (tris->empty()) {
						++perf.objEmpty;
					}
					if (objLogged < kObjLogFirst) {
						++objLogged;
						float lo[3] = { col::kInf, col::kInf, col::kInf }, hi[3] = { -col::kInf, -col::kInf, -col::kInf };
						for (const auto& tri : *tris) {
							float tl[3], th[3];
							col::TriBounds(tri, tl, th);
							for (int k = 0; k < 3; ++k) {
								lo[k] = std::min(lo[k], tl[k]);
								hi[k] = std::max(hi[k], th[k]);
							}
						}
						LC_LOG("objects: probed model %d hash 0x%08X at MC (%.2f %.2f %.2f): %d cells of %.3f, %u rays (%u hits, %u bad, %u unpaired), "
							   "%zu boxes, %zu tris, shape MC (%.2f %.2f %.2f)..(%.2f %.2f %.2f), %.1f ms",
							t.model, t.hashKey, t.box.o[0], t.box.o[1], t.box.o[2], objProbe->Cells(), objProbe->Step(), objProbe->rays, objProbe->hits, objProbe->bad,
							objProbe->unpaired, t.boxes.size(), tris->size(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
							std::chrono::duration<double, std::milli>(Clock::now() - objProbeStart).count());
					}
					t.tris = std::move(tris);
				}
				objProbe.reset();
				objProbeKey = nullptr;
			}
		}

		void LogObjects(double a_secs)
		{
			std::uint32_t solid = 0, probed = 0, doors = 0, tiny = 0, huge = 0, skipped = 0, waiting = 0, inside = 0, outside = 0;
			for (const auto& [ptr, t] : objects) {
				if (t.skip) {
					++skipped;
				} else if (t.cls == col::ObjClass::kDoor) {
					++doors;
				} else if (t.cls == col::ObjClass::kTiny) {
					++tiny;
				} else if (t.cls != col::ObjClass::kSolid) {
					++huge;
				} else {
					++solid;
					probed += t.tris ? 1 : 0;
					const bool in = t.hi[0] >= objArea[0] && t.lo[0] <= objArea[1] && t.hi[2] >= objArea[2] && t.lo[2] <= objArea[3];
					waiting += !t.tris && in ? 1 : 0;
					outside += !t.tris && !in ? 1 : 0;
					inside += t.inside ? 1 : 0;
				}
			}
			const auto& r = col::objectRays;
			LC_LOG("collision objects %.0fs: %zu tracked: %u solid (%u probed, %u waiting or moving, %u kept outside the area, %u around the player), %u doors, %u tiny, %u huge/bad, %u skipped; "
				   "probed %u (%u cells, %u boxes, %u empty, max %u rays), %u rays (%u hits: %u on target, %u others skipped, %u unknown), "
				   "probe time %.1f ms (%.3f ms/frame, max %.2f), %u scans %.3f ms avg, %u column updates, %u moved, %u aborted",
				a_secs, objects.size(), solid, probed, waiting, outside, inside, doors, tiny, huge, skipped, perf.objProbed, perf.objCells, perf.objBoxes, perf.objEmpty,
				perf.objMaxRays, r.rays, r.hits, r.target, r.other, r.unknown, perf.objMs, perf.frames ? perf.objMs / perf.frames : 0.0, perf.objMaxMs, perf.scans,
				perf.scans ? perf.scanMs / perf.scans : 0.0, perf.objColumns, perf.objMoved, perf.objAborted);
			col::objectRays = {};
		}

		void LogPerf(std::uint64_t a_nowMs)
		{
			if (perf.start == 0) {
				perf.start = a_nowMs;
				return;
			}
			if (a_nowMs - perf.start < 10000) {
				return;
			}
			const double secs = double(a_nowMs - perf.start) / 1000.0;
			const auto   builds = counters.builds.exchange(0);
			const auto   buildUs = counters.buildUs.exchange(0);
			std::size_t  queued = 0;
			{
				std::lock_guard lock(mutex);
				queued = queue.size();
			}
			LC_LOG("collision v2 %.0fs: %u columns probed (%.1f/s), probes: %u vertical + %u wall + %u check (%u bad), %.0f/frame avg, max %u; "
				   "probe time %.1f ms (%.2f ms/frame avg over %u frames, %.2f max), %u checks (%u changed), %u regions queued (%.1f/s), "
				   "worker %u columns %.1f ms avg, queue %zu; water %u writes %.2f ms/write, %u wet cells, %u suppressed",
				secs, perf.probed, perf.probed / secs, perf.vertical, perf.wall, perf.checkRays, perf.bad,
				perf.frames ? double(perf.vertical + perf.wall + perf.checkRays) / perf.frames : 0.0, perf.maxRaysFrame, perf.probeMs,
				perf.frames ? perf.probeMs / perf.frames : 0.0, perf.frames, perf.maxFrameMs, perf.checks, perf.changed, perf.regionsQueued,
				perf.regionsQueued / secs, builds, builds ? double(buildUs) / builds / 1000.0 : 0.0, queued, col::waterStats.writes,
				col::waterStats.writes ? perf.waterMs / col::waterStats.writes : 0.0, col::waterStats.wetCells, col::waterStats.suppressed);
			LogObjects(secs);
			perf = {};
			perf.start = a_nowMs;
			col::waterStats = {};
			col::rayCounters = {};
		}
	}

	Collision& Collision::Get()
	{
		static Collision instance;
		return instance;
	}

	void Collision::Start()
	{
		if (started) {
			return;
		}
		started = true;
		for (int dx = -kRadius; dx <= kRadius; ++dx) {
			for (int dz = -kRadius; dz <= kRadius; ++dz) {
				for (int dy = -kBelow; dy <= kAbove; ++dy) {
					offsets.push_back({ dx, dy, dz });
				}
			}
		}
		std::stable_sort(offsets.begin(), offsets.end(),
			[](const auto& a, const auto& b) { return a[0] * a[0] + a[2] * a[2] + a[1] * a[1] * 2 < b[0] * b[0] + b[2] * b[2] + b[1] * b[1] * 2; });
		std::thread([this] { WorkerLoop(); }).detach();
		LC_LOG("collision v2 (line probes: floors, ceilings, walls) started: %zu regions around the player, %.0f us probe budget a frame", offsets.size(),
			std::chrono::duration<double, std::micro>(kFrameBudget).count());
	}

	void Collision::Reset(std::uint32_t a_epoch)
	{
		Start();
		currentEpoch = a_epoch;
		columns.clear();
		refreshLeft = refreshChanged = 0;
		active.reset();
		objLogged = std::min<std::uint32_t>(objLogged, kObjLogFirst - 10);  // a few more probe lines for the new world
		std::lock_guard lock(mutex);
		queue.clear();
		Job job{};
		job.clear = true;
		job.epoch = a_epoch;
		queue.push_back(job);
		cv.notify_one();
	}

	void Collision::Refresh()
	{
		std::size_t n = 0;
		for (auto& [key, st] : columns) {
			const int cx = static_cast<int>(static_cast<std::int32_t>(key >> 32)), cz = static_cast<int>(static_cast<std::int32_t>(key & 0xFFFFFFFF));
			if (st.data && !st.stale && std::abs(cx - lastPrx) <= kRefreshRadius && std::abs(cz - lastPrz) <= kRefreshRadius) {
				st.stale = true;
				++n;
			}
		}
		refreshLeft += n;
		refreshChanged = 0;
		LC_LOG("collision: %zu columns to probe again (interior change; nothing cleared)", n);
	}

	void Collision::Update(const McVec& a_centerMc, float a_feetGtaZ)
	{
		Start();
		(void)a_feetGtaZ;
		static bool selfTested = false;
		if (!selfTested) {
			selfTested = true;
			SelfTest(a_centerMc);
		}
		++frameNo;
		const auto frameStart = Clock::now();
		const auto deadline = frameStart + kFrameBudget;
		const auto t0 = Perf::Now();
		const auto rays0 = col::rayCounters.rays;

		if (frameNo % kWaterEvery == 0) {
			const auto w0 = Perf::Now();
			col::WriteWater(a_centerMc);
			perf.waterMs += double(Perf::Now() - w0) / Perf::TicksPerMs();
		}

		const int  prx = static_cast<int>(std::floor(a_centerMc.x / kRegionSize));
		const int  pry = static_cast<int>(std::floor(a_centerMc.y / kRegionSize));
		const int  prz = static_cast<int>(std::floor(a_centerMc.z / kRegionSize));
		const auto epoch = currentEpoch.load();
		lastPrx = prx, lastPrz = prz;

		// Street furniture: who's around, what moved; their shapes go into the columns' regions.
		if (frameNo % kObjScanEvery == 0) {
			ScanObjects(a_centerMc, prx, pry, prz);
		}
		// The map's columns first, but while objects wait some of the budget is theirs.
		const auto mapDeadline = (objProbe || !objQueue.empty()) ? deadline - kObjReserve : deadline;
		auto       outOfTime = [&] { return Clock::now() >= mapDeadline; };
		auto       ray = [](const float* a_from, const float* a_to, col::Hit& a_hit) { return col::CastMc(a_from, a_to, a_hit); };

		// A column finished probing: keep it (or retry later if nothing was loaded there yet).
		auto finish = [&] {
			auto        data = active->Result();
			auto&       st = columns[activeKey];
			perf.vertical += active->verticalRays;
			perf.wall += active->wallRays;
			perf.bad += active->badHits;
			if (data->emptySamples == static_cast<std::uint32_t>(col::kSamples)) {
				// Not a single surface in the span: not streamed in yet, or open air / deep water.
				// GET_GROUND_Z_FOR_3D_COORD says 0 where nothing is loaded at all.
				const float cx = float(data->rx * col::kRegion + col::kRegion / 2), cz = float(data->rz * col::kRegion + col::kRegion / 2);
				float       ground = 0.0f;
				::Scripting::GET_GROUND_Z_FOR_3D_COORD(cx, -cz, 1000.0f, &ground);
				if (ground == 0.0f && st.emptyTries < kMaxEmptyRetries) {
					++st.emptyTries;
					st.retryAt = Clock::now() + kRetryEmpty;
					st.requestedOnce = false;  // ask again
					counters.columnFailures.fetch_add(1, std::memory_order_relaxed);
					active.reset();
					return;
				}
			}
			st.emptyTries = 0;
			const auto hash = data->Hash();
			if (!st.data || hash != st.hash) {
				st.sent.clear();  // (re)send every region from the new data
			}
			if (st.stale) {
				refreshChanged += st.data && hash != st.hash ? 1 : 0;
				if (refreshLeft && --refreshLeft == 0) {
					LC_LOG("collision: interior refresh done, %zu columns changed (sent again)", refreshChanged);
				}
			}
			st.stale = false;
			st.hash = hash;
			st.data = data;
			st.checked = Clock::now();
			++perf.probed;
			counters.columns.fetch_add(1, std::memory_order_relaxed);
			active.reset();
		};

		if (active && active->Run(ray, outOfTime)) {
			finish();
		}

		// What's due, nearest first: regions whose column is probed go to the worker; the first
		// column that isn't gets probed.
		struct Pending
		{
			std::uint64_t                      key;
			int                                rx, rz;
			std::shared_ptr<const col::Column> data;
			std::vector<int>                   rys;
			std::shared_ptr<const std::vector<col::Tri>> objTris;
		};
		std::vector<Pending> pending;
		int                  queuedRegions = 0;
		for (const auto& o : offsets) {
			if (queuedRegions >= kMaxRegionsPerFrame) {
				break;
			}
			const int  rx = prx + o[0], ry = pry + o[1], rz = prz + o[2];
			const auto key = Key2(rx, rz);
			auto&      st = columns[key];
			if (st.data && Covers(*st.data, ry)) {
				if (st.stale && !active && !outOfTime()) {
					// Refresh: probe it again; its regions stay with Minecraft until the new data differs.
					const float yLo = float((std::min(pry, ry) - kBelow - kSpanExtra) * kRegionSize);
					const float yHi = float((std::max(pry, ry) + kAbove + 1 + kSpanExtra) * kRegionSize);
					active = std::make_unique<col::ColumnProbe>(rx, rz, yLo, yHi);
					activeKey = key;
					if (active->Run(ray, outOfTime)) {
						finish();
					}
				}
				if (std::find(st.sent.begin(), st.sent.end(), ry) == st.sent.end()) {
					st.sent.push_back(ry);
					auto it = std::find_if(pending.begin(), pending.end(), [&](const Pending& p) { return p.key == key; });
					if (it == pending.end()) {
						pending.push_back({ key, rx, rz, st.data, {}, st.objTris });
						it = pending.end() - 1;
					}
					it->rys.push_back(ry);
					++queuedRegions;
				}
				continue;
			}
			if (active || outOfTime() || Clock::now() < st.retryAt) {
				continue;
			}
			const bool nearColumn = std::abs(o[0]) <= 1 && std::abs(o[2]) <= 1;
			if (!nearColumn && !st.requestedOnce) {
				const GtaVec c = McToGta(double(rx * kRegionSize + kRegionSize / 2), a_centerMc.y, double(rz * kRegionSize + kRegionSize / 2));
				::Scripting::REQUEST_COLLISION_AT_POSN(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z));
				st.requestedOnce = true;
				st.requested = Clock::now();
				counters.collisionRequests.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			if (!nearColumn && Clock::now() - st.requested < kRequestLead) {
				continue;
			}
			const float yLo = float((std::min(pry, ry) - kBelow - kSpanExtra) * kRegionSize);
			const float yHi = float((std::max(pry, ry) + kAbove + 1 + kSpanExtra) * kRegionSize);
			active = std::make_unique<col::ColumnProbe>(rx, rz, yLo, yHi);
			activeKey = key;
			if (active->Run(ray, outOfTime)) {
				finish();
			}
		}
		if (!pending.empty()) {
			std::lock_guard lock(mutex);
			for (auto& p : pending) {
				Job job{};
				job.rx = p.rx;
				job.rz = p.rz;
				job.epoch = epoch;
				job.data = std::move(p.data);
				job.rys = std::move(p.rys);
				job.objTris = std::move(p.objTris);
				queue.push_back(std::move(job));
			}
			cv.notify_one();
			perf.regionsQueued += static_cast<std::uint32_t>(queuedRegions);
		}

		// Re-check one of the 3x3 columns around the player now and then (late streaming).
		if (!outOfTime()) {
			const auto now = Clock::now();
			for (int dz = -1; dz <= 1; ++dz) {
				for (int dx = -1; dx <= 1; ++dx) {
					auto it = columns.find(Key2(prx + dx, prz + dz));
					if (it == columns.end() || !it->second.data || now - it->second.checked < kCheckNear || (active && activeKey == it->first)) {
						continue;
					}
					it->second.checked = now;
					++perf.checks;
					if (ColumnChanged(*it->second.data)) {
						++perf.changed;
						it->second.data.reset();  // re-probed by the scan; regions resent if the data differs
						it->second.hash = 0;
					}
					dz = dx = 2;  // one a frame
				}
			}
		}

		// Objects get the rest of the frame's budget.
		{
			const auto o0 = Perf::Now();
			ProbeObjects(deadline);
			const double oms = double(Perf::Now() - o0) / Perf::TicksPerMs();
			perf.objMs += oms;
			perf.objMaxMs = std::max(perf.objMaxMs, oms);
		}

		// Bound memory: forget columns well outside the harvest area.
		if (frameNo % 120 == 0 && columns.size() > std::size_t((2 * kRadius + 5) * (2 * kRadius + 5))) {
			for (auto it = columns.begin(); it != columns.end();) {
				const int cx = static_cast<int>(static_cast<std::int32_t>(it->first >> 32)), cz = static_cast<int>(static_cast<std::int32_t>(it->first & 0xFFFFFFFF));
				if ((std::abs(cx - prx) > kRadius + 2 || std::abs(cz - prz) > kRadius + 2) && !(active && activeKey == it->first)) {
					refreshLeft -= it->second.stale && refreshLeft ? 1 : 0;
					it = columns.erase(it);
				} else {
					++it;
				}
			}
		}

		const double ms = double(Perf::Now() - t0) / Perf::TicksPerMs();
		const auto   rays = col::rayCounters.rays - rays0;
		++perf.frames;
		perf.probeMs += ms;
		perf.maxFrameMs = std::max(perf.maxFrameMs, ms);
		perf.maxRaysFrame = std::max(perf.maxRaysFrame, rays);
		LogPerf(::GetTickCount64());
	}

	void Collision::WorkerLoop()
	{
		std::vector<col::Tri>        all, regionTris;
		std::vector<proto::ColTri>   out;
		std::vector<proto::ColBlock> blocks;
		for (;;) {
			Job job;
			{
				std::unique_lock lock(mutex);
				cv.wait(lock, [] { return !queue.empty(); });
				job = std::move(queue.front());
				queue.pop_front();
			}
			if (job.clear) {
				std::vector<std::uint8_t> payload(4);
				std::memcpy(payload.data(), &job.epoch, 4);
				Send(payload, proto::kColClear, job.epoch);
				counters.clears.fetch_add(1, std::memory_order_relaxed);
				LC_LOG("kColClear epoch %u sent", job.epoch);
				continue;
			}
			if (job.epoch != currentEpoch.load() || !job.data) {
				continue;
			}
			const auto t0 = Clock::now();
			col::BuildColumn(*job.data, all);
			if (job.objTris) {
				all.insert(all.end(), job.objTris->begin(), job.objTris->end());
			}
			for (int ry : job.rys) {
				if (job.epoch != currentEpoch.load()) {
					break;
				}
				col::RegionTris(all, job.rx, ry, job.rz, regionTris);
				out.clear();
				out.reserve(regionTris.size());
				for (const auto& t : regionTris) {
					proto::ColTri c{};
					std::memcpy(c.v, t.v, sizeof(c.v));
					c.flags = t.flags;
					out.push_back(c);
				}
				// triangles first, like SkyCraft (kColTris precedes kColRegion)
				SendRegion(proto::kColTris, RegionHeader(job.rx, ry, job.rz, job.epoch, out.size()), out, job.epoch);
				counters.tris.fetch_add(static_cast<std::uint32_t>(out.size()), std::memory_order_relaxed);
				col::Voxelize(regionTris, job.rx, ry, job.rz, blocks);
				SendRegion(proto::kColRegion, RegionHeader(job.rx, ry, job.rz, job.epoch, blocks.size()), blocks, job.epoch);
				counters.blocks.fetch_add(static_cast<std::uint32_t>(blocks.size()), std::memory_order_relaxed);
				counters.regions.fetch_add(1, std::memory_order_relaxed);
			}
			counters.buildUs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()),
				std::memory_order_relaxed);
			counters.builds.fetch_add(1, std::memory_order_relaxed);
		}
	}

	Collision::Counters Collision::TakeCounters()
	{
		auto take = [](std::atomic<std::uint32_t>& a_c) { return a_c.exchange(0, std::memory_order_relaxed); };
		Counters c{ take(counters.regions), take(counters.tris), take(counters.blocks), take(counters.columns), take(counters.columnFailures),
			take(counters.collisionRequests), take(counters.ringWaits), take(counters.dropped), take(counters.clears), 0, Link::Get().CollisionPending() };
		{
			std::lock_guard lock(mutex);
			c.queued = static_cast<std::uint32_t>(queue.size());
		}
		return c;
	}
}
