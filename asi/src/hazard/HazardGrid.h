// Minecraft's burning blocks for GTA IV's peds (port of SkyCraft's BlockLights hazards): fire,
// soul fire and lit campfires burn what stands in them, lava burns hard, magma scorches feet.
// Minecraft sends every light-emitting block of a section (kRenLights) with its BlockHazard in
// RenLight::color bits 28-31; every hazard block emits light (a campfire only while lit, and an
// unlit one doesn't burn), so the light list is the complete hazard list. Water and lava come as
// kRenLiquids (kind and surface height per block): vehicles struggle in them (LiquidGrid, Drag).
// Pure and SDK-free: tests/hazard_test.cpp. Hazards.cpp feeds it and acts on its verdicts.
#pragma once

#include "libertycraft_protocol.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lc::hazard
{
	namespace proto = ::libertycraft::proto;

	inline std::uint8_t HazardOf(std::uint32_t a_color) { return static_cast<std::uint8_t>((a_color >> 28) & 0xF); }

	// Worse first: lava, then fire, then magma.
	inline int Severity(std::uint8_t a_h)
	{
		return a_h == proto::kHazardLava ? 3 : a_h == proto::kHazardFire ? 2 : a_h == proto::kHazardMagma ? 1 : 0;
	}

	inline std::uint64_t BlockKey(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
	{
		return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x) & 0x1FFFFF) << 42) |
		       (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_y) & 0x1FFFFF) << 21) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_z) & 0x1FFFFF));
	}

	// The hazard blocks Minecraft has sent, by block (Minecraft block coordinates).
	class Grid
	{
	public:
		// A section's light list replaces what the section had (count 0: none left).
		void SetSection(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, const proto::RenLight* a_lights, std::uint32_t a_count)
		{
			const std::uint64_t sk = BlockKey(a_sx, a_sy, a_sz);
			if (auto it = sections_.find(sk); it != sections_.end()) {
				for (const auto k : it->second) {
					cells_.erase(k);
				}
				sections_.erase(it);
			}
			std::vector<std::uint64_t> keys;
			for (std::uint32_t i = 0; i < a_count; ++i) {
				const auto h = HazardOf(a_lights[i].color);
				if (h == proto::kHazardNone || h > proto::kHazardMagma || a_lights[i].x > 15 || a_lights[i].y > 15 || a_lights[i].z > 15) {
					continue;
				}
				const auto k = BlockKey(a_sx * 16 + a_lights[i].x, a_sy * 16 + a_lights[i].y, a_sz * 16 + a_lights[i].z);
				cells_[k] = h;
				keys.push_back(k);
			}
			if (!keys.empty()) {
				sections_.emplace(sk, std::move(keys));
			}
		}

		void Clear()
		{
			cells_.clear();
			sections_.clear();
		}

		// A single block outside any section's list (test hooks); kHazardNone removes it.
		void SetCell(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, std::uint8_t a_hazard)
		{
			if (a_hazard == proto::kHazardNone) {
				cells_.erase(BlockKey(a_x, a_y, a_z));
			} else {
				cells_[BlockKey(a_x, a_y, a_z)] = a_hazard;
			}
		}

		// The blocks' extent (diagnostics). False: none.
		bool Bounds(std::int32_t (&a_lo)[3], std::int32_t (&a_hi)[3]) const
		{
			if (cells_.empty()) {
				return false;
			}
			for (int k = 0; k < 3; ++k) {
				a_lo[k] = INT32_MAX;
				a_hi[k] = INT32_MIN;
			}
			for (const auto& [key, h] : cells_) {
				const std::int32_t c[3] = { Unpack(key >> 42), Unpack(key >> 21), Unpack(key) };
				for (int k = 0; k < 3; ++k) {
					a_lo[k] = std::min(a_lo[k], c[k]);
					a_hi[k] = std::max(a_hi[k], c[k]);
				}
			}
			return true;
		}

		// How many blocks of each hazard (index: BlockHazard).
		void Count(std::size_t (&a_out)[4]) const
		{
			a_out[0] = a_out[1] = a_out[2] = a_out[3] = 0;
			for (const auto& [k, h] : cells_) {
				++a_out[h & 3];
			}
		}

		bool          Empty() const { return cells_.empty(); }
		bool          HasSection(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz) const { return sections_.count(BlockKey(a_sx, a_sy, a_sz)) != 0; }
		std::size_t   Cells() const { return cells_.size(); }
		std::size_t   Sections() const { return sections_.size(); }

		std::uint8_t At(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z) const
		{
			const auto it = cells_.find(BlockKey(a_x, a_y, a_z));
			return it == cells_.end() ? static_cast<std::uint8_t>(proto::kHazardNone) : it->second;
		}

		// The worst hazard a body touches: an upright box a_r around (a_x, a_z), from a_feetY up a_h
		// (Minecraft coordinates, blocks), fire and lava anywhere in it; magma only in the block its
		// feet stand on.
		std::uint8_t Touching(double a_x, double a_feetY, double a_z, double a_r, double a_h) const
		{
			if (cells_.empty()) {
				return proto::kHazardNone;
			}
			const int x0 = static_cast<int>(std::floor(a_x - a_r)), x1 = static_cast<int>(std::floor(a_x + a_r));
			const int z0 = static_cast<int>(std::floor(a_z - a_r)), z1 = static_cast<int>(std::floor(a_z + a_r));
			const int y0 = static_cast<int>(std::floor(a_feetY)), y1 = static_cast<int>(std::floor(a_feetY + a_h));
			std::uint8_t worst = proto::kHazardNone;
			for (int x = x0; x <= x1; ++x) {
				for (int z = z0; z <= z1; ++z) {
					for (int y = y0; y <= y1; ++y) {
						const auto h = At(x, y, z);
						if ((h == proto::kHazardFire || h == proto::kHazardLava) && Severity(h) > Severity(worst)) {
							worst = h;
						}
					}
					// Standing on magma: the block just under the feet (or the one they sink into).
					if (worst == proto::kHazardNone &&
						(At(x, static_cast<int>(std::floor(a_feetY - 0.1)), z) == proto::kHazardMagma || At(x, y0, z) == proto::kHazardMagma)) {
						worst = proto::kHazardMagma;
					}
				}
			}
			return worst;
		}

	private:
		static std::int32_t Unpack(std::uint64_t a_bits)
		{
			const auto v = static_cast<std::int32_t>(a_bits & 0x1FFFFF);
			return v & 0x100000 ? v - 0x200000 : v;
		}

		std::unordered_map<std::uint64_t, std::uint8_t>               cells_;
		std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> sections_;
	};

	// ---- a ped in Minecraft's fire ----------------------------------------------------------------
	struct Tuning
	{
		float fireAfterburn = 4.0f;    // seconds a ped keeps burning after leaving fire
		float lavaAfterburn = 8.0f;    // ... lava
		float firePerSecond = 2.0f;    // Minecraft damage per second standing in fire (1 a hit, 2 hits a second)
		float lavaPerSecond = 8.0f;    // ... in lava (4 a hit)
		float magmaPerSecond = 1.0f;   // ... on magma
		float damageScale = 10.0f;     // Minecraft damage x this = GTA health (PedDamageScale)
		float igniteRetry = 1.0f;      // seconds between attempts to set a ped alight that isn't burning
	};

	struct Burn
	{
		float afterburn = 0.0f;  // seconds of burning left once out of the fire
		float retry = 0.0f;      // seconds until the next ignite attempt
		float owed = 0.0f;       // GTA health not taken yet (damage goes in whole points)
		bool  touched = false;   // has been in fire or lava (else only magma)
	};

	struct Action
	{
		bool     ignite = false;      // set the ped alight (it isn't burning now)
		bool     extinguish = false;  // put it out: the afterburn is over
		unsigned damage = 0;          // GTA health to take off now
		bool     done = false;        // forget the ped
	};

	// One step for a ped: a_hazard what it touches now, a_burning whether GTA has it alight.
	inline Action Step(Burn& a_b, std::uint8_t a_hazard, bool a_burning, float a_dt, const Tuning& a_t)
	{
		Action a;
		a_b.retry = std::max(0.0f, a_b.retry - a_dt);
		float perSecond = 0.0f;
		if (a_hazard == proto::kHazardFire || a_hazard == proto::kHazardLava) {
			const bool lava = a_hazard == proto::kHazardLava;
			a_b.afterburn = std::max(a_b.afterburn, lava ? a_t.lavaAfterburn : a_t.fireAfterburn);
			a_b.touched = true;
			perSecond = lava ? a_t.lavaPerSecond : a_t.firePerSecond;
			if (!a_burning && a_b.retry <= 0.0f) {
				a.ignite = true;
				a_b.retry = a_t.igniteRetry;
			}
		} else {
			a_b.afterburn = std::max(0.0f, a_b.afterburn - a_dt);
			if (a_hazard == proto::kHazardMagma) {
				perSecond = a_t.magmaPerSecond;
			}
			if (a_b.afterburn <= 0.0f) {
				a.extinguish = a_burning && a_b.touched;
				a.done = a_hazard == proto::kHazardNone;
			}
		}
		a_b.owed += perSecond * a_t.damageScale * a_dt;
		if (a_b.owed >= 1.0f) {
			a.damage = static_cast<unsigned>(a_b.owed);
			a_b.owed -= static_cast<float>(a.damage);
		}
		return a;
	}
	// ---- water and lava (kRenLiquids) ----------------------------------------------------------------
	class LiquidGrid
	{
	public:
		void SetSection(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, const proto::RenLiquid* a_liquids, std::uint32_t a_count)
		{
			const std::uint64_t sk = BlockKey(a_sx, a_sy, a_sz);
			if (auto it = sections_.find(sk); it != sections_.end()) {
				for (const auto k : it->second) {
					cells_.erase(k);
				}
				sections_.erase(it);
			}
			std::vector<std::uint64_t> keys;
			for (std::uint32_t i = 0; i < a_count; ++i) {
				const auto& l = a_liquids[i];
				const std::uint8_t kind = (l.info >> 4) & 3, surface = l.info & 15;
				if (kind == proto::kLiquidNone || kind > proto::kLiquidLava || surface == 0 || l.x > 15 || l.y > 15 || l.z > 15) {
					continue;
				}
				const auto k = BlockKey(a_sx * 16 + l.x, a_sy * 16 + l.y, a_sz * 16 + l.z);
				cells_[k] = static_cast<std::uint8_t>(surface | kind << 4);
				keys.push_back(k);
			}
			if (!keys.empty()) {
				sections_.emplace(sk, std::move(keys));
			}
		}

		void Clear()
		{
			cells_.clear();
			sections_.clear();
		}

		bool        Empty() const { return cells_.empty(); }
		std::size_t Cells() const { return cells_.size(); }
		bool        HasSection(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz) const { return sections_.count(BlockKey(a_sx, a_sy, a_sz)) != 0; }

		// Every cell: a_f(x, y, z, info).
		template <typename F>
		void ForEach(F&& a_f) const
		{
			for (const auto& [key, info] : cells_) {
				a_f(Unpack(key >> 42), Unpack(key >> 21), Unpack(key), info);
			}
		}

		// The cell's info (kind << 4 | surface in fifteenths), 0: dry.
		std::uint8_t At(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z) const
		{
			const auto it = cells_.find(BlockKey(a_x, a_y, a_z));
			return it == cells_.end() ? std::uint8_t(0) : it->second;
		}

		// How deep the liquid is over a point of a vehicle's underside at height a_bottom (blocks), and
		// which: the liquid run that reaches down to within half a block of it, its surface minus
		// a_bottom. 0: dry there. Minecraft draws a liquid in the block that holds GTA's ground from
		// that ground up (WorldExporter: surface = ground + height x (1 - ground's share of the
		// block)), and the underside rests on that ground: the same here.
		float DepthAt(double a_x, double a_bottom, double a_z, std::uint8_t& a_kind) const
		{
			a_kind = proto::kLiquidNone;
			const int x = static_cast<int>(std::floor(a_x)), z = static_cast<int>(std::floor(a_z));
			const int y0 = static_cast<int>(std::floor(a_bottom - 0.5));
			int       y = y0;
			while (y <= y0 + 1 && !At(x, y, z)) {
				++y;  // the run must start at (or just above) the underside
			}
			if (y > y0 + 1) {
				return 0.0f;
			}
			std::uint8_t info = At(x, y, z), top = info;
			int          topY = y;
			for (int k = 0; k < 6 && At(x, topY + 1, z); ++k) {
				++topY;
				top = At(x, topY, z);
			}
			a_kind = (info >> 4) & 3;
			if (((top >> 4) & 3) == proto::kLiquidLava) {
				a_kind = proto::kLiquidLava;
			}
			double surface = topY + (top & 15) / 15.0;
			if (topY == y) {
				const double g = std::clamp(a_bottom - y, 0.0, 1.0);  // the ground's share of the block
				surface = y + g + (top & 15) / 15.0 * (1.0 - g);
			}
			if (surface <= a_bottom) {
				a_kind = proto::kLiquidNone;
				return 0.0f;
			}
			return static_cast<float>(surface - a_bottom);
		}

	private:
		static std::int32_t Unpack(std::uint64_t a_bits)
		{
			const auto v = static_cast<std::int32_t>(a_bits & 0x1FFFFF);
			return v & 0x100000 ? v - 0x200000 : v;
		}

		std::unordered_map<std::uint64_t, std::uint8_t>               cells_;
		std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> sections_;
	};

	// ---- Minecraft water as GTA's water (Hazards.cpp hooks GTA's water level query) --------------------
	// Per block column (Minecraft x, z), its runs of water: bottom and surface (Minecraft y = GTA z).
	struct WaterRun
	{
		float bottom = 0.0f, surface = 0.0f;
	};

	inline std::uint64_t ColumnKey(std::int32_t a_x, std::int32_t a_z) { return BlockKey(a_x, 0, a_z); }

	class WaterColumns
	{
	public:
		void Build(const LiquidGrid& a_liquids)
		{
			std::unordered_map<std::uint64_t, std::vector<std::pair<std::int32_t, std::uint8_t>>> cells;
			a_liquids.ForEach([&](std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, std::uint8_t a_info) {
				if (((a_info >> 4) & 3) == proto::kLiquidWater) {
					cells[ColumnKey(a_x, a_z)].emplace_back(a_y, a_info);
				}
			});
			runs_.clear();
			for (auto& [key, list] : cells) {
				std::sort(list.begin(), list.end());
				auto& out = runs_[key];
				for (std::size_t i = 0; i < list.size();) {
					std::size_t j = i;
					while (j + 1 < list.size() && list[j + 1].first == list[j].first + 1) {
						++j;
					}
					out.push_back({ static_cast<float>(list[i].first), static_cast<float>(list[j].first) + (list[j].second & 15) / 15.0f });
					i = j + 1;
				}
			}
		}

		bool Empty() const { return runs_.empty(); }

		// The water surface (GTA z) over GTA point (a_x, a_y, a_z): the run of its column the point is in
		// or near (from 2 m under its bottom to 3 m over its surface), the nearest one. False: none.
		bool Surface(float a_x, float a_y, float a_z, float& a_out) const
		{
			const auto it = runs_.find(ColumnKey(static_cast<std::int32_t>(std::floor(a_x)), static_cast<std::int32_t>(std::floor(-a_y))));
			if (it == runs_.end()) {
				return false;
			}
			float best = 1e9f;
			for (const auto& r : it->second) {
				if (a_z < r.bottom - 2.0f || a_z > r.surface + 3.0f) {
					continue;
				}
				const float d = a_z < r.bottom ? r.bottom - a_z : a_z > r.surface ? a_z - r.surface : 0.0f;
				if (d < best) {
					best = d;
					a_out = r.surface;
				}
			}
			return best < 1e8f;
		}

	private:
		std::unordered_map<std::uint64_t, std::vector<WaterRun>> runs_;
	};

	// ---- a vehicle in Minecraft's blocks -------------------------------------------------------------
	// Its model box (vehicle frame: x right, y forward, z up, metres) placed by its matrix rows in GTA
	// world axes.
	struct Footprint
	{
		double pos[3]{};
		float  right[3]{ 1, 0, 0 }, fwd[3]{ 0, 1, 0 }, up[3]{ 0, 0, 1 };
		float  lo[3]{ -1, -2, -0.5f }, hi[3]{ 1, 2, 1 };
		// GTA's ground under it (GTA z), where its wheels stand: liquid depth counts from there (the
		// model box's bottom is its body, some way above the road). NaN: unknown, the box bottom.
		double ground = std::nan("");
	};

	struct Contact
	{
		std::uint8_t hazard = proto::kHazardNone;  // the worst fire / lava / magma its underside touches
		std::uint8_t liquid = proto::kLiquidNone;  // lava if any of it is in lava, else water if wet
		float        depth = 0.0f;                 // liquid over its underside, averaged over the footprint (m)
		float        depthMax = 0.0f;
		float        wet = 0.0f;                   // the share of the footprint in liquid
	};

	// Samples a 3 x 4 grid over the underside (wheels and floor), from its bottom up 0.6 m for fire.
	inline Contact Touch(const Grid& a_hazards, const LiquidGrid& a_liquids, const Footprint& a_f)
	{
		Contact c;
		int     samples = 0, wetSamples = 0;
		float   depthSum = 0.0f;
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 4; ++j) {
				const float lx = a_f.lo[0] + (a_f.hi[0] - a_f.lo[0]) * (0.1f + 0.4f * i);
				const float ly = a_f.lo[1] + (a_f.hi[1] - a_f.lo[1]) * (0.08f + 0.28f * j);
				const float lz = a_f.lo[2];
				const double gx = a_f.pos[0] + a_f.right[0] * lx + a_f.fwd[0] * ly + a_f.up[0] * lz;
				const double gy = a_f.pos[1] + a_f.right[1] * lx + a_f.fwd[1] * ly + a_f.up[1] * lz;
				const double gz = a_f.pos[2] + a_f.right[2] * lx + a_f.fwd[2] * ly + a_f.up[2] * lz;
				// Minecraft axes: x, y = GTA z, z = -GTA y.
				const double mx = gx, my = gz, mz = -gy;
				++samples;
				if (!a_hazards.Empty()) {
					const auto h = a_hazards.Touching(mx, my, mz, 0.05, 0.6);
					if (Severity(h) > Severity(c.hazard)) {
						c.hazard = h;
					}
				}
				if (!a_liquids.Empty()) {
					std::uint8_t kind = proto::kLiquidNone;
					const double wheels = std::isnan(a_f.ground) ? my : std::min(my, a_f.ground);
					const float  d = a_liquids.DepthAt(mx, wheels, mz, kind);
					if (d > 0.0f) {
						++wetSamples;
						depthSum += d;
						c.depthMax = std::max(c.depthMax, d);
						if (kind == proto::kLiquidLava || c.liquid == proto::kLiquidNone) {
							c.liquid = kind;
						}
					}
				}
			}
		}
		c.depth = samples ? depthSum / static_cast<float>(samples) : 0.0f;
		c.wet = samples ? static_cast<float>(wetSamples) / static_cast<float>(samples) : 0.0f;
		return c;
	}

	// How a vehicle's horizontal speed fares in a liquid this frame. a_last: the speed it was left with
	// last frame (negative: none); what it gained since is the engine's push, of which only part
	// takes in the liquid (sluggish); then drag, and a top speed it's eased down to (no hard clamp:
	// nothing jerks against the throttle). Deeper is worse; lava is thicker than water.
	struct DragTuning
	{
		float waterTop = 20.0f, waterTopFall = 2.9f, waterTopMin = 1.5f;  // top speed: waterTop * exp(-fall * depth), at least min (m/s)
		float lavaTop = 14.0f, lavaTopFall = 3.5f, lavaTopMin = 1.0f;
		float waterKeep = 4.0f, lavaKeep = 8.0f;                          // share of the engine's push kept: 1 / (1 + keep * depth)
		float waterDrag = 0.2f, waterDragDepth = 1.0f;                    // drag per second: drag + dragDepth * depth
		float lavaDrag = 0.4f, lavaDragDepth = 2.0f;
		float ease = 6.0f;                                                // per second, down to the top speed
	};

	// A ped wading in water this deep (m over its feet) moves at this share of its speed: ankle deep
	// barely slower, waist deep half, deeper a slow struggle (GTA's own peds only swim in GTA's water).
	inline float WadeSpeed(float a_depth)
	{
		if (a_depth <= 0.15f) {
			return 1.0f;
		}
		return std::clamp(1.0f - 0.55f * (a_depth - 0.15f) / 0.85f, 0.35f, 1.0f);
	}

	inline float TopSpeed(float a_depth, std::uint8_t a_kind, const DragTuning& a_t = {})
	{
		return a_kind == proto::kLiquidLava ? std::max(a_t.lavaTopMin, a_t.lavaTop * std::exp(-a_t.lavaTopFall * a_depth))
		                                    : std::max(a_t.waterTopMin, a_t.waterTop * std::exp(-a_t.waterTopFall * a_depth));
	}

	inline float Drag(float a_speed, float a_last, float a_depth, std::uint8_t a_kind, float a_dt, const DragTuning& a_t = {})
	{
		if (a_depth <= 0.0f || a_kind == proto::kLiquidNone) {
			return a_speed;
		}
		const bool lava = a_kind == proto::kLiquidLava;
		float      s = a_speed;
		if (a_last >= 0.0f && s > a_last) {
			s = a_last + (s - a_last) / (1.0f + (lava ? a_t.lavaKeep : a_t.waterKeep) * a_depth);
		}
		s *= std::exp(-(lava ? a_t.lavaDrag + a_t.lavaDragDepth * a_depth : a_t.waterDrag + a_t.waterDragDepth * a_depth) * a_dt);
		const float top = TopSpeed(a_depth, a_kind, a_t);
		if (s > top) {
			s = top + (s - top) * std::exp(-a_t.ease * a_dt);
		}
		return s;
	}

	// A vehicle's engine in deep water, as in GTA IV's own water: it runs on for a while once the water
	// is over the engine (deeper than a_t.depth), then dies, and stays off while it's that deep. Out of
	// the deep water the clock starts over, and a stalled engine starts again (GTA's own drowned car
	// stays dead). GTA's own timing, measured (Hazards.cpp DebugHarbour, an Admiral, accelerator held):
	// dropped into the sea, its engine died 2.4 s after it hit the water; driven off a beach at 12 m/s,
	// 3.2 s after its wheels touched the water (2.7 s after its middle went under). A car rolls into
	// Minecraft water on GTA's ground (no drop: the water is over it as it comes in), so 3 s.
	struct StallTuning
	{
		float depth = 1.0f;  // m of water over GTA's ground under the vehicle
		float after = 3.0f;  // s of it, without a break, before the engine dies
	};
	struct Stall
	{
		float under = 0.0f;  // s in deep water, without a break
		bool  off = false;   // stalled (held off)
	};
	enum class StallEvent : std::uint8_t
	{
		kNone,
		kDeep,     // just went under (the engine runs on)
		kStalls,   // the engine dies now
		kOff,      // stalled: hold it off
		kRestarts  // out of the deep water after a stall: start it again
	};
	inline StallEvent StallStep(Stall& a_s, bool a_water, float a_depth, float a_dt, const StallTuning& a_t = {})
	{
		if (a_water && a_depth > a_t.depth) {
			const bool first = a_s.under == 0.0f;
			a_s.under += a_dt;
			if (a_s.off) {
				return StallEvent::kOff;
			}
			if (a_s.under >= a_t.after) {
				a_s.off = true;
				return StallEvent::kStalls;
			}
			return first ? StallEvent::kDeep : StallEvent::kNone;
		}
		a_s.under = 0.0f;
		if (a_s.off) {
			a_s.off = false;
			return StallEvent::kRestarts;
		}
		return StallEvent::kNone;
	}
}
