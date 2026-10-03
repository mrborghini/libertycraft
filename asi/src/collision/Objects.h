// Collision v2, objects: GTA IV's street furniture (lamp posts, bins, hydrants, benches, phone
// boxes, bollards...) as Minecraft collision. Header-only and SDK-free like Geometry.h (the probes
// come in through a callback), unit-tested by asi/tests/collision_test.cpp.
//
// The map's static collision (BUILDINGS, Geometry.h) has none of these: they are CObjects that
// GTA streams in near the player. Collision.cpp walks the object pool, picks the objects worth
// colliding with (ObjectBox + Classify: not doors, not tiny, not attached to a ped or car) and
// probes each one ONCE with vertical line probes that only report hits on that object:
//  * a grid over the object's footprint in its own heading frame, as fine as 1/16 block for small
//    things (a bollard, a post) and coarser for big ones (cells capped per object);
//  * per cell a chain down from above its bounds (tops) and, if that hit anything, a chain up from
//    below (undersides); solid is where more tops than undersides lie above (pieces resting on
//    each other merge; a top with no underside below reaches down to the object's bottom);
//  * cells with the same solid spans merge into rectangles: oriented boxes, sent as triangles
//    (top = floor filled down to the box's bottom, sides = walls, undersides of overhangs =
//    ceilings) that Collision.cpp merges into the regions of every column they touch.
// A lamp post's arm stays an arm (free under it); doors stay passable (Classify): Niko is frozen
// with collision off while Minecraft drives him, so GTA's doors never swing open for him and a
// solid door would lock the player out of every building.
#pragma once

