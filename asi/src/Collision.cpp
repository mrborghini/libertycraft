// Unity-built into dllmain.cpp (needs IV-SDK). See Collision.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "collision"
#include "Collision.h"

#include "Link.h"
#include "Log.h"
#include "Perf.h"
#include "collision/Geometry.h"
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

		struct Job
		{
			int                                  rx = 0, rz = 0;
			std::uint32_t                        epoch = 0;
			bool                                 clear = false;
			std::shared_ptr<const col::Column>   data;
			std::vector<int>                     rys;
		};

		struct ColumnState
		{
			std::shared_ptr<const col::Column> data;
			std::uint64_t                      hash = 0;
			std::vector<int>                   sent;  // regions (ry) sent from data
			Clock::time_point                  requested{}, retryAt{}, checked{};
			bool                               requestedOnce = false;
			int                                emptyTries = 0;
		};

		std::mutex                                     mutex;
		std::condition_variable                        cv;
		std::deque<Job>                                queue;
		std::atomic<std::uint32_t>                     currentEpoch{ 0 };
		std::unordered_map<std::uint64_t, ColumnState> columns;  // (rx, rz)
		std::vector<std::array<int, 3>>                offsets;
		bool                                           started = false;
		std::unique_ptr<col::ColumnProbe>              active;
		std::uint64_t                                  activeKey = 0;
		std::uint32_t                                  frameNo = 0;

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
		active.reset();
		std::lock_guard lock(mutex);
		queue.clear();
		Job job{};
		job.clear = true;
		job.epoch = a_epoch;
		queue.push_back(job);
		cv.notify_one();
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
		auto       outOfTime = [&] { return Clock::now() >= deadline; };
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
				if (std::find(st.sent.begin(), st.sent.end(), ry) == st.sent.end()) {
					st.sent.push_back(ry);
					auto it = std::find_if(pending.begin(), pending.end(), [&](const Pending& p) { return p.key == key; });
					if (it == pending.end()) {
						pending.push_back({ key, rx, rz, st.data, {} });
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

		// Bound memory: forget columns well outside the harvest area.
		if (frameNo % 120 == 0 && columns.size() > std::size_t((2 * kRadius + 5) * (2 * kRadius + 5))) {
			for (auto it = columns.begin(); it != columns.end();) {
				const int cx = static_cast<int>(static_cast<std::int32_t>(it->first >> 32)), cz = static_cast<int>(static_cast<std::int32_t>(it->first & 0xFFFFFFFF));
				if ((std::abs(cx - prx) > kRadius + 2 || std::abs(cz - prz) > kRadius + 2) && !(active && activeKey == it->first)) {
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
