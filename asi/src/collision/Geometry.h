// Collision v2: GTA IV's static collision, sampled with line probes, as Minecraft collision.
// Header-only and SDK-free (the probes come in through a callback), unit-tested on Linux by
// asi/tests/collision_test.cpp against a synthetic one-sided ray caster.
//
// Facts this rests on (measured in game, 1.0.8.0, see Collision.cpp):
//  * CWorld::ProcessLineOfSight is ONE-SIDED: it only reports faces that face the ray. A probe
//    going down sees the tops of things, one going up sees undersides, a horizontal one sees the
//    wall faces turned towards it. Walls are closed (both faces), ground is a single sheet.
//  * ~4-5 us a probe, game thread only.
//
// Per 8x8-block column of regions (MC space, x east, y up, z south):
//  1. Every half block (17x17 samples, region borders included) a vertical chain down from the
//     top of the probed span and one up from its bottom, each restarted just past every hit:
//     the sample's events, sorted top-down, each a top (seen from above) or an underside.
//  2. Between consecutive events: under a top is solid down to the next underside (a slab, or
//     the ground: solid to the end of the span); between an underside and the top below is free
//     (a room, the street under a bridge). Two tops in a row (a roof, then the ground under it:
//     a building closed everywhere but underneath; or an awning over the street) count as FREE:
//     walls come from the horizontal probes, and an awning must never become a pillar.
//  3. For every sample edge and every floor (top) of the near sample, a horizontal probe at body
//     height (or inside the solid the far sample has there) finds the wall face between them;
//     a hit gets a second probe at head height to tell a railing from a wall.
//  4. Floors (and ceilings) become triangles between neighbouring samples whose heights belong
//     to one surface (within a stair rise, or on one sloped plane); a floor is extended under a
//     wall (the corner beyond it is solid at that height) so it reaches the wall. Walls become
//     vertical quads at the probe hits. Voxels (1/8 block, for mobs and items) are the walls' and
//     ceilings' shells plus fill under floors (2 blocks into the ground, the slab's thickness, or
//     one voxel under a sheet such as an awning).
#pragma once

