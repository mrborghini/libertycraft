// Unity-built into dllmain.cpp (needs IV-SDK). See Collision.h.
#include "Sdk.h"

#undef LC_MODULE
#define LC_MODULE "collision"
#include "Collision.h"

#include "Config.h"
#include "Link.h"
#include "Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
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

		constexpr int   kRadius = 5;  // regions around the player horizontally
		constexpr int   kBelow = 3;   // regions below the player
		constexpr int   kAbove = 2;   // regions above the player
		constexpr auto  kRefreshNear = 1000ms;     // re-send regions next to the player this often
		constexpr auto  kFrameBudget = 2500us;     // probing time per frame
		constexpr int   kMaxColumnsPerFrame = 3;   // column probes (81 natives each) per frame
		constexpr int   kMaxRegionsPerFrame = 24;  // region jobs queued per frame (cheap once probed)
		constexpr auto  kRequestLead = 300ms;      // REQUEST_COLLISION_AT_POSN this long before probing a far column
		constexpr auto  kRetryMissing = 500ms;     // a column with no ground yet: try again after this
		constexpr float kTopProbeZ = 1000.0f;
		constexpr float kWalkableNy = 0.7f;

		struct Job
		{
			int                         rx = 0, ry = 0, rz = 0;
			std::uint32_t               epoch = 0;
			bool                        clear = false;
			Collision::ColumnHeights    heights{};
		};

		struct Column
		{
			Collision::ColumnHeights heights{};
			Clock::time_point        probed{};
			Clock::time_point        requested{};
			Clock::time_point        retryAt{};
			bool                     valid = false;
			bool                     requestedOnce = false;
		};

		std::mutex                                     mutex;
		std::condition_variable                        cv;
		std::deque<Job>                                queue;
		std::atomic<std::uint32_t>                     currentEpoch{ 0 };
		std::unordered_map<std::uint64_t, Clock::time_point> harvested;  // region -> when
		std::unordered_map<std::uint64_t, Column>      columns;    // (rx, rz) -> heights
		std::vector<std::array<int, 3>>                offsets;
		bool                                           started = false;

		struct AtomicCounters
		{
			std::atomic<std::uint32_t> regions{ 0 }, tris{ 0 }, blocks{ 0 }, columns{ 0 }, columnFailures{ 0 }, collisionRequests{ 0 }, ringWaits{ 0 },
				dropped{ 0 }, clears{ 0 };
		} counters;

		std::uint64_t Key3(int a_x, int a_y, int a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) |
			       std::uint64_t(std::uint32_t(a_z) & 0x1FFFFF);
		}

		std::uint64_t Key2(int a_x, int a_z) { return (std::uint64_t(std::uint32_t(a_x)) << 32) | std::uint32_t(a_z); }

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

		// Height of the heightfield at MC (x, z) inside the region column, matching the triangles:
		// each block cell (i, j) is split along its (i, j+1)-(i+1, j) diagonal.
		float SurfaceAt(const Collision::ColumnHeights& a_h, float a_u, float a_v)  // a_u, a_v in [0, 8)
		{
			constexpr int n = Collision::kCorners;
			const int     i = std::min(static_cast<int>(a_u), Collision::kRegionSize - 1);
			const int     j = std::min(static_cast<int>(a_v), Collision::kRegionSize - 1);
			const float   u = a_u - float(i), v = a_v - float(j);
			const float   h00 = a_h.h[j * n + i], h10 = a_h.h[j * n + i + 1], h01 = a_h.h[(j + 1) * n + i], h11 = a_h.h[(j + 1) * n + i + 1];
			if (u + v <= 1.0f) {
				return h00 + (h10 - h00) * u + (h01 - h00) * v;
			}
			return h11 + (h01 - h11) * (1.0f - u) + (h10 - h11) * (1.0f - v);
		}

		void SendTriangles(const Job& a_job)
		{
			constexpr int n = Collision::kCorners;
			const float   x0 = float(a_job.rx * Collision::kRegionSize), z0 = float(a_job.rz * Collision::kRegionSize);
			const float   loY = float(a_job.ry * Collision::kRegionSize) - 0.5f, hiY = loY + Collision::kRegionSize + 1.0f;
			std::vector<proto::ColTri> out;
			out.reserve(2 * Collision::kRegionSize * Collision::kRegionSize);
			auto add = [&](const float a_a[3], const float a_b[3], const float a_c[3]) {
				const float lo = std::min({ a_a[1], a_b[1], a_c[1] }), hi = std::max({ a_a[1], a_b[1], a_c[1] });
				if (hi < loY || lo > hiY) {
					return;
				}
				proto::ColTri t{};
				std::memcpy(t.v, a_a, 12);
				std::memcpy(t.v + 3, a_b, 12);
				std::memcpy(t.v + 6, a_c, 12);
				// normal = (b - a) x (c - a); its y decides dirt (walkable) or stone (steep)
				const float ux = a_b[0] - a_a[0], uy = a_b[1] - a_a[1], uz = a_b[2] - a_a[2];
				const float wx = a_c[0] - a_a[0], wy = a_c[1] - a_a[1], wz = a_c[2] - a_a[2];
				const float qx = uy * wz - uz * wy, qy = uz * wx - ux * wz, qz = ux * wy - uy * wx;
				const float len = std::sqrt(qx * qx + qy * qy + qz * qz);
				const auto  material = len > 0.0f && qy / len >= kWalkableNy ? proto::kDigDirt : proto::kDigStone;
				t.flags = proto::kTriTerrain | proto::kTriDiggable | (std::uint32_t(material) << proto::kTriMaterialShift);
				out.push_back(t);
			};
			for (int j = 0; j < Collision::kRegionSize; ++j) {
				for (int i = 0; i < Collision::kRegionSize; ++i) {
					const float x = x0 + float(i), z = z0 + float(j);
					const float a[3] = { x, a_job.heights.h[j * n + i], z };                  // (i, j)
					const float b[3] = { x, a_job.heights.h[(j + 1) * n + i], z + 1.0f };     // (i, j+1)
					const float c[3] = { x + 1.0f, a_job.heights.h[j * n + i + 1], z };       // (i+1, j)
					const float d[3] = { x + 1.0f, a_job.heights.h[(j + 1) * n + i + 1], z + 1.0f };  // (i+1, j+1)
					// Outward winding (normal up, out of the ground): (b - a) x (c - a) has +y.
					add(a, b, c);
					add(c, b, d);
				}
			}
			proto::ColRegion header{};
			header.minX = a_job.rx * Collision::kRegionSize;
			header.minY = a_job.ry * Collision::kRegionSize;
			header.minZ = a_job.rz * Collision::kRegionSize;
			header.maxX = header.minX + Collision::kRegionSize - 1;
			header.maxY = header.minY + Collision::kRegionSize - 1;
			header.maxZ = header.minZ + Collision::kRegionSize - 1;
			header.epoch = a_job.epoch;
			header.count = static_cast<std::uint32_t>(out.size());
			std::vector<std::uint8_t> payload(sizeof(header) + out.size() * sizeof(proto::ColTri));
			std::memcpy(payload.data(), &header, sizeof(header));
			if (!out.empty()) {
				std::memcpy(payload.data() + sizeof(header), out.data(), out.size() * sizeof(proto::ColTri));
			}
			Send(payload, proto::kColTris, a_job.epoch);
			counters.tris.fetch_add(header.count, std::memory_order_relaxed);
		}

		// Every 1/8-block voxel whose centre is at or below the surface is solid.
		void Voxelize(const Job& a_job)
		{
			constexpr int R = Collision::kRegionSize;
			std::array<proto::ColBlock, R * R * R> grid{};  // [bx + R * (bz + R * by)]
			const int baseY = a_job.ry * R;
			for (int vz = 0; vz < R * 8; ++vz) {
				for (int vx = 0; vx < R * 8; ++vx) {
					const float h = SurfaceAt(a_job.heights, (float(vx) + 0.5f) / 8.0f, (float(vz) + 0.5f) / 8.0f);
					const int   bx = vx >> 3, bz = vz >> 3;
					const auto  bit = std::uint64_t(1) << ((vz & 7) * 8 + (vx & 7));
					for (int by = 0; by < R; ++by) {
						// voxel sy is solid when baseY + by + (sy + 0.5) / 8 <= h
						const float rel = (h - float(baseY + by)) * 8.0f - 0.5f;
						if (rel < 0.0f) {
							break;  // this block and every one above it are above the surface here
						}
						const int count = std::min(8, static_cast<int>(std::floor(rel)) + 1);
						auto&     blk = grid[bx + R * (bz + R * by)];
						for (int sy = 0; sy < count; ++sy) {
							blk.bits[sy] |= bit;
						}
					}
				}
			}
			std::vector<proto::ColBlock> blocks;
			for (int by = 0; by < R; ++by) {
				for (int bz = 0; bz < R; ++bz) {
					for (int bx = 0; bx < R; ++bx) {
						auto&         blk = grid[bx + R * (bz + R * by)];
						std::uint64_t any = 0;
						for (auto b : blk.bits) {
							any |= b;
						}
						if (!any) {
							continue;
						}
						blk.x = a_job.rx * R + bx;
						blk.y = baseY + by;
						blk.z = a_job.rz * R + bz;
						blocks.push_back(blk);
					}
				}
			}
			proto::ColRegion header{};
			header.minX = a_job.rx * R;
			header.minY = baseY;
			header.minZ = a_job.rz * R;
			header.maxX = header.minX + R - 1;
			header.maxY = header.minY + R - 1;
			header.maxZ = header.minZ + R - 1;
			header.epoch = a_job.epoch;
			header.count = static_cast<std::uint32_t>(blocks.size());
			std::vector<std::uint8_t> payload(sizeof(header) + blocks.size() * sizeof(proto::ColBlock));
			std::memcpy(payload.data(), &header, sizeof(header));
			if (!blocks.empty()) {
				std::memcpy(payload.data() + sizeof(header), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
			}
			Send(payload, proto::kColRegion, a_job.epoch);
			counters.blocks.fetch_add(header.count, std::memory_order_relaxed);
			counters.regions.fetch_add(1, std::memory_order_relaxed);
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
		LC_LOG("collision v1 (ground heightfield) started: %zu regions around the player, probe from %s", offsets.size(),
			Config::Get().probeFrom == Config::ProbeFrom::kFeet ? "the feet" : "the top (1000 m)");
	}

	void Collision::Reset(std::uint32_t a_epoch)
	{
		Start();
		currentEpoch = a_epoch;
		harvested.clear();
		columns.clear();
		std::lock_guard lock(mutex);
		queue.clear();
		Job job{};
		job.clear = true;
		job.epoch = a_epoch;
		queue.push_back(job);
		cv.notify_one();
	}

	bool Collision::HarvestColumn(int a_rx, int a_rz, float a_feetGtaZ, ColumnHeights& a_out)
	{
		// TODO(v2): real geometry instead of the ground height (see Collision.h).
		const float startZ = Config::Get().probeFrom == Config::ProbeFrom::kFeet ? a_feetGtaZ + Config::Get().probeHeight : kTopProbeZ;
		int         missing = 0;
		bool        miss[kCorners * kCorners]{};
		for (int j = 0; j < kCorners; ++j) {
			for (int i = 0; i < kCorners; ++i) {
				const GtaVec g = McToGta(double(a_rx * kRegionSize + i), 0.0, double(a_rz * kRegionSize + j));
				float        z = 0.0f;
				::Scripting::GET_GROUND_Z_FOR_3D_COORD(static_cast<float>(g.x), static_cast<float>(g.y), startZ, &z);
				// 0 exactly: nothing found (collision not streamed in). Liberty City's ground is never
				// exactly at sea level, so this costs at most a corner.
				if (z == 0.0f || !std::isfinite(z) || z > startZ) {
					miss[j * kCorners + i] = true;
					++missing;
				}
				a_out.h[j * kCorners + i] = z;  // GTA z == MC y
			}
		}
		if (missing * 2 > kCorners * kCorners) {
			return false;
		}
		if (missing) {
			// Fill the odd hole with the lowest found height (never invents a wall).
			float lowest = 1e9f;
			for (int k = 0; k < kCorners * kCorners; ++k) {
				if (!miss[k]) {
					lowest = std::min(lowest, a_out.h[k]);
				}
			}
			for (int k = 0; k < kCorners * kCorners; ++k) {
				if (miss[k]) {
					a_out.h[k] = lowest;
				}
			}
		}
		return true;
	}

	void Collision::Update(const McVec& a_centerMc, float a_feetGtaZ)
	{
		Start();
		const int  prx = static_cast<int>(std::floor(a_centerMc.x / kRegionSize));
		const int  pry = static_cast<int>(std::floor(a_centerMc.y / kRegionSize));
		const int  prz = static_cast<int>(std::floor(a_centerMc.z / kRegionSize));
		const auto start = Clock::now();
		const auto epoch = currentEpoch.load();
		int        probes = 0, jobs = 0;

		for (const auto& o : offsets) {
			const int  rx = prx + o[0], ry = pry + o[1], rz = prz + o[2];
			const auto key = Key3(rx, ry, rz);
			const auto it = harvested.find(key);
			const bool isNear = std::abs(o[0]) <= 1 && std::abs(o[2]) <= 1 && o[1] >= -1 && o[1] <= 0;
			if (it != harvested.end() && !(isNear && start - it->second > kRefreshNear)) {
				continue;
			}
			auto&      col = columns[Key2(rx, rz)];
			const bool nearColumn = std::abs(o[0]) <= 1 && std::abs(o[2]) <= 1;
			const bool stale = !col.valid || (isNear && start - col.probed > kRefreshNear);
			if (stale) {
				if (start < col.retryAt) {
					continue;
				}
				// Far columns: ask the streamer for their collision first, probe a little later.
				if (!nearColumn && !col.requestedOnce) {
					const GtaVec c = McToGta(double(rx * kRegionSize + kRegionSize / 2), 0.0, double(rz * kRegionSize + kRegionSize / 2));
					::Scripting::REQUEST_COLLISION_AT_POSN(static_cast<float>(c.x), static_cast<float>(c.y), a_feetGtaZ);
					col.requestedOnce = true;
					col.requested = start;
					counters.collisionRequests.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				if (!nearColumn && start - col.requested < kRequestLead) {
					continue;
				}
				if (probes >= kMaxColumnsPerFrame || Clock::now() - start > kFrameBudget) {
					break;
				}
				++probes;
				counters.columns.fetch_add(1, std::memory_order_relaxed);
				ColumnHeights heights{};
				if (!HarvestColumn(rx, rz, a_feetGtaZ, heights)) {
					counters.columnFailures.fetch_add(1, std::memory_order_relaxed);
					col.retryAt = start + kRetryMissing;
					col.requestedOnce = false;  // ask again
					continue;
				}
				col.heights = heights;
				col.valid = true;
				col.probed = start;
			}
			Job job{};
			job.rx = rx;
			job.ry = ry;
			job.rz = rz;
			job.epoch = epoch;
			job.heights = col.heights;
			{
				std::lock_guard lock(mutex);
				queue.push_back(job);
			}
			cv.notify_one();
			harvested[key] = start;
			if (++jobs >= kMaxRegionsPerFrame) {
				break;
			}
		}

		// Bound memory: drop bookkeeping for far-away regions now and then.
		if (harvested.size() > offsets.size() * 4) {
			harvested.clear();
		}
		if (columns.size() > 4 * (2 * kRadius + 1) * (2 * kRadius + 1)) {
			columns.clear();
		}
	}

	void Collision::WorkerLoop()
	{
		for (;;) {
			Job job;
			{
				std::unique_lock lock(mutex);
				cv.wait(lock, [] { return !queue.empty(); });
				job = queue.front();
				queue.pop_front();
			}
			if (job.clear) {
				std::vector<std::uint8_t> payload(4);
				std::memcpy(payload.data(), &job.epoch, 4);
				Send(payload, proto::kColClear, job.epoch);
				counters.clears.fetch_add(1, std::memory_order_relaxed);
				LC_LOG("kColClear epoch %u sent", job.epoch);
			} else if (job.epoch == currentEpoch.load()) {
				SendTriangles(job);  // triangles first, like SkyCraft (kColTris precedes kColRegion)
				Voxelize(job);
			}
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
