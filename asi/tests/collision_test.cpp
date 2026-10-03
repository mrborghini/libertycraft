// Linux unit test for collision/Geometry.h: the v2 sampler and builder against a synthetic world
// probed by a one-sided ray caster (like CWorld::ProcessLineOfSight: only faces turned towards
// the ray are hit). Scenes: an awning over a street next to a building (the in-game regression:
// no pillar under the awning), a room inside a building, an overpass, stairs.
#include "collision/Geometry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace lc::col;

static int failures = 0;
#define CHECK(cond)                                                               \
	do {                                                                          \
		if (!(cond)) {                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                           \
		}                                                                         \
	} while (0)

struct STri
{
	float a[3], b[3], c[3], n[3];
};

struct Scene
{
	std::vector<STri> tris;
	long              rays = 0;

	void Add(const float* a, const float* b, const float* c)
	{
		STri t{};
		for (int k = 0; k < 3; ++k) {
			t.a[k] = a[k];
			t.b[k] = b[k];
			t.c[k] = c[k];
		}
		const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		Cross(e1, e2, t.n);
		const float l = std::sqrt(Dot(t.n, t.n));
		for (float& v : t.n) {
			v /= l;
		}
		tris.push_back(t);
	}

	// Quad with corners p0..p3, facing a_n (wound accordingly).
	void Quad(const float* p0, const float* p1, const float* p2, const float* p3, const float* a_n)
	{
		const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
		float       n[3];
		Cross(e1, e2, n);
		if (Dot(n, a_n) >= 0) {
			Add(p0, p1, p2);
			Add(p0, p2, p3);
		} else {
			Add(p0, p2, p1);
			Add(p0, p3, p2);
		}
	}

	void Horizontal(float x0, float z0, float x1, float z1, float y, bool up)
	{
		const float p0[3] = { x0, y, z0 }, p1[3] = { x1, y, z0 }, p2[3] = { x1, y, z1 }, p3[3] = { x0, y, z1 };
		const float n[3] = { 0, up ? 1.0f : -1.0f, 0 };
		Quad(p0, p1, p2, p3, n);
	}

	// Axis-aligned box; a_out: faces point out (a solid) or in (a room). a_bottom: has a bottom face.
	void Box(const float* lo, const float* hi, bool a_out, bool a_bottom = true, bool a_top = true)
	{
		const float s = a_out ? 1.0f : -1.0f;
		auto        face = [&](int axis, bool high) {
            const int   u = (axis + 1) % 3, v = (axis + 2) % 3;
            const float w = high ? hi[axis] : lo[axis];
            float       p[4][3];
            const float us[4] = { lo[u], hi[u], hi[u], lo[u] }, vs[4] = { lo[v], lo[v], hi[v], hi[v] };
            for (int k = 0; k < 4; ++k) {
                p[k][axis] = w;
                p[k][u] = us[k];
                p[k][v] = vs[k];
            }
            float n[3] = { 0, 0, 0 };
            n[axis] = (high ? 1.0f : -1.0f) * s;
            Quad(p[0], p[1], p[2], p[3], n);
		};
		face(0, false);
		face(0, true);
		face(2, false);
		face(2, true);
		if (a_top) {
			face(1, true);
		}
		if (a_bottom) {
			face(1, false);
		}
	}

	// Möller-Trumbore, culling faces that don't face the ray.
	bool Cast(const float* from, const float* to, Hit& out)
	{
		++rays;
		const float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
		float       best = 2.0f;
		const STri* hit = nullptr;
		for (const auto& t : tris) {
			if (Dot(t.n, d) >= 0.0f) {
				continue;
			}
			const float e1[3] = { t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2] }, e2[3] = { t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2] };
			float       p[3];
			Cross(d, e2, p);
			const float det = Dot(e1, p);
			if (std::fabs(det) < 1e-12f) {
				continue;
			}
			const float inv = 1.0f / det;
			const float s[3] = { from[0] - t.a[0], from[1] - t.a[1], from[2] - t.a[2] };
			const float u = Dot(s, p) * inv;
			if (u < 0.0f || u > 1.0f) {
				continue;
			}
			float q[3];
			Cross(s, e1, q);
			const float v = Dot(d, q) * inv;
			if (v < 0.0f || u + v > 1.0f) {
				continue;
			}
			const float tt = Dot(e2, q) * inv;
			if (tt >= 0.0f && tt <= 1.0f && tt < best) {
				best = tt;
				hit = &t;
			}
		}
		if (!hit) {
			return false;
		}
		for (int k = 0; k < 3; ++k) {
			out.pos[k] = from[k] + d[k] * best;
			out.n[k] = hit->n[k];
		}
		return true;
	}
};