#include "collision/Geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace lc::col
{
	// ---- tuning ---------------------------------------------------------------------------------
	inline constexpr float kObjFineStep = 1.0f / 16.0f;  // finest sample spacing (thin posts)
	inline constexpr float kObjStep = 0.125f;            // spacing once a footprint is too big for fine cells
	inline constexpr int   kObjTargetCells = 160;        // aim for this many cells per object...
	inline constexpr int   kObjMaxCells = 640;           // ...and never more than this
	inline constexpr float kObjMargin = 0.1f;            // probe this far around the model's bounds
	inline constexpr int   kObjMaxChain = 8;             // hits per chain
	inline constexpr float kObjChainStep = 0.02f;        // restart a chain this far past a hit
	inline constexpr float kObjMergeTol = 0.1f;          // cells whose spans differ less than this merge
	inline constexpr float kObjOverhang = 0.3f;          // a box bottom this far above the object's lowest is an overhang (ceiling)
	inline constexpr float kObjMinSpan = 0.03f;          // thinner solid spans are dropped
	inline constexpr float kObjTiny = 0.2f;              // objects smaller than this in every direction are ignored (litter, cans)
	inline constexpr float kObjHuge = 40.0f;             // ...or larger than this in any (not street furniture)
	// A door: upright, thin, door-wide and door-tall (local model bounds). GTA IV's door models
	// have their origin at the hinge; that is logged, not required: a door that ends up solid locks
	// the player out of a building, a sign that ends up passable costs nothing.
	inline constexpr float kDoorMaxThick = 0.4f;
	inline constexpr float kDoorMinWidth = 0.45f, kDoorMaxWidth = 3.4f;
	inline constexpr float kDoorMinHeight = 1.5f, kDoorMaxHeight = 4.5f;
	inline constexpr float kDoorUpright = 0.9f;  // local z . world up

	// An object's pose and model bounds in Minecraft space.
	struct ObjectBox
	{
		float o[3]{};        // origin (MC)
		float axes[3][3]{};  // the model's local x, y, z axes in MC space (GTA matrix rows right, up, at)
		float lo[3]{}, hi[3]{};  // model bounds in its local frame (metres = blocks)
	};

	// GTA (x, y, z) -> MC (x, z, -y), for points and directions alike.
	inline void GtaToMcVec(const float* a_g, float* a_m)
	{
		a_m[0] = a_g[0];
		a_m[1] = a_g[2];
		a_m[2] = -a_g[1];
	}

	// From a GTA matrix (rows right, up, at, pos: 3 floats each) and local model bounds.
	inline ObjectBox MakeObjectBox(const float a_right[3], const float a_up[3], const float a_at[3], const float a_pos[3], const float a_lo[3],
		const float a_hi[3])
	{
		ObjectBox b;
		GtaToMcVec(a_pos, b.o);
		GtaToMcVec(a_right, b.axes[0]);
		GtaToMcVec(a_up, b.axes[1]);
		GtaToMcVec(a_at, b.axes[2]);
		for (int k = 0; k < 3; ++k) {
			b.lo[k] = std::min(a_lo[k], a_hi[k]);
			b.hi[k] = std::max(a_lo[k], a_hi[k]);
		}
		return b;
	}

	// World point of local (x, y, z).
	inline void LocalToMc(const ObjectBox& a_b, const float* a_l, float* a_out)
	{
		for (int k = 0; k < 3; ++k) {
			a_out[k] = a_b.o[k] + a_b.axes[0][k] * a_l[0] + a_b.axes[1][k] * a_l[1] + a_b.axes[2][k] * a_l[2];
		}
	}

	// MC-space bounding box of the model's (grown) bounds.
	inline void ObjectAabb(const ObjectBox& a_b, float a_grow, float* a_lo, float* a_hi)
	{
		for (int k = 0; k < 3; ++k) {
			a_lo[k] = kInf;
			a_hi[k] = -kInf;
		}
		for (int c = 0; c < 8; ++c) {
			const float l[3] = { (c & 1) ? a_b.hi[0] + a_grow : a_b.lo[0] - a_grow, (c & 2) ? a_b.hi[1] + a_grow : a_b.lo[1] - a_grow,
				(c & 4) ? a_b.hi[2] + a_grow : a_b.lo[2] - a_grow };
			float p[3];
			LocalToMc(a_b, l, p);
			for (int k = 0; k < 3; ++k) {
				a_lo[k] = std::min(a_lo[k], p[k]);
				a_hi[k] = std::max(a_hi[k], p[k]);
			}
		}
	}

	enum class ObjClass : std::uint8_t
	{
		kSolid = 0,
		kDoor,
		kTiny,
		kHuge,
		kBadBounds,
	};

	inline const char* ObjClassName(ObjClass a_c)
	{
		switch (a_c) {
		case ObjClass::kSolid:
			return "solid";
		case ObjClass::kDoor:
			return "door";
		case ObjClass::kTiny:
			return "tiny";
		case ObjClass::kHuge:
			return "huge";
		default:
			return "bad bounds";
		}
	}

	struct DoorShape
	{
		bool  upright = false;
		float thick = 0.0f, width = 0.0f, height = 0.0f;
		bool  hingeAtEdge = false;  // origin within 0.2 m of one end of the wide axis
	};

	inline DoorShape DoorMeasure(const ObjectBox& a_b)
	{
		DoorShape d;
		const float ex = a_b.hi[0] - a_b.lo[0], ey = a_b.hi[1] - a_b.lo[1];
		d.height = a_b.hi[2] - a_b.lo[2];
		const int w = ex >= ey ? 0 : 1;
		d.width = std::max(ex, ey);
		d.thick = std::min(ex, ey);
		const float zl = std::sqrt(Dot(a_b.axes[2], a_b.axes[2]));
		d.upright = zl > 1e-3f && a_b.axes[2][1] / zl >= kDoorUpright;
		d.hingeAtEdge = std::fabs(a_b.lo[w]) < 0.2f || std::fabs(a_b.hi[w]) < 0.2f;
		return d;
	}

	inline ObjClass Classify(const ObjectBox& a_b)
	{
		float ext[3];
		for (int k = 0; k < 3; ++k) {
			ext[k] = a_b.hi[k] - a_b.lo[k];
			if (!std::isfinite(ext[k]) || !std::isfinite(a_b.o[k]) || ext[k] < 0.0f) {
				return ObjClass::kBadBounds;
			}
		}
		if (ext[0] < kObjTiny && ext[1] < kObjTiny && ext[2] < kObjTiny) {
			return ObjClass::kTiny;
		}
		if (ext[0] > kObjHuge || ext[1] > kObjHuge || ext[2] > kObjHuge) {
			return ObjClass::kHuge;
		}
		const DoorShape d = DoorMeasure(a_b);
		if (d.upright && d.thick <= kDoorMaxThick && d.width >= kDoorMinWidth && d.width <= kDoorMaxWidth && d.height >= kDoorMinHeight &&
			d.height <= kDoorMaxHeight) {
			return ObjClass::kDoor;
		}
		return ObjClass::kSolid;
	}

	// A solid span along a vertical line.
	struct Span
	{
		float lo, hi;
	};

	// Top-down events of one vertical line -> its solid spans (top-down). Tops open solid, undersides
	// close it (counted, so pieces resting on each other merge); still open at the bottom: solid
	// down to a_bottom. Returns the number of undersides with no top above (ignored).
	inline int SpansFromEvents(const std::vector<Event>& a_events, float a_bottom, std::vector<Span>& a_out)
	{
		a_out.clear();
		int   depth = 0, unpaired = 0;
		float start = 0.0f;
		for (const auto& e : a_events) {
			if (e.top) {
				if (depth++ == 0) {
					start = e.y;
				}
			} else if (depth > 0) {
				if (--depth == 0 && start - e.y >= kObjMinSpan) {
					a_out.push_back({ e.y, start });
				}
			} else {
				++unpaired;
			}
		}
		if (depth > 0 && start - a_bottom >= kObjMinSpan) {
			a_out.push_back({ a_bottom, start });
		}
		return unpaired;
	}

	// An oriented box: [a0, a1] x [b0, b1] in the object's heading frame (origin o, axes u, v in
	// MC xz), [y0, y1] in MC y.
	struct OBox
	{
		float o[2], u[2], v[2];
		float a0, a1, b0, b1, y0, y1;
	};

	// Does the OBox come within a_r (horizontally) of point (x, z) between heights y0..y1?
	inline bool OBoxNear(const OBox& a_b, float a_x, float a_y0, float a_y1, float a_z, float a_r)
	{
		if (a_y1 < a_b.y0 || a_y0 > a_b.y1) {
			return false;
		}
		const float dx = a_x - a_b.o[0], dz = a_z - a_b.o[1];
		const float a = dx * a_b.u[0] + dz * a_b.u[1], b = dx * a_b.v[0] + dz * a_b.v[1];
		const float ca = std::clamp(a, a_b.a0, a_b.a1), cb = std::clamp(b, a_b.b0, a_b.b1);
		return (a - ca) * (a - ca) + (b - cb) * (b - cb) <= a_r * a_r;
	}

	// The triangles of a set of boxes: tops are floors filled down to the box bottom, sides walls,
	// bottoms above the lowest box bottom (overhangs: a lamp arm, a bench seat) ceilings.
	inline void BoxTris(const std::vector<OBox>& a_boxes, std::vector<Tri>& a_out)
	{
		float lowest = kInf;
		for (const auto& b : a_boxes) {
			lowest = std::min(lowest, b.y0);
		}
		for (const auto& b : a_boxes) {
			auto at = [&](float a, float c, float y, float* p) {
				p[0] = b.o[0] + b.u[0] * a + b.v[0] * c;
				p[1] = y;
				p[2] = b.o[1] + b.u[1] * a + b.v[1] * c;
			};
			float       p00[3], p10[3], p11[3], p01[3];
			const float up[3] = { 0.0f, 1.0f, 0.0f }, down[3] = { 0.0f, -1.0f, 0.0f };
			at(b.a0, b.b0, b.y1, p00);
			at(b.a1, b.b0, b.y1, p10);
			at(b.a1, b.b1, b.y1, p11);
			at(b.a0, b.b1, b.y1, p01);
			Emit(a_out, p00, p10, p11, up, 0, kFloor, b.y1 - b.y0);
			Emit(a_out, p00, p11, p01, up, 0, kFloor, b.y1 - b.y0);
			if (b.y0 > lowest + kObjOverhang) {
				for (float* p : { p00, p10, p11, p01 }) {
					p[1] = b.y0;
				}
				Emit(a_out, p00, p10, p11, down, 0, kCeiling);
				Emit(a_out, p00, p11, p01, down, 0, kCeiling);
			}
			// sides: centre, tangent, half width, outward normal
			const float am = 0.5f * (b.a0 + b.a1), bm = 0.5f * (b.b0 + b.b1);
			const float ha = 0.5f * (b.a1 - b.a0), hb = 0.5f * (b.b1 - b.b0);
			struct Side
			{
				float a, c, n[2], t[2], hw;
			};
			const Side sides[4] = {
				{ b.a1, bm, { b.u[0], b.u[1] }, { b.v[0], b.v[1] }, hb },
				{ b.a0, bm, { -b.u[0], -b.u[1] }, { b.v[0], b.v[1] }, hb },
				{ am, b.b1, { b.v[0], b.v[1] }, { b.u[0], b.u[1] }, ha },
				{ am, b.b0, { -b.v[0], -b.v[1] }, { b.u[0], b.u[1] }, ha },
			};
			for (const auto& s : sides) {
				float c[3];
				at(s.a, s.c, 0.0f, c);
				const float w[2] = { c[0], c[2] };
				const float n3[3] = { s.n[0], 0.0f, s.n[1] };
				EmitWall(a_out, w, s.t, s.hw, n3, b.y0, b.y1);
			}
		}
	}

	// FNV-1a over the triangles to the centimetre: did a re-probe change the shape?
	inline std::uint64_t TrisHash(const std::vector<Tri>& a_tris)
	{
		std::uint64_t h = 1469598103934665603ull;
		for (const auto& t : a_tris) {
			for (float v : t.v) {
				h ^= static_cast<std::uint64_t>(std::llround(v * 100.0f));
				h *= 1099511628211ull;
			}
			h ^= static_cast<std::uint64_t>(t.kind);
			h *= 1099511628211ull;
		}
		return h;
	}

	// Probes one object, resumable across frames like ColumnProbe. The ray callback must report
	// hits on this object only (Collision.cpp skips past anything else).
	class ObjectProbe
	{
	public:
		explicit ObjectProbe(const ObjectBox& a_b)
		{
			// Heading frame: the model's x axis flattened (its y axis if x points up).
			float ux = a_b.axes[0][0], uz = a_b.axes[0][2];
			if (ux * ux + uz * uz < 0.04f) {
				ux = a_b.axes[1][0];
				uz = a_b.axes[1][2];
			}
			const float ul = std::sqrt(ux * ux + uz * uz);
			if (ul > 1e-4f) {
				u_[0] = ux / ul;
				u_[1] = uz / ul;
			}
			v_[0] = -u_[1];
			v_[1] = u_[0];
			o_[0] = a_b.o[0];
			o_[1] = a_b.o[2];
			// footprint of the grown bounds in that frame, and their height
			float aLo = kInf, aHi = -kInf, bLo = kInf, bHi = -kInf;
			yLo_ = kInf;
			yHi_ = -kInf;
			for (int c = 0; c < 8; ++c) {
				const float l[3] = { (c & 1) ? a_b.hi[0] + kObjMargin : a_b.lo[0] - kObjMargin, (c & 2) ? a_b.hi[1] + kObjMargin : a_b.lo[1] - kObjMargin,
					(c & 4) ? a_b.hi[2] + kObjMargin : a_b.lo[2] - kObjMargin };
				float p[3];
				LocalToMc(a_b, l, p);
				const float dx = p[0] - o_[0], dz = p[2] - o_[1];
				const float a = dx * u_[0] + dz * u_[1], b = dx * v_[0] + dz * v_[1];
				aLo = std::min(aLo, a);
				aHi = std::max(aHi, a);
				bLo = std::min(bLo, b);
				bHi = std::max(bHi, b);
				yLo_ = std::min(yLo_, p[1]);
				yHi_ = std::max(yHi_, p[1]);
			}
			const float area = std::max(1e-4f, (aHi - aLo) * (bHi - bLo));
			step_ = std::max(kObjFineStep, std::sqrt(area / float(kObjTargetCells)));
			if (step_ > kObjStep) {
				step_ = std::max(kObjStep, std::sqrt(area / float(kObjMaxCells)));
			}
			nu_ = std::max(1, static_cast<int>(std::ceil((aHi - aLo) / step_ - 1e-3f)));
			nv_ = std::max(1, static_cast<int>(std::ceil((bHi - bLo) / step_ - 1e-3f)));
			// centre the grid on the footprint
			a0_ = 0.5f * (aLo + aHi) - 0.5f * float(nu_) * step_;
			b0_ = 0.5f * (bLo + bHi) - 0.5f * float(nv_) * step_;
			first_.assign(static_cast<std::size_t>(nu_ * nv_) + 1, 0);
		}

		template <class Ray, class OutOfTime>
		bool Run(Ray&& a_ray, OutOfTime&& a_outOfTime)
		{
			const int n = nu_ * nv_;
			while (cursor_ < n) {
				ProbeCell(a_ray, cursor_++);
				if (a_outOfTime()) {
					break;
				}
			}
			return cursor_ >= n;
		}

		bool Done() const { return cursor_ >= nu_ * nv_; }
		int  Cells() const { return nu_ * nv_; }
		float Step() const { return step_; }

		// The probed shape as boxes (cells with the same spans merged into rectangles).
		void Boxes(std::vector<OBox>& a_out) const
		{
			a_out.clear();
			struct Rect
			{
				int i0, i1, j0, j1;
				std::vector<Span> spans;
				bool open;
			};
			std::vector<Rect> rects;  // rectangles still growing in j (open) and finished ones
			auto same = [](const Span* a, int na, const Span* b, int nb) {
				if (na != nb) {
					return false;
				}
				for (int k = 0; k < na; ++k) {
					if (std::fabs(a[k].lo - b[k].lo) > kObjMergeTol || std::fabs(a[k].hi - b[k].hi) > kObjMergeTol) {
						return false;
					}
				}
				return true;
			};
			auto widen = [](std::vector<Span>& a, const Span* b) {
				for (std::size_t k = 0; k < a.size(); ++k) {
					a[k].lo = std::min(a[k].lo, b[k].lo);
					a[k].hi = std::max(a[k].hi, b[k].hi);
				}
			};
			for (int j = 0; j < nv_; ++j) {
				// runs along i in row j
				std::vector<Rect> runs;
				for (int i = 0; i < nu_;) {
					const int   s = j * nu_ + i;
					const int   cnt = Count(s);
					const Span* sp = Sp(s);
					if (cnt == 0) {
						++i;
						continue;
					}
					Rect r{ i, i, j, j, std::vector<Span>(sp, sp + cnt), true };
					int  k = i + 1;
					while (k < nu_ && same(r.spans.data(), int(r.spans.size()), Sp(j * nu_ + k), Count(j * nu_ + k))) {
						widen(r.spans, Sp(j * nu_ + k));
						++k;
					}
					r.i1 = k - 1;
					runs.push_back(std::move(r));
					i = k;
				}
				// extend the open rectangles of the row before with runs of the same extent
				for (auto& rect : rects) {
					if (!rect.open) {
						continue;
					}
					bool extended = false;
					for (auto& run : runs) {
						if (run.open && run.i0 == rect.i0 && run.i1 == rect.i1 && same(rect.spans.data(), int(rect.spans.size()), run.spans.data(), int(run.spans.size()))) {
							widen(rect.spans, run.spans.data());
							rect.j1 = j;
							run.open = false;  // consumed
							extended = true;
							break;
						}
					}
					rect.open = extended;
				}
				for (auto& run : runs) {
					if (run.open) {
						rects.push_back(std::move(run));
					}
				}
			}
			for (const auto& r : rects) {
				for (const auto& s : r.spans) {
					OBox b{};
					b.o[0] = o_[0];
					b.o[1] = o_[1];
					b.u[0] = u_[0];
					b.u[1] = u_[1];
					b.v[0] = v_[0];
					b.v[1] = v_[1];
					b.a0 = a0_ + float(r.i0) * step_;
					b.a1 = a0_ + float(r.i1 + 1) * step_;
					b.b0 = b0_ + float(r.j0) * step_;
					b.b1 = b0_ + float(r.j1 + 1) * step_;
					b.y0 = s.lo;
					b.y1 = s.hi;
					a_out.push_back(b);
				}
			}
		}

		std::uint32_t rays = 0, hits = 0, bad = 0, unpaired = 0;

	private:
		int         Count(int a_s) const { return int(first_[a_s + 1] - first_[a_s]); }
		const Span* Sp(int a_s) const { return spans_.data() + first_[a_s]; }

		template <class Ray>
		void Chain(Ray& a_ray, float a_x, float a_z, bool a_down)
		{
			const float end = a_down ? yLo_ : yHi_;
			float       y = a_down ? yHi_ : yLo_;
			for (int k = 0; k < kObjMaxChain && (a_down ? y > end : y < end); ++k) {
				const float from[3] = { a_x, y, a_z }, to[3] = { a_x, end, a_z };
				Hit         h{};
				++rays;
				if (!a_ray(from, to, h)) {
					break;
				}
				const bool onSegment = std::isfinite(h.pos[1]) && std::fabs(h.pos[0] - a_x) < 0.05f && std::fabs(h.pos[2] - a_z) < 0.05f &&
				                       (a_down ? (h.pos[1] <= y + 0.01f && h.pos[1] >= end - 0.01f) : (h.pos[1] >= y - 0.01f && h.pos[1] <= end + 0.01f));
				if (!onSegment) {
					++bad;
					y += a_down ? -0.05f : 0.05f;
					continue;
				}
				++hits;
				Event e{};
				e.y = h.pos[1];
				std::memcpy(e.n, h.n, sizeof(e.n));
				e.top = a_down;
				events_.push_back(e);
				y = h.pos[1] + (a_down ? -kObjChainStep : kObjChainStep);
			}
		}

		template <class Ray>
		void ProbeCell(Ray& a_ray, int a_s)
		{
			const int   i = a_s % nu_, j = a_s / nu_;
			const float a = a0_ + (float(i) + 0.5f) * step_, b = b0_ + (float(j) + 0.5f) * step_;
			const float x = o_[0] + u_[0] * a + v_[0] * b, z = o_[1] + u_[1] * a + v_[1] * b;
			events_.clear();
			Chain(a_ray, x, z, true);
			if (!events_.empty()) {
				Chain(a_ray, x, z, false);  // nothing seen from above: nothing there
			}
			std::stable_sort(events_.begin(), events_.end(), [](const Event& p, const Event& q) { return p.y > q.y || (p.y == q.y && p.top && !q.top); });
			unpaired += static_cast<std::uint32_t>(SpansFromEvents(events_, yLo_ + kObjMargin, scratch_));
			spans_.insert(spans_.end(), scratch_.begin(), scratch_.end());
			first_[a_s + 1] = static_cast<std::uint32_t>(spans_.size());
		}

		float                      o_[2]{}, u_[2]{ 1.0f, 0.0f }, v_[2]{ 0.0f, 1.0f };
		float                      a0_ = 0.0f, b0_ = 0.0f, step_ = kObjStep, yLo_ = 0.0f, yHi_ = 0.0f;
		int                        nu_ = 1, nv_ = 1, cursor_ = 0;
		std::vector<std::uint32_t> first_;
		std::vector<Span>          spans_, scratch_;
		std::vector<Event>         events_;
	};
}