#include "libertycraft_protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace lc::col
{
	namespace proto = ::libertycraft::proto;

	inline constexpr int   kRegion = 8;                           // blocks per region edge (must match Java)
	inline constexpr int   kPerBlock = 2;                         // samples per block
	inline constexpr float kSpacing = 1.0f / float(kPerBlock);    // 0.5 block between samples
	inline constexpr int   kGrid = kRegion * kPerBlock + 1;       // 17 sample lines per column edge
	inline constexpr int   kSamples = kGrid * kGrid;
	inline constexpr float kInf = std::numeric_limits<float>::infinity();

	// ---- tuning ---------------------------------------------------------------------------------
	inline constexpr float kChainStep = 0.05f;      // restart a vertical chain this far past a hit
	inline constexpr int   kMaxChain = 12;          // hits per chain
	inline constexpr float kMaxRise = 0.5f;         // per half block: floors this close are one surface (stairs, curbs)
	inline constexpr float kPlaneTol = 0.1f;        // ...or this close to each other's plane (steep slopes)
	inline constexpr float kStep = 0.6f;            // Minecraft's step height: solid lower than this is a step, not a wall
	inline constexpr float kWalkableNy = 0.7f;      // as HostTri.WALKABLE_NY
	inline constexpr float kMinHeadroom = 0.4f;     // floors with less free space above get no wall probes
	inline constexpr float kRayHeight = 1.0f;       // wall probe this far above a floor
	inline constexpr float kHeadHeight = 1.9f;      // second probe: wall or railing?
	inline constexpr float kRailTop = 1.5f;         // a railing (head probe missed) is this tall
	inline constexpr float kWallCap = 3.0f;         // a wall whose top is unknown is this tall (unjumpable)
	inline constexpr float kWallMaxReach = 40.0f;   // a wall reaches the far sample's next top this far up at most
	inline constexpr float kEvidenceTop = 3.0f;     // look for solid in the far sample this far above a floor
	inline constexpr float kFillDepth = 2.0f;       // voxel fill under the ground / thick slabs
	inline constexpr float kSheet = 0.125f;         // voxel fill under a sheet (a top with free space under it)
	inline constexpr float kTerrainThick = 2.0f;    // floors over at least this much solid are terrain (diggable)
	inline constexpr float kSpanMargin = 1.0f;      // floors this close to the span's ends get no wall probes
	inline constexpr float kHollowMin = 0.5f;       // two tops closer than this: one solid, not a sheet over free space

	// Probe directions in the sample grid: +x, -x, +z, -z.
	inline constexpr int kDirI[4] = { 1, -1, 0, 0 };
	inline constexpr int kDirJ[4] = { 0, 0, 1, -1 };

	// GTA IV's material of a surface (its materials.dat index), or this when not known.
	inline constexpr std::uint8_t kNoMaterial = 0xFF;

	struct Hit
	{
		float        pos[3];
		float        n[3];                // unit, facing the ray
		std::uint8_t mat = kNoMaterial;  // the hit surface's material (Rays.h), for the blocky city
	};

	struct Event
	{
		float        y;
		float        n[3];
		bool         top;  // seen from above (a floor); else an underside (a ceiling)
		std::uint8_t mat = kNoMaterial;
	};

	// ColTri flags for a triangle of this material (kTriGtaMaterial, bits 16-23), 0 if not known.
	inline std::uint32_t MaterialFlags(std::uint8_t a_mat)
	{
		return a_mat == kNoMaterial ? 0u : proto::kTriGtaMaterial | (std::uint32_t(a_mat) << proto::kTriGtaMaterialShift);
	}

	// Top-down events of one sample: is the gap between a_upper (null: above them all) and
	// a_lower (null: below them all) solid? See the header comment, step 2.
	inline bool GapSolid(const Event* a_upper, const Event* a_lower)
	{
		if (!a_upper) {
			return a_lower && !a_lower->top;  // above an underside: inside whatever it is the bottom of
		}
		if (!a_lower) {
			return a_upper->top;  // below the lowest top: the ground
		}
		if (a_upper->top != a_lower->top) {
			return a_upper->top;  // a slab (top over underside), or a room (underside over top)
		}
		// Two tops (or undersides) in a row: free inside, unless too thin to be anything but
		// stacked faces of one solid (overlapping meshes, steps).
		return a_upper->y - a_lower->y < kHollowMin;
	}

	enum WallKind : std::uint8_t
	{
		kWallProbe = 0,     // body-height probe, no solid seen in the far sample
		kWallEvidence = 1,  // the far sample is solid above the step height: there is a wall/ledge
		kWallFollow = 2,    // head-height probe after a kWallProbe hit (result goes to the parent)
	};

	struct WallQuery
	{
		std::uint16_t sample = 0;  // near sample (free side)
		std::uint8_t  dir = 0;     // kDirI/kDirJ index
		std::uint8_t  kind = kWallProbe;
		float         floorY = 0.0f, ceilY = 0.0f;  // the near sample's floor and the top of its free space
		float         y = 0.0f;                     // probe height
		float         oLo = 0.0f, oHi = 0.0f;       // kWallEvidence: the far sample's solid within the free space
		float         topGuess = 0.0f;              // kWallProbe: how high the wall goes if the head probe hits
		std::int32_t  parent = -1;                  // kWallFollow
		std::uint8_t  mat = kNoMaterial;            // the wall's material where the probe hit it
		// results
		bool         done = false;
		bool         hit = false;  // a steep face was hit
		float        pos[3]{}, n[3]{};
		std::int8_t  follow = -1;  // kWallProbe: -1 no head probe, 0 missed, 1 hit
		float        followY = 0.0f;
	};

	struct Column
	{
		int                                  rx = 0, rz = 0;
		float                                yLo = 0.0f, yHi = 0.0f;  // probed span (MC y)
		std::vector<Event>                   events;                  // every sample's, top-down
		std::array<std::uint32_t, kSamples + 1> first{};               // sample s: events[first[s], first[s + 1])
		std::vector<WallQuery>               walls;
		std::uint32_t                        emptySamples = 0;        // samples without a single event

		static int Index(int a_i, int a_j) { return a_j * kGrid + a_i; }
		float      X(int a_i) const { return float(rx * kRegion) + float(a_i) * kSpacing; }
		float      Z(int a_j) const { return float(rz * kRegion) + float(a_j) * kSpacing; }
		int        Count(int a_s) const { return int(first[a_s + 1] - first[a_s]); }
		const Event* Ev(int a_s) const { return events.data() + first[a_s]; }

		bool SolidAt(int a_s, float a_y) const
		{
			const Event* e = Ev(a_s);
			const int    n = Count(a_s);
			int          k = 0;
			while (k < n && e[k].y > a_y) {
				++k;
			}
			return GapSolid(k > 0 ? &e[k - 1] : nullptr, k < n ? &e[k] : nullptr);
		}

		// FNV-1a over the events (heights to the centimetre) and wall hits: did a re-probe change anything?
		std::uint64_t Hash() const
		{
			std::uint64_t h = 1469598103934665603ull;
			auto mix = [&](std::int64_t a_v) {
				h ^= static_cast<std::uint64_t>(a_v);
				h *= 1099511628211ull;
			};
			for (int s = 0; s <= kSamples; ++s) {
				mix(first[s]);
			}
			for (const auto& e : events) {
				mix(std::llround(e.y * 100.0f) * 2 + (e.top ? 1 : 0));
			}
			for (const auto& w : walls) {
				mix(w.hit ? std::llround(w.pos[0] * 100.0f) ^ (std::llround(w.pos[2] * 100.0f) << 20) : -1);
				mix(w.follow);
			}
			return h;
		}
	};

	// A sample's floors (tops) or ceilings (undersides) with what's around them.
	struct Surf
	{
		float y;
		float n[3];
		float open;   // free space on the open side, up to the next event (kInf: none)
		float thick;  // solid on the other side (kInf: to the end of the span; 0: a sheet, free beyond)
		std::uint8_t mat = kNoMaterial;
	};

	inline void Surfaces(const Column& a_c, int a_s, bool a_floors, std::vector<Surf>& a_out)
	{
		a_out.clear();
		const Event* e = a_c.Ev(a_s);
		const int    n = a_c.Count(a_s);
		for (int k = 0; k < n; ++k) {
			if (e[k].top != a_floors) {
				continue;
			}
			// the free side (above a floor, below a ceiling) and the solid side
			const Event* openNext = a_floors ? (k > 0 ? &e[k - 1] : nullptr) : (k + 1 < n ? &e[k + 1] : nullptr);
			const bool   openSolid = a_floors ? GapSolid(openNext, &e[k]) : GapSolid(&e[k], openNext);
			if (openSolid) {
				continue;  // a face inside something (stacked faces)
			}
			Surf f{};
			f.y = e[k].y;
			std::memcpy(f.n, e[k].n, sizeof(f.n));
			f.mat = e[k].mat;
			f.open = openNext ? std::fabs(openNext->y - e[k].y) : kInf;
			// solid on the back side: through every solid gap in a row
			f.thick = 0.0f;
			const int step = a_floors ? 1 : -1;
			for (int m = k;; m += step) {
				const int    o = m + step;
				const Event* next = o >= 0 && o < n ? &e[o] : nullptr;
				if (!(a_floors ? GapSolid(&e[m], next) : GapSolid(next, &e[m]))) {
					break;
				}
				if (!next) {
					f.thick = kInf;
					break;
				}
				f.thick = std::fabs(e[k].y - next->y);
			}
			a_out.push_back(f);
		}
	}

	// The lowest solid stretch of sample a_s overlapping (a_lo, a_hi), clipped to [a_clipLo, a_clipHi].
	inline bool SolidSpan(const Column& a_c, int a_s, float a_lo, float a_hi, float a_clipLo, float a_clipHi, float& a_oLo, float& a_oHi)
	{
		if (a_hi <= a_lo) {
			return false;
		}
		const Event* e = a_c.Ev(a_s);
		const int    n = a_c.Count(a_s);
		bool         found = false;
		for (int k = 0; k <= n; ++k) {  // gap k: between e[k-1] (above) and e[k] (below)
			const Event* upper = k > 0 ? &e[k - 1] : nullptr;
			const Event* lower = k < n ? &e[k] : nullptr;
			const float  gHi = upper ? upper->y : kInf;
			const float  gLo = lower ? lower->y : -kInf;
			if (gHi <= a_lo || gLo >= a_hi || !GapSolid(upper, lower)) {
				continue;
			}
			// gaps go top-down: keep the last (lowest) one
			a_oLo = std::max(gLo, a_clipLo);
			a_oHi = std::min(gHi, a_clipHi);
			found = a_oHi > a_oLo;
		}
		return found;
	}

	// Step 3: the horizontal probes (results are filled in by ColumnProbe).
	inline void PlanWalls(Column& a_c)
	{
		a_c.walls.clear();
		std::vector<Surf> floors;
		for (int j = 0; j < kGrid; ++j) {
			for (int i = 0; i < kGrid; ++i) {
				const int s = Column::Index(i, j);
				Surfaces(a_c, s, true, floors);
				if (floors.empty()) {
					continue;
				}
				for (int d = 0; d < 4; ++d) {
					const int qi = i + kDirI[d], qj = j + kDirJ[d];
					// The sample lines on the column's border are probed by both neighbours: each then
					// has the walls' voxels on its own side of the border.
					if (qi < 0 || qj < 0 || qi >= kGrid || qj >= kGrid) {
						continue;
					}
					const int q = Column::Index(qi, qj);
					for (const auto& f : floors) {
						if (f.y < a_c.yLo + kSpanMargin || f.y > a_c.yHi - kSpanMargin - kHeadHeight || f.open < kMinHeadroom) {
							continue;
						}
						WallQuery w{};
						w.sample = static_cast<std::uint16_t>(s);
						w.dir = static_cast<std::uint8_t>(d);
						w.floorY = f.y;
						w.ceilY = f.y + f.open;
						float oLo = 0.0f, oHi = 0.0f;
						if (SolidSpan(a_c, q, f.y + kStep, std::min(w.ceilY, f.y + kEvidenceTop), f.y, w.ceilY, oLo, oHi)) {
							w.kind = kWallEvidence;
							w.oLo = oLo;
							w.oHi = oHi;
							const float lo = std::max(oLo, f.y + kStep) + 0.05f, hi = std::min(oHi, f.y + 1.6f) - 0.05f;
							w.y = lo <= hi ? 0.5f * (lo + hi) : 0.5f * (std::max(oLo, f.y + kStep) + std::min(oHi, w.ceilY));
						} else {
							w.kind = kWallProbe;
							w.y = f.y + kRayHeight;
							if (w.y > w.ceilY - 0.1f) {
								w.y = 0.5f * (f.y + w.ceilY);
							}
							// If the head probe hits too, the wall goes up to the far sample's next top
							// (a building's roof) or kWallCap.
							w.topGuess = f.y + kWallCap;
							const Event* e = a_c.Ev(q);
							for (int k = a_c.Count(q) - 1; k >= 0; --k) {  // bottom-up
								if (e[k].top && e[k].y > w.y + 0.3f) {
									if (e[k].y - f.y <= kWallMaxReach) {
										w.topGuess = std::max(w.topGuess, e[k].y);
									}
									break;
								}
							}
						}
						a_c.walls.push_back(w);
					}
				}
			}
		}
	}

	// Steps 1 and 3, resumable across frames: Run() probes until a_outOfTime() says stop.
	class ColumnProbe
	{
	public:
		ColumnProbe(int a_rx, int a_rz, float a_yLo, float a_yHi) : col_(std::make_shared<Column>())
		{
			col_->rx = a_rx;
			col_->rz = a_rz;
			col_->yLo = a_yLo;
			col_->yHi = a_yHi;
			col_->events.reserve(kSamples * 3);
		}

		// a_ray(const float from[3], const float to[3], Hit&) -> bool; a_outOfTime() -> bool.
		// Returns true once the column is complete.
		template <class Ray, class OutOfTime>
		bool Run(Ray&& a_ray, OutOfTime&& a_outOfTime)
		{
			while (phase_ < 2) {
				if (phase_ == 0) {
					if (cursor_ >= kSamples) {
						PlanWalls(*col_);
						phase_ = 1;
						cursor_ = 0;
						continue;
					}
					ProbeSample(a_ray, cursor_++);
				} else {
					if (cursor_ >= static_cast<int>(col_->walls.size())) {
						// Only walls that were found matter from here on (memory: a column has
						// ~1500 probes, a few dozen hits).
						auto& w = col_->walls;
						w.erase(std::remove_if(w.begin(), w.end(), [](const WallQuery& q) { return q.kind == kWallFollow || (!q.hit && q.kind != kWallEvidence); }),
							w.end());
						w.shrink_to_fit();
						col_->events.shrink_to_fit();
						phase_ = 2;
						break;
					}
					ProbeWall(a_ray, cursor_++);
				}
				if (a_outOfTime()) {
					break;
				}
			}
			return phase_ == 2;
		}

		bool                    Done() const { return phase_ == 2; }
		std::shared_ptr<Column> Result() const { return col_; }
		int                     Rx() const { return col_->rx; }
		int                     Rz() const { return col_->rz; }

		std::uint32_t verticalRays = 0, wallRays = 0, badHits = 0;

	private:
		template <class Ray>
		void Chain(Ray& a_ray, float a_x, float a_z, bool a_down, std::vector<Event>& a_out)
		{
			const float end = a_down ? col_->yLo : col_->yHi;
			float       y = a_down ? col_->yHi : col_->yLo;
			for (int k = 0; k < kMaxChain && (a_down ? y > end : y < end); ++k) {
				const float from[3] = { a_x, y, a_z }, to[3] = { a_x, end, a_z };
				Hit         h{};
				++verticalRays;
				if (!a_ray(from, to, h)) {
					break;
				}
				// A sane hit is on the segment (the game sometimes reports junk for probes that
				// start right at a surface): else skip ahead a little.
				const bool onSegment = std::fabs(h.pos[0] - a_x) < 0.05f && std::fabs(h.pos[2] - a_z) < 0.05f &&
				                       (a_down ? (h.pos[1] <= y + 0.01f && h.pos[1] >= end - 0.01f) : (h.pos[1] >= y - 0.01f && h.pos[1] <= end + 0.01f));
				if (!onSegment) {
					++badHits;
					y += a_down ? -0.1f : 0.1f;
					continue;
				}
				Event e{};
				e.y = h.pos[1];
				std::memcpy(e.n, h.n, sizeof(e.n));
				e.top = a_down;
				e.mat = h.mat;
				a_out.push_back(e);
				y = h.pos[1] + (a_down ? -kChainStep : kChainStep);
			}
		}

		template <class Ray>
		void ProbeSample(Ray& a_ray, int a_s)
		{
			const int   i = a_s % kGrid, j = a_s / kGrid;
			const float x = col_->X(i), z = col_->Z(j);
			scratch_.clear();
			Chain(a_ray, x, z, true, scratch_);
			Chain(a_ray, x, z, false, scratch_);
			std::stable_sort(scratch_.begin(), scratch_.end(), [](const Event& a, const Event& b) {
				return a.y > b.y || (a.y == b.y && a.top && !b.top);  // a slab thinner than the chain step: top first
			});
			if (scratch_.empty()) {
				++col_->emptySamples;
			}
			col_->events.insert(col_->events.end(), scratch_.begin(), scratch_.end());
			col_->first[a_s + 1] = static_cast<std::uint32_t>(col_->events.size());
		}

		template <class Ray>
		void ProbeWall(Ray& a_ray, int a_k)
		{
			WallQuery&  w = col_->walls[a_k];
			const int   i = w.sample % kGrid, j = w.sample / kGrid;
			const int   qi = i + kDirI[w.dir], qj = j + kDirJ[w.dir];
			const float from[3] = { col_->X(i), w.y, col_->Z(j) };
			const float to[3] = { col_->X(qi) + 0.1f * float(kDirI[w.dir]), w.y, col_->Z(qj) + 0.1f * float(kDirJ[w.dir]) };
			Hit         h{};
			++wallRays;
			bool hit = a_ray(from, to, h);
			if (hit) {
				const bool onSegment = std::fabs(h.pos[1] - w.y) < 0.05f && h.pos[0] >= std::min(from[0], to[0]) - 0.01f &&
				                       h.pos[0] <= std::max(from[0], to[0]) + 0.01f && h.pos[2] >= std::min(from[2], to[2]) - 0.01f &&
				                       h.pos[2] <= std::max(from[2], to[2]) + 0.01f;
				if (!onSegment) {
					++badHits;
					hit = false;
				}
			}
			w.done = true;
			w.hit = hit && std::fabs(h.n[1]) < kWalkableNy;
			w.mat = w.hit ? h.mat : kNoMaterial;
			if (hit) {
				std::memcpy(w.pos, h.pos, sizeof(w.pos));
				std::memcpy(w.n, h.n, sizeof(w.n));
			}
			if (w.kind == kWallFollow) {
				WallQuery& p = col_->walls[w.parent];
				p.follow = w.hit ? 1 : 0;
				p.followY = w.y;
				return;
			}
			if (w.kind == kWallProbe && w.hit) {
				const float y2 = std::min(w.ceilY - 0.05f, w.floorY + kHeadHeight);
				if (y2 > w.y + 0.3f) {
					WallQuery f = w;
					f.kind = kWallFollow;
					f.y = y2;
					f.parent = a_k;
					f.done = f.hit = false;
					col_->walls.push_back(f);  // w is dangling from here on
				}
			}
		}

		std::shared_ptr<Column> col_;
		std::vector<Event>      scratch_;
		int                     phase_ = 0;
		int                     cursor_ = 0;
	};

	// ---- step 4: triangles ------------------------------------------------------------------------

	enum TriKind : std::uint8_t
	{
		kFloor = 0,
		kCeiling = 1,
		kWall = 2,
	};

	struct Tri
	{
		float         v[9];
		std::uint32_t flags = 0;  // proto::ColTriFlags + material
		TriKind       kind = kFloor;
		float         fill = 0.0f;  // floors: voxel fill depth under the surface
	};

	inline void Cross(const float* a, const float* b, float* o)
	{
		o[0] = a[1] * b[2] - a[2] * b[1];
		o[1] = a[2] * b[0] - a[0] * b[2];
		o[2] = a[0] * b[1] - a[1] * b[0];
	}

	inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

	// Adds triangle (a, b, c) wound so that its normal points along a_want.
	inline void Emit(std::vector<Tri>& a_out, const float* a_a, const float* a_b, const float* a_c, const float* a_want, std::uint32_t a_flags, TriKind a_kind,
		float a_fill = 0.0f)
	{
		const float e1[3] = { a_b[0] - a_a[0], a_b[1] - a_a[1], a_b[2] - a_a[2] };
		const float e2[3] = { a_c[0] - a_a[0], a_c[1] - a_a[1], a_c[2] - a_a[2] };
		float       n[3];
		Cross(e1, e2, n);
		if (Dot(n, n) < 1e-10f) {
			return;
		}
		Tri t{};
		const bool flip = Dot(n, a_want) < 0.0f;
		std::memcpy(t.v, a_a, 12);
		std::memcpy(t.v + 3, flip ? a_c : a_b, 12);
		std::memcpy(t.v + 6, flip ? a_b : a_c, 12);
		t.flags = a_flags;
		t.kind = a_kind;
		t.fill = a_fill;
		a_out.push_back(t);
	}

	inline float PlaneSlope(const float* a_n, float a_dx, float a_dz)  // dy for a step (dx, dz) along the surface
	{
		return std::fabs(a_n[1]) > 0.2f ? -(a_n[0] * a_dx + a_n[2] * a_dz) / a_n[1] : 0.0f;
	}

	inline bool SameSurface(const Surf& a_f, const float* a_pf, const Surf& a_g, const float* a_pg)
	{
		const float dx = a_pg[0] - a_pf[0], dz = a_pg[1] - a_pf[1];
		const float dist = std::sqrt(dx * dx + dz * dz);
		const float dy = std::fabs(a_f.y - a_g.y);
		if (dy <= kMaxRise * dist / kSpacing + 1e-4f) {
			return true;
		}
		// one steep plane (a roof, a ramp): each lies on the other's plane
		return std::fabs(a_f.y + PlaneSlope(a_f.n, dx, dz) - a_g.y) <= kPlaneTol && std::fabs(a_g.y + PlaneSlope(a_g.n, -dx, -dz) - a_f.y) <= kPlaneTol;
	}

	// Floors (a_floors) or ceilings between the samples of every cell.
	inline void BuildSurfaces(const Column& a_c, bool a_floors, std::vector<Tri>& a_out)
	{
		std::array<std::vector<Surf>, 4> surfs;
		const float                       up[3] = { 0.0f, a_floors ? 1.0f : -1.0f, 0.0f };
		struct Cluster
		{
			int member[4];
		};
		std::vector<Cluster> clusters;
		// The wall probes by (sample, direction), to extend a floor up to a wall found between samples.
		std::vector<std::uint32_t> wallFirst(kSamples * 4 + 1, 0), wallIdx;
		if (a_floors) {
			for (const auto& w : a_c.walls) {
				++wallFirst[w.sample * 4 + w.dir + 1];
			}
			for (std::size_t k = 1; k < wallFirst.size(); ++k) {
				wallFirst[k] += wallFirst[k - 1];
			}
			wallIdx.resize(a_c.walls.size());
			std::vector<std::uint32_t> fillPos(wallFirst.begin(), wallFirst.end() - 1);
			for (std::uint32_t k = 0; k < a_c.walls.size(); ++k) {
				const auto& w = a_c.walls[k];
				wallIdx[fillPos[w.sample * 4 + w.dir]++] = k;
			}
		}
		auto walled = [&](int a_s, int a_d, float a_y) {
			for (std::uint32_t k = wallFirst[a_s * 4 + a_d]; k < wallFirst[a_s * 4 + a_d + 1]; ++k) {
				const auto& w = a_c.walls[wallIdx[k]];
				if (w.kind != kWallFollow && w.done && std::fabs(w.floorY - a_y) < 0.02f && (w.hit || w.kind == kWallEvidence)) {
					return true;
				}
			}
			return false;
		};
		for (int j = 0; j + 1 < kGrid; ++j) {
			for (int i = 0; i + 1 < kGrid; ++i) {
				// corners in cyclic order: A (i, j), B (i+1, j), D (i+1, j+1), C (i, j+1)
				const int   ci[4] = { i, i + 1, i + 1, i };
				const int   cj[4] = { j, j, j + 1, j + 1 };
				int         idx[4];
				float       pos[4][2];
				bool        any = false;
				for (int k = 0; k < 4; ++k) {
					idx[k] = Column::Index(ci[k], cj[k]);
					pos[k][0] = a_c.X(ci[k]);
					pos[k][1] = a_c.Z(cj[k]);
					Surfaces(a_c, idx[k], a_floors, surfs[k]);
					any |= !surfs[k].empty();
				}
				if (!any) {
					continue;
				}
				// Group the corners' surfaces into sheets, at most one surface per corner.
				clusters.clear();
				for (int k = 0; k < 4; ++k) {
					for (int m = 0; m < static_cast<int>(surfs[k].size()); ++m) {
						int   best = -1;
						float bestDy = kInf;
						for (int c = 0; c < static_cast<int>(clusters.size()); ++c) {
							if (clusters[c].member[k] >= 0) {
								continue;
							}
							for (int l = 0; l < 4; ++l) {
								const int o = clusters[c].member[l];
								if (o < 0 || !SameSurface(surfs[k][m], pos[k], surfs[l][o], pos[l])) {
									continue;
								}
								const float dy = std::fabs(surfs[k][m].y - surfs[l][o].y);
								if (dy < bestDy) {
									bestDy = dy;
									best = c;
								}
							}
						}
						if (best < 0) {
							clusters.push_back({ { -1, -1, -1, -1 } });
							best = static_cast<int>(clusters.size()) - 1;
						}
						clusters[best].member[k] = m;
					}
				}
				for (const auto& cl : clusters) {
					bool  present[4];
					float h[4];
					float fill = kInf, thickMin = kInf;
					int   real = 0;
					for (int k = 0; k < 4; ++k) {
						present[k] = cl.member[k] >= 0;
						if (present[k]) {
							const Surf& s = surfs[k][cl.member[k]];
							h[k] = s.y;
							thickMin = std::min(thickMin, s.thick);
							++real;
						}
					}
					// Extend the sheet to corners that are inside solid at its height (under a wall).
					for (int k = 0; k < 4 && real < 4; ++k) {
						if (present[k] || cl.member[k] >= 0) {
							continue;
						}
						const int a = (k + 1) % 4, b = (k + 3) % 4, c = (k + 2) % 4;
						float     sum = 0.0f;
						int       cnt = 0;
						for (int o : { a, b }) {
							if (cl.member[o] >= 0) {
								sum += surfs[o][cl.member[o]].y;
								++cnt;
							}
						}
						if (!cnt && cl.member[c] >= 0) {
							sum = surfs[c][cl.member[c]].y;
							cnt = 1;
						}
						if (!cnt) {
							continue;
						}
						const float hx = sum / float(cnt);
						bool        extend = a_c.SolidAt(idx[k], hx + (a_floors ? 0.1f : -0.1f));
						// ...or a wall stands between it and a neighbour on the sheet (a room's floor
						// reaches its walls even when what's behind them reads as free)
						for (int o : { a, b }) {
							if (!extend && a_floors && cl.member[o] >= 0) {
								const int di = ci[k] - ci[o], dj = cj[k] - cj[o];
								const int d = di > 0 ? 0 : di < 0 ? 1 : dj > 0 ? 2 : 3;
								extend = walled(idx[o], d, surfs[o][cl.member[o]].y);
							}
						}
						if (extend) {
							present[k] = true;
							h[k] = hx;
						}
					}
					int count = 0;
					for (bool p : present) {
						count += p ? 1 : 0;
					}
					if (count < 3) {
						continue;
					}
					// the sheet's material: the one most of its corners were probed on
					std::uint8_t mat = kNoMaterial;
					int          matVotes = 0;
					for (int k = 0; k < 4; ++k) {
						if (cl.member[k] < 0 || surfs[k][cl.member[k]].mat == kNoMaterial) {
							continue;
						}
						int votes = 0;
						for (int l = 0; l < 4; ++l) {
							votes += cl.member[l] >= 0 && surfs[l][cl.member[l]].mat == surfs[k][cl.member[k]].mat ? 1 : 0;
						}
						if (votes > matVotes) {
							matVotes = votes;
							mat = surfs[k][cl.member[k]].mat;
						}
					}
					std::uint32_t flags = MaterialFlags(mat);
					if (a_floors) {
						fill = thickMin <= 0.0f ? kSheet : std::min(kFillDepth, thickMin);
						if (thickMin >= kTerrainThick) {
							flags |= proto::kTriTerrain | proto::kTriDiggable;  // material below, per triangle
						}
					}
					float v[4][3];
					for (int k = 0; k < 4; ++k) {
						v[k][0] = pos[k][0];
						v[k][1] = present[k] ? h[k] : 0.0f;
						v[k][2] = pos[k][1];
					}
					auto emit = [&](int a, int b, int c) {
						std::uint32_t f = flags;
						if (f & proto::kTriDiggable) {
							const float e1[3] = { v[b][0] - v[a][0], v[b][1] - v[a][1], v[b][2] - v[a][2] };
							const float e2[3] = { v[c][0] - v[a][0], v[c][1] - v[a][1], v[c][2] - v[a][2] };
							float       n[3];
							Cross(e1, e2, n);
							const float len = std::sqrt(Dot(n, n));
							const auto  material = len > 0.0f && std::fabs(n[1]) / len >= kWalkableNy ? proto::kDigDirt : proto::kDigStone;
							f |= std::uint32_t(material) << proto::kTriMaterialShift;
						}
						Emit(a_out, v[a], v[b], v[c], up, f, a_floors ? kFloor : kCeiling, fill);
					};
					if (count == 4) {
						// split along the diagonal whose ends are closer in height
						if (std::fabs(h[0] - h[2]) <= std::fabs(h[1] - h[3])) {
							emit(0, 1, 2);
							emit(0, 2, 3);
						} else {
							emit(0, 1, 3);
							emit(1, 2, 3);
						}
					} else {
						int t[3], n = 0;
						for (int k = 0; k < 4; ++k) {
							if (present[k]) {
								t[n++] = k;
							}
						}
						emit(t[0], t[1], t[2]);
					}
				}
			}
		}
	}

	// Vertical quad pieces between y0 and y1, split at region boundaries.
	inline void EmitWall(std::vector<Tri>& a_out, const float a_w[2], const float a_t[2], float a_hw, const float a_n[3], float a_y0, float a_y1,
		std::uint32_t a_flags = 0)
	{
		const float x0 = a_w[0] - a_t[0] * a_hw, z0 = a_w[1] - a_t[1] * a_hw;
		const float x1 = a_w[0] + a_t[0] * a_hw, z1 = a_w[1] + a_t[1] * a_hw;
		float       y = a_y0;
		while (y < a_y1 - 1e-4f) {
			const float next = std::min(a_y1, (std::floor(y / float(kRegion) + 1e-4f) + 1.0f) * float(kRegion));
			const float a[3] = { x0, y, z0 }, b[3] = { x1, y, z1 }, c[3] = { x1, next, z1 }, d[3] = { x0, next, z0 };
			Emit(a_out, a, b, c, a_n, a_flags, kWall);
			Emit(a_out, a, c, d, a_n, a_flags, kWall);
			y = next;
		}
	}

	inline void BuildWalls(const Column& a_c, std::vector<Tri>& a_out)
	{
		for (const auto& w : a_c.walls) {
			if (w.kind == kWallFollow || !w.done) {
				continue;
			}
			const int   i = w.sample % kGrid, j = w.sample / kGrid;
			const float dx = float(kDirI[w.dir]), dz = float(kDirJ[w.dir]);
			float       at[2], hn[2];
			if (w.hit) {
				const float l = std::sqrt(w.n[0] * w.n[0] + w.n[2] * w.n[2]);
				if (l > 0.3f) {
					hn[0] = w.n[0] / l;
					hn[1] = w.n[2] / l;
				} else {
					hn[0] = -dx;
					hn[1] = -dz;
				}
				at[0] = w.pos[0];
				at[1] = w.pos[2];
			} else if (w.kind == kWallEvidence) {
				hn[0] = -dx;
				hn[1] = -dz;
				at[0] = a_c.X(i) + 0.5f * kSpacing * dx;
				at[1] = a_c.Z(j) + 0.5f * kSpacing * dz;
			} else {
				continue;  // a body-height probe that hit nothing: no wall
			}
			float lo, hi;
			if (w.kind == kWallEvidence) {
				lo = w.oLo <= w.floorY + kStep ? w.floorY - 0.05f : w.oLo;
				hi = w.oHi;
			} else {
				lo = w.floorY - 0.05f;
				if (w.follow == 1) {
					hi = std::min(w.ceilY, w.topGuess);
				} else if (w.follow == 0) {
					hi = std::min(w.ceilY, w.floorY + kRailTop);
				} else {
					hi = std::min(w.ceilY, w.floorY + kWallCap);  // no room for a head probe
				}
			}
			hi = std::min(hi, a_c.yHi);
			lo = std::max(lo, a_c.yLo);
			if (hi - lo < 0.05f) {
				continue;
			}
			// The edge stands for a stripe one sample spacing wide across it; along the wall that's
			// wider when the wall runs at an angle to the grid.
			const float t[2] = { -hn[1], hn[0] };
			const float lateral = std::fabs(dx) > 0.5f ? std::fabs(t[1]) : std::fabs(t[0]);
			const float hw = std::min(0.75f, 0.5f * kSpacing / std::max(lateral, 0.3f)) + 0.02f;
			const float n3[3] = { hn[0], 0.0f, hn[1] };
			EmitWall(a_out, at, t, hw, n3, lo, hi, MaterialFlags(w.mat));
		}
	}

	inline void BuildColumn(const Column& a_c, std::vector<Tri>& a_out)
	{
		a_out.clear();
		BuildSurfaces(a_c, true, a_out);
		BuildSurfaces(a_c, false, a_out);
		BuildWalls(a_c, a_out);
	}

	inline void TriBounds(const Tri& a_t, float* a_lo, float* a_hi)
	{
		for (int k = 0; k < 3; ++k) {
			a_lo[k] = std::min({ a_t.v[k], a_t.v[3 + k], a_t.v[6 + k] });
			a_hi[k] = std::max({ a_t.v[k], a_t.v[3 + k], a_t.v[6 + k] });
		}
	}

	// Triangles for region (rx, ry, rz): those overlapping its box grown by half a block (SkyCraft).
	inline void RegionTris(const std::vector<Tri>& a_all, int a_rx, int a_ry, int a_rz, std::vector<Tri>& a_out)
	{
		a_out.clear();
		const float lo[3] = { float(a_rx * kRegion) - 0.5f, float(a_ry * kRegion) - 0.5f, float(a_rz * kRegion) - 0.5f };
		const float hi[3] = { lo[0] + kRegion + 1.0f, lo[1] + kRegion + 1.0f, lo[2] + kRegion + 1.0f };
		for (const auto& t : a_all) {
			float tlo[3], thi[3];
			TriBounds(t, tlo, thi);
			if (thi[0] >= lo[0] && tlo[0] <= hi[0] && thi[1] >= lo[1] && tlo[1] <= hi[1] && thi[2] >= lo[2] && tlo[2] <= hi[2]) {
				a_out.push_back(t);
			}
		}
	}

	// ---- voxels -----------------------------------------------------------------------------------

	namespace detail
	{
		inline bool AxisTest(const float* v0, const float* v1, const float* v2, const float* axis, float h)
		{
			const float p0 = Dot(v0, axis), p1 = Dot(v1, axis), p2 = Dot(v2, axis);
			const float r = h * (std::fabs(axis[0]) + std::fabs(axis[1]) + std::fabs(axis[2]));
			return !(std::min({ p0, p1, p2 }) > r || std::max({ p0, p1, p2 }) < -r);
		}

		// Triangle (a, b, c) against the cube centred at a_c with half size a_h (separating axes).
		inline bool TriBoxOverlap(const float* a_c, float a_h, const float* a_a, const float* a_b, const float* a_cc)
		{
			float v0[3], v1[3], v2[3];
			for (int k = 0; k < 3; ++k) {
				v0[k] = a_a[k] - a_c[k];
				v1[k] = a_b[k] - a_c[k];
				v2[k] = a_cc[k] - a_c[k];
			}
			for (int k = 0; k < 3; ++k) {
				if (std::min({ v0[k], v1[k], v2[k] }) > a_h || std::max({ v0[k], v1[k], v2[k] }) < -a_h) {
					return false;
				}
			}
			const float e[3][3] = { { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] }, { v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2] },
				{ v0[0] - v2[0], v0[1] - v2[1], v0[2] - v2[2] } };
			float n[3];
			Cross(e[0], e[1], n);
			if (!AxisTest(v0, v0, v0, n, a_h)) {
				return false;
			}
			const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			for (const auto& ed : e) {
				for (const auto& ax : axes) {
					float a[3];
					Cross(ed, ax, a);
					if (Dot(a, a) > 1e-12f && !AxisTest(v0, v1, v2, a, a_h)) {
						return false;
					}
				}
			}
			return true;
		}
	}

	// The region's 1/8-block voxels from its triangles: floors filled down by their fill depth,
	// everything else as a shell.
	inline void Voxelize(const std::vector<Tri>& a_tris, int a_rx, int a_ry, int a_rz, std::vector<proto::ColBlock>& a_out)
	{
		constexpr int              G = kRegion * 8;
		std::vector<std::uint64_t> grid(G * G, 0);  // [y * G + z], bit x
		const float                ox = float(a_rx * kRegion), oy = float(a_ry * kRegion), oz = float(a_rz * kRegion);
		auto set = [&](int x, int y, int z) { grid[y * G + z] |= 1ull << x; };
		auto clampLo = [](float v) { return std::clamp(static_cast<int>(std::floor(v)), 0, G - 1); };
		auto clampHi = [](float v) { return std::clamp(static_cast<int>(std::ceil(v)) - 1, 0, G - 1); };

		for (const auto& t : a_tris) {
			float lo[3], hi[3];
			TriBounds(t, lo, hi);
			// to voxel units
			const float vlo[3] = { (lo[0] - ox) * 8.0f, (lo[1] - oy) * 8.0f, (lo[2] - oz) * 8.0f };
			const float vhi[3] = { (hi[0] - ox) * 8.0f, (hi[1] - oy) * 8.0f, (hi[2] - oz) * 8.0f };
			if (t.kind == kFloor) {
				if (vhi[0] < 0 || vhi[2] < 0 || vlo[0] > G || vlo[2] > G || vhi[1] < 0.0f || vlo[1] - t.fill * 8.0f > G) {
					continue;
				}
				const float* a = t.v;
				const float* b = t.v + 3;
				const float* c = t.v + 6;
				const float  den = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
				if (std::fabs(den) < 1e-9f) {
					continue;  // vertical in XZ: no footprint
				}
				for (int vz = clampLo(vlo[2]); vz <= clampHi(vhi[2]); ++vz) {
					for (int vx = clampLo(vlo[0]); vx <= clampHi(vhi[0]); ++vx) {
						const float px = ox + (float(vx) + 0.5f) / 8.0f, pz = oz + (float(vz) + 0.5f) / 8.0f;
						const float l0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / den;
						const float l1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / den;
						const float l2 = 1.0f - l0 - l1;
						constexpr float eps = 1e-4f;
						if (l0 < -eps || l1 < -eps || l2 < -eps) {
							continue;
						}
						const float h = l0 * a[1] + l1 * b[1] + l2 * c[1];
						// voxel centres in [h - fill, h]
						const int y0 = std::max(0, static_cast<int>(std::ceil((h - t.fill - oy) * 8.0f - 0.5f)));
						const int y1 = std::min(G - 1, static_cast<int>(std::floor((h - oy) * 8.0f - 0.5f)));
						for (int vy = y0; vy <= y1; ++vy) {
							set(vx, vy, vz);
						}
					}
				}
				continue;
			}
			if (vhi[0] < 0 || vhi[1] < 0 || vhi[2] < 0 || vlo[0] > G || vlo[1] > G || vlo[2] > G) {
				continue;
			}
			const float a[3] = { (t.v[0] - ox) * 8.0f, (t.v[1] - oy) * 8.0f, (t.v[2] - oz) * 8.0f };
			const float b[3] = { (t.v[3] - ox) * 8.0f, (t.v[4] - oy) * 8.0f, (t.v[5] - oz) * 8.0f };
			const float c[3] = { (t.v[6] - ox) * 8.0f, (t.v[7] - oy) * 8.0f, (t.v[8] - oz) * 8.0f };
			for (int vy = clampLo(vlo[1]); vy <= clampHi(vhi[1]); ++vy) {
				for (int vz = clampLo(vlo[2]); vz <= clampHi(vhi[2]); ++vz) {
					for (int vx = clampLo(vlo[0]); vx <= clampHi(vhi[0]); ++vx) {
						const float cen[3] = { float(vx) + 0.5f, float(vy) + 0.5f, float(vz) + 0.5f };
						if (detail::TriBoxOverlap(cen, 0.5f, a, b, c)) {
							set(vx, vy, vz);
						}
					}
				}
			}
		}

		a_out.clear();
		for (int by = 0; by < kRegion; ++by) {
			for (int bz = 0; bz < kRegion; ++bz) {
				for (int bx = 0; bx < kRegion; ++bx) {
					proto::ColBlock blk{};
					bool            any = false;
					for (int sy = 0; sy < 8; ++sy) {
						std::uint64_t layer = 0;
						for (int sz = 0; sz < 8; ++sz) {
							const std::uint64_t row = (grid[(by * 8 + sy) * G + (bz * 8 + sz)] >> (bx * 8)) & 0xFF;
							layer |= row << (sz * 8);
						}
						blk.bits[sy] = layer;
						any |= layer != 0;
					}
					if (any) {
						blk.x = a_rx * kRegion + bx;
						blk.y = a_ry * kRegion + by;
						blk.z = a_rz * kRegion + bz;
						a_out.push_back(blk);
					}
				}
			}
		}
	}
}