struct Built
{
	std::shared_ptr<Column> col;
	std::vector<Tri>        tris;
	ColumnProbe*            probe = nullptr;
	std::uint32_t           vRays = 0, wRays = 0, bad = 0;
};

static Built Probe(Scene& a_scene, int a_rx, int a_rz, float a_yLo, float a_yHi)
{
	ColumnProbe probe(a_rx, a_rz, a_yLo, a_yHi);
	int         slices = 0;
	int         budget = 0;
	// resumable: stop every 50 probes
	while (!probe.Run([&](const float* f, const float* t, Hit& h) { return a_scene.Cast(f, t, h); }, [&] { return ++budget % 50 == 0; })) {
		++slices;
	}
	Built b;
	b.col = probe.Result();
	b.vRays = probe.verticalRays;
	b.wRays = probe.wallRays;
	b.bad = probe.badHits;
	BuildColumn(*b.col, b.tris);
	std::printf("  column (%d,%d): %u vertical + %u wall probes (%d slices), %zu events, %zu wall queries, %zu triangles, %u bad\n", a_rx, a_rz, b.vRays,
		b.wRays, slices, b.col->events.size(), b.col->walls.size(), b.tris.size(), b.bad);
	return b;
}

// Any triangle of a kind overlapping the box?
static int TrisIn(const std::vector<Tri>& a_tris, const float* lo, const float* hi, int a_kind = -1)
{
	int n = 0;
	for (const auto& t : a_tris) {
		if (a_kind >= 0 && t.kind != a_kind) {
			continue;
		}
		float tlo[3], thi[3];
		TriBounds(t, tlo, thi);
		if (thi[0] > lo[0] && tlo[0] < hi[0] && thi[1] > lo[1] && tlo[1] < hi[1] && thi[2] > lo[2] && tlo[2] < hi[2]) {
			++n;
		}
	}
	return n;
}

// Solid voxels inside the box (MC coords) over the column's regions ry0..ry1.
static int VoxelsIn(const Built& a_b, int ry0, int ry1, const float* lo, const float* hi)
{
	int                          n = 0;
	std::vector<Tri>             rt;
	std::vector<proto::ColBlock> blocks;
	for (int ry = ry0; ry <= ry1; ++ry) {
		RegionTris(a_b.tris, a_b.col->rx, ry, a_b.col->rz, rt);
		Voxelize(rt, a_b.col->rx, ry, a_b.col->rz, blocks);
		for (const auto& blk : blocks) {
			for (int sy = 0; sy < 8; ++sy) {
				for (int bit = 0; bit < 64; ++bit) {
					if (!((blk.bits[sy] >> bit) & 1)) {
						continue;
					}
					const float x = blk.x + ((bit & 7) + 0.5f) / 8.0f, y = blk.y + (sy + 0.5f) / 8.0f, z = blk.z + ((bit >> 3) + 0.5f) / 8.0f;
					if (x > lo[0] && x < hi[0] && y > lo[1] && y < hi[1] && z > lo[2] && z < hi[2]) {
						++n;
					}
				}
			}
		}
	}
	return n;
}

// The wall triangle nearest to a point (horizontal distance from its plane), facing a_n.
static bool WallAt(const std::vector<Tri>& a_tris, float x, float y, float z, float nx, float nz, float tol)
{
	for (const auto& t : a_tris) {
		if (t.kind != kWall) {
			continue;
		}
		const float e1[3] = { t.v[3] - t.v[0], t.v[4] - t.v[1], t.v[5] - t.v[2] }, e2[3] = { t.v[6] - t.v[0], t.v[7] - t.v[1], t.v[8] - t.v[2] };
		float       n[3];
		Cross(e1, e2, n);
		const float l = std::sqrt(Dot(n, n));
		if (n[0] / l * nx + n[2] / l * nz < 0.9f) {
			continue;
		}
		float lo[3], hi[3];
		TriBounds(t, lo, hi);
		if (y < lo[1] || y > hi[1]) {
			continue;
		}
		// distance of (x, z) to the wall's plane and within its lateral extent
		const float d = ((x - t.v[0]) * n[0] + (z - t.v[2]) * n[2]) / l;
		if (std::fabs(d) <= tol && x >= lo[0] - tol && x <= hi[0] + tol && z >= lo[2] - tol && z <= hi[2] + tol) {
			return true;
		}
	}
	return false;
}

static float FloorAt(const std::vector<Tri>& a_tris, float x, float z, float below)
{
	float best = -1e9f;
	for (const auto& t : a_tris) {
		if (t.kind != kFloor) {
			continue;
		}
		const float* a = t.v;
		const float* b = t.v + 3;
		const float* c = t.v + 6;
		const float  den = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
		if (std::fabs(den) < 1e-9f) {
			continue;
		}
		const float l0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / den;
		const float l1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / den;
		const float l2 = 1.0f - l0 - l1;
		if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) {
			continue;
		}
		const float h = l0 * a[1] + l1 * b[1] + l2 * c[1];
		if (h <= below && h > best) {
			best = h;
		}
	}
	return best;
}

static void TestAwningAndBuilding()
{
	std::puts("awning + building (in-game regression spot, MC 901.77 14.54 475.26)");
	Scene s;
	const float street = 14.54f;
	s.Horizontal(880, 460, 920, 490, street, true);
	s.Horizontal(901.0f, 472.0f, 903.3f, 480.0f, 17.0f, true);  // awning: a top face only
	const float blo[3] = { 897.3f, street - 1.0f, 474.2f }, bhi[3] = { 899.8f, 27.0f, 478.6f };
	s.Box(blo, bhi, true, false);  // a building without a bottom face
	auto b = Probe(s, 112, 59, 0.0f, 40.0f);
	CHECK(b.bad == 0);

	// no pillar under the awning: nothing solid between the street and the awning
	const float lo[3] = { 901.1f, street + 0.06f, 472.1f }, hi[3] = { 903.2f, 16.85f, 479.9f };  // the awning's own voxel is [16.875, 17]
	CHECK(TrisIn(b.tris, lo, hi) == 0);
	CHECK(VoxelsIn(b, 1, 2, lo, hi) == 0);
	// ...and the awning itself is there
	const float alo[3] = { 901.6f, 16.9f, 473.0f }, ahi[3] = { 902.7f, 17.1f, 479.0f };
	CHECK(TrisIn(b.tris, alo, ahi, kFloor) > 0);
	// the street everywhere outside the building, at its height
	for (float x = 896.1f; x < 904.0f; x += 0.37f) {
		for (float z = 472.1f; z < 480.0f; z += 0.41f) {
			const bool inside = x > blo[0] && x < bhi[0] && z > blo[2] && z < bhi[2];
			if (!inside) {
				const float f = FloorAt(b.tris, x, z, street + 1.0f);
				if (std::fabs(f - street) > 0.01f) {
					std::printf("  street floor at %.2f %.2f: %.3f\n", x, z, f);
				}
				CHECK(std::fabs(f - street) <= 0.01f);
			}
		}
	}
	// the facade, right where it is, from the street to at least head height
	for (float y : { street + 0.7f, street + 1.5f, street + 2.5f }) {
		for (float z = 474.6f; z < 478.3f; z += 0.3f) {
			CHECK(WallAt(b.tris, bhi[0], y, z, 1.0f, 0.0f, 0.03f));
			CHECK(WallAt(b.tris, blo[0], y, z, -1.0f, 0.0f, 0.03f));
		}
		for (float x = 897.6f; x < 899.6f; x += 0.3f) {
			CHECK(WallAt(b.tris, x, y, blo[2], 0.0f, -1.0f, 0.03f));
			CHECK(WallAt(b.tris, x, y, bhi[2], 0.0f, 1.0f, 0.03f));
		}
	}
	// the roof is a floor
	CHECK(std::fabs(FloorAt(b.tris, 898.5f, 476.0f, 30.0f) - 27.0f) < 0.01f);
	// ground voxels: filled 2 blocks down under the street
	const float glo[3] = { 902.0f, street - 1.9f, 473.0f }, ghi[3] = { 902.5f, street, 473.5f };
	CHECK(VoxelsIn(b, 1, 2, glo, ghi) == 4 * 4 * 15);
	// facade voxels exist at the wall
	const float wlo[3] = { 899.7f, street + 0.5f, 475.0f }, whi[3] = { 899.95f, street + 2.0f, 478.0f };
	CHECK(VoxelsIn(b, 1, 2, wlo, whi) > 0);
}

static void TestRoomInBuilding()
{
	std::puts("room inside a building");
	Scene s;
	s.Horizontal(60, 60, 110, 110, 0.0f, true);
	const float hlo[3] = { 78, -1, 78 }, hhi[3] = { 92, 30, 92 };
	s.Box(hlo, hhi, true, false);
	const float rlo[3] = { 81.2f, 3.0f, 81.4f }, rhi[3] = { 86.7f, 5.8f, 86.1f };
	s.Box(rlo, rhi, false);  // faces inward: floor up, ceiling down, walls facing in
	auto b = Probe(s, 10, 10, -8.0f, 24.0f);
	// floors in the room at 3
	for (float x = 81.3f; x < 86.6f; x += 0.29f) {
		for (float z = 81.5f; z < 86.0f; z += 0.31f) {
			const float f = FloorAt(b.tris, x, z, 4.0f);
			if (std::fabs(f - 3.0f) >= 0.01f) {
				std::printf("  room floor at %.2f %.2f: %.3f\n", x, z, f);
			}
			CHECK(std::fabs(f - 3.0f) < 0.01f);
		}
	}
	// its walls, facing in
	for (float z = 81.7f; z < 85.9f; z += 0.3f) {
		CHECK(WallAt(b.tris, rlo[0], 4.0f, z, 1.0f, 0.0f, 0.03f));
		CHECK(WallAt(b.tris, rhi[0], 4.0f, z, -1.0f, 0.0f, 0.03f));
	}
	for (float x = 81.5f; x < 86.5f; x += 0.3f) {
		CHECK(WallAt(b.tris, x, 4.0f, rlo[2], 0.0f, 1.0f, 0.03f));
		CHECK(WallAt(b.tris, x, 4.0f, rhi[2], 0.0f, -1.0f, 0.03f));
	}
	// ceiling triangles at 5.8 and no solid in the room
	const float clo[3] = { 82.0f, 5.7f, 82.0f }, chi[3] = { 86.0f, 5.9f, 85.5f };
	CHECK(TrisIn(b.tris, clo, chi, kCeiling) > 0);
	const float in_lo[3] = { 81.5f, 3.1f, 81.7f }, in_hi[3] = { 86.4f, 5.6f, 85.8f };
	CHECK(TrisIn(b.tris, in_lo, in_hi) == 0);
	CHECK(VoxelsIn(b, 0, 0, in_lo, in_hi) == 0);
}

static void TestOverpass()
{
	std::puts("overpass");
	Scene s;
	s.Horizontal(-20, -20, 30, 30, 0.0f, true);
	const float dlo[3] = { -10, 9, 2 }, dhi[3] = { 20, 10, 5 };
	s.Box(dlo, dhi, true);
	auto b = Probe(s, 0, 0, -8.0f, 24.0f);
	const float ulo[3] = { 0.1f, 0.05f, 2.1f }, uhi[3] = { 7.9f, 8.95f, 4.9f };
	CHECK(TrisIn(b.tris, ulo, uhi) == 0);
	CHECK(VoxelsIn(b, 0, 1, ulo, uhi) == 0);
	CHECK(std::fabs(FloorAt(b.tris, 4.0f, 3.0f, 9.5f) - 0.0f) < 0.01f);
	CHECK(std::fabs(FloorAt(b.tris, 4.0f, 3.0f, 11.0f) - 10.0f) < 0.01f);
	const float clo[3] = { 1.0f, 8.9f, 2.5f }, chi[3] = { 7.0f, 9.1f, 4.5f };
	CHECK(TrisIn(b.tris, clo, chi, kCeiling) > 0);
	// the deck's sides are walls for someone standing on it? (no: it's a ledge, nobody stands beside it)
}

static void TestStairs()
{
	std::puts("stairs");
	Scene s;
	s.Horizontal(-10, 10, 0.001f, 30, 0.0f, true);
	for (int k = 0; k < 20; ++k) {
		const float lo[3] = { k * 0.28f, -1.0f, 10.0f }, hi[3] = { k < 19 ? (k + 1) * 0.28f : 20.0f, 0.18f * (k + 1), 30.0f };
		s.Box(lo, hi, true, false);
	}
	auto b = Probe(s, 0, 2, -8.0f, 24.0f);
	int  walls = 0;
	for (const auto& t : b.tris) {
		float lo[3], hi[3];
		TriBounds(t, lo, hi);
		if (t.kind == kWall && hi[1] > lo[1] + 0.3f && lo[2] > 16.5f && hi[2] < 23.5f) {
			++walls;
		}
	}
	CHECK(walls == 0);  // no wall across the stairs
	for (float x = 0.1f; x < 5.5f; x += 0.13f) {
		const float f = FloorAt(b.tris, x, 20.0f, 10.0f);
		const float want = 0.18f * (std::floor(x / 0.28f) + 1.0f);
		CHECK(std::fabs(f - want) < 0.4f);
	}
}

int main()
{
	TestAwningAndBuilding();
	TestRoomInBuilding();
	TestOverpass();
	TestStairs();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::puts("collision_test: all checks passed");
	return 0;
}
