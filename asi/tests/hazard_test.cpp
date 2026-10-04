// Linux unit test for hazard/HazardGrid.h: Minecraft's fire, lava and magma blocks as GTA's peds
// meet them, and how a ped burns.
#include "hazard/HazardGrid.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace lc::hazard;
namespace proto = ::libertycraft::proto;

static int failures = 0;
#define CHECK(cond)                                                               \
	do {                                                                          \
		if (!(cond)) {                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                           \
		}                                                                         \
	} while (0)

static proto::RenLight Light(int a_x, int a_y, int a_z, std::uint8_t a_hazard, std::uint8_t a_kind = proto::kLightFlame)
{
	proto::RenLight l{};
	l.x = static_cast<std::uint8_t>(a_x);
	l.y = static_cast<std::uint8_t>(a_y);
	l.z = static_cast<std::uint8_t>(a_z);
	l.level = 15;
	l.color = 0x203090u | (static_cast<std::uint32_t>(a_kind) << 24) | (static_cast<std::uint32_t>(a_hazard) << 28);
	return l;
}

static void TestGrid()
{
	Grid g;
	CHECK(g.Empty());
	// Section (-1, 0, 2): a torch (no hazard), fire, lava and magma.
	const std::vector<proto::RenLight> lights = { Light(1, 4, 1, proto::kHazardNone), Light(3, 4, 5, proto::kHazardFire), Light(6, 4, 5, proto::kHazardLava, proto::kLightLava),
		Light(9, 3, 5, proto::kHazardMagma, proto::kLightLava) };
	g.SetSection(-1, 0, 2, lights.data(), static_cast<std::uint32_t>(lights.size()));
	CHECK(g.Cells() == 3 && g.Sections() == 1);
	CHECK(g.At(-16 + 1, 4, 32 + 1) == proto::kHazardNone);
	CHECK(g.At(-16 + 3, 4, 32 + 5) == proto::kHazardFire);
	CHECK(g.At(-16 + 6, 4, 32 + 5) == proto::kHazardLava);
	CHECK(g.At(-16 + 9, 3, 32 + 5) == proto::kHazardMagma);
	// The same block of another section isn't it.
	CHECK(g.At(3, 4, 37) == proto::kHazardNone);

	// A ped standing in the fire block, feet a little above its bottom.
	CHECK(g.Touching(-12.5, 4.2, 37.5, 0.3, 1.8) == proto::kHazardFire);
	// Its body brushing the fire from the next block (0.3 m around it reaches in).
	CHECK(g.Touching(-11.8, 4.0, 37.5, 0.3, 1.8) == proto::kHazardFire);
	CHECK(g.Touching(-11.6, 4.0, 37.5, 0.3, 1.8) == proto::kHazardNone);
	// Feet a little below the fire block (GTA's ground under Minecraft's): the body still reaches it.
	CHECK(g.Touching(-12.5, 3.3, 37.5, 0.3, 1.8) == proto::kHazardFire);
	// Lava and fire both touched: lava.
	CHECK(g.Touching(-11.0, 4.0, 37.5, 1.6, 1.8) == proto::kHazardLava);
	// Magma only under the feet.
	CHECK(g.Touching(-6.5, 4.02, 37.5, 0.3, 1.8) == proto::kHazardMagma);
	CHECK(g.Touching(-6.5, 1.0, 37.5, 0.3, 1.8) == proto::kHazardNone);  // walking under it

	// A new list for the section replaces the old one; an empty one clears it.
	const std::vector<proto::RenLight> fewer = { Light(3, 4, 5, proto::kHazardFire) };
	g.SetSection(-1, 0, 2, fewer.data(), 1);
	CHECK(g.Cells() == 1 && g.At(-16 + 6, 4, 32 + 5) == proto::kHazardNone);
	g.SetSection(-1, 0, 2, nullptr, 0);
	CHECK(g.Empty() && g.Sections() == 0);
	g.SetSection(-1, 0, 2, lights.data(), static_cast<std::uint32_t>(lights.size()));
	g.Clear();
	CHECK(g.Empty());
	// Far coordinates don't collide.
	CHECK(BlockKey(1, 2, 3) != BlockKey(-1, 2, 3) && BlockKey(1000, 64, -2000) != BlockKey(-1000, 64, 2000));
}

static void TestBurn()
{
	const Tuning t;
	const float  dt = 0.1f;
	Burn         b;
	// Into the fire: alight at once, hurt 2 Minecraft damage a second (20 GTA health).
	Action a = Step(b, proto::kHazardFire, false, dt, t);
	CHECK(a.ignite && !a.extinguish && !a.done);
	unsigned taken = a.damage;
	bool     burning = true;
	for (int i = 1; i < 10; ++i) {
		a = Step(b, proto::kHazardFire, burning, dt, t);
		CHECK(!a.ignite);
		taken += a.damage;
	}
	CHECK(taken >= 19 && taken <= 21);
	// GTA put it out while still in the fire: alight again, retried once a second while it won't take.
	a = Step(b, proto::kHazardFire, false, dt, t);
	CHECK(a.ignite);
	int steps = 0;
	do {
		a = Step(b, proto::kHazardFire, false, dt, t);
		++steps;
	} while (!a.ignite && steps < 30);
	CHECK(a.ignite && steps >= 9 && steps <= 11);
	// Out of the fire: burns on for the afterburn (GTA's fire does the damage), then put out.
	float out = 0.0f;
	a = Action{};
	while (!a.extinguish && out < 10.0f) {
		a = Step(b, proto::kHazardNone, true, dt, t);
		CHECK(a.damage == 0);
		out += dt;
	}
	CHECK(a.extinguish && a.done);
	CHECK(std::fabs(out - t.fireAfterburn) < 0.25f);

	// Lava: hurts 8 a second (80 GTA health: a ped at 200 with death at 100 goes in about 1.3 s).
	Burn l;
	taken = 0;
	for (int i = 0; i < 10; ++i) {
		a = Step(l, proto::kHazardLava, i > 0, dt, t);
		taken += a.damage;
	}
	CHECK(taken >= 79 && taken <= 81);
	CHECK(l.afterburn >= t.lavaAfterburn - 1e-3f);

	// Magma: hurts, never sets alight, and a ped that only stood on magma isn't put out (it may burn
	// from something else).
	Burn m;
	taken = 0;
	for (int i = 0; i < 10; ++i) {
		a = Step(m, proto::kHazardMagma, true, dt, t);
		CHECK(!a.ignite && !a.extinguish && !a.done);
		taken += a.damage;
	}
	CHECK(taken >= 9 && taken <= 11);
	a = Step(m, proto::kHazardNone, true, dt, t);
	CHECK(a.done && !a.extinguish);
}

static proto::RenLiquid Liquid(int a_x, int a_y, int a_z, std::uint8_t a_kind, int a_surface)
{
	proto::RenLiquid l{};
	l.x = static_cast<std::uint8_t>(a_x);
	l.y = static_cast<std::uint8_t>(a_y);
	l.z = static_cast<std::uint8_t>(a_z);
	l.info = static_cast<std::uint8_t>(a_surface | a_kind << 4);
	return l;
}

static void TestLiquids()
{
	LiquidGrid g;
	// Section (0, 0, 0): a shallow water layer at y 4 (surface 3/15) over x 0-7, a deep pool at x 10
	// (two blocks, full under a 13/15 top), and lava at x 14.
	std::vector<proto::RenLiquid> l;
	for (int x = 0; x < 8; ++x) {
		for (int z = 0; z < 8; ++z) {
			l.push_back(Liquid(x, 4, z, proto::kLiquidWater, 3));
		}
	}
	l.push_back(Liquid(10, 4, 2, proto::kLiquidWater, 15));
	l.push_back(Liquid(10, 5, 2, proto::kLiquidWater, 13));
	l.push_back(Liquid(14, 4, 2, proto::kLiquidLava, 13));
	g.SetSection(0, 0, 0, l.data(), static_cast<std::uint32_t>(l.size()));
	CHECK(g.Cells() == 67);
	std::uint8_t kind = 0;
	// A wheel on the ground at y 4.0: 0.2 m of water.
	CHECK(std::fabs(g.DepthAt(2.5, 4.0, 2.5, kind) - 0.2f) < 1e-4f && kind == proto::kLiquidWater);
	// GTA's ground a little above Minecraft's block bottom: drawn from that ground up, a fifth of the
	// rest of the block (WorldExporter), so 0.18 m over it.
	CHECK(std::fabs(g.DepthAt(2.5, 4.1, 2.5, kind) - 0.18f) < 1e-4f);
	CHECK(std::fabs(g.DepthAt(2.5, 4.5, 2.5, kind) - 0.1f) < 1e-4f);
	// Above the block: dry.
	CHECK(g.DepthAt(2.5, 5.2, 2.5, kind) == 0.0f && kind == proto::kLiquidNone);
	// The deep pool: 1 + 13/15.
	CHECK(std::fabs(g.DepthAt(10.5, 4.0, 2.5, kind) - (1.0f + 13.0f / 15.0f)) < 1e-4f);
	CHECK(std::fabs(g.DepthAt(14.5, 4.0, 2.5, kind) - 13.0f / 15.0f) < 1e-4f && kind == proto::kLiquidLava);
	// Water far below the wheels (a fall under the road) doesn't count.
	CHECK(g.DepthAt(2.5, 6.0, 2.5, kind) == 0.0f);
	// The ground in the block below the liquid: the full height over the underside.
	CHECK(std::fabs(g.DepthAt(2.5, 3.8, 2.5, kind) - 0.4f) < 1e-4f);

	// A car (4 x 2 m) on the shallow layer, facing north (GTA +y = Minecraft -z).
	Footprint f;
	f.pos[0] = 4.0, f.pos[1] = -4.0, f.pos[2] = 4.6;
	f.lo[0] = -1.0f, f.hi[0] = 1.0f, f.lo[1] = -2.0f, f.hi[1] = 2.0f, f.lo[2] = -0.6f, f.hi[2] = 0.9f;
	Grid       none;
	const auto c = Touch(none, g, f);
	CHECK(c.liquid == proto::kLiquidWater && std::fabs(c.depth - 0.2f) < 1e-3f && c.wet > 0.99f);
	// Its body higher up (the box bottom 0.3 m over the road): the depth still counts from the road.
	f.pos[2] = 4.9;
	f.ground = 4.0;
	CHECK(std::fabs(Touch(none, g, f).depth - 0.2f) < 1e-3f);
	f.ground = std::nan("");
	CHECK(Touch(none, g, f).depth < 0.15f);  // (from the body: shallower than it is)
	f.pos[2] = 4.6;
	CHECK(c.hazard == proto::kHazardNone);
	// Half on it: half as wet.
	f.pos[0] = 8.0;
	const auto half = Touch(none, g, f);
	CHECK(half.wet > 0.2f && half.wet < 0.8f && half.depth < 0.2f);
	// Fire under the front wheels.
	Grid                           fire;
	const std::vector<proto::RenLight> lights = { Light(4, 4, 6, proto::kHazardFire), Light(3, 4, 6, proto::kHazardFire), Light(5, 4, 6, proto::kHazardFire) };
	fire.SetSection(0, 0, 0, lights.data(), 3);
	f.pos[0] = 4.0;
	f.pos[1] = -2.0;  // over z 0 to 4: clear of it
	CHECK(Touch(fire, LiquidGrid{}, f).hazard == proto::kHazardNone);
	f.pos[1] = -8.0;
	CHECK(Touch(fire, LiquidGrid{}, f).hazard == proto::kHazardFire);
}

static void TestWade()
{
	CHECK(WadeSpeed(0.0f) == 1.0f && WadeSpeed(0.1f) == 1.0f);
	CHECK(WadeSpeed(0.5f) < 0.9f && WadeSpeed(0.5f) > WadeSpeed(1.0f));
	CHECK(std::fabs(WadeSpeed(1.0f) - 0.45f) < 0.01f);
	CHECK(WadeSpeed(1.7f) == 0.35f);
}

static void TestWaterColumns()
{
	LiquidGrid g;
	// Section (0, 0, -1): a pool two blocks deep at x 3, z -5 (y 4 full, y 5 13/15), one shallow block
	// at x 4, lava at x 6 (not water), and a second run high up in the same column as the pool.
	std::vector<proto::RenLiquid> l = { Liquid(3, 4, 11, proto::kLiquidWater, 15), Liquid(3, 5, 11, proto::kLiquidWater, 13),
		Liquid(4, 4, 11, proto::kLiquidWater, 3), Liquid(6, 4, 11, proto::kLiquidLava, 13), Liquid(3, 12, 11, proto::kLiquidWater, 15) };
	g.SetSection(0, 0, -1, l.data(), static_cast<std::uint32_t>(l.size()));
	WaterColumns w;
	w.Build(g);
	CHECK(!w.Empty());
	float s = 0.0f;
	// GTA (x, y) = Minecraft (x, -z): the pool's column is GTA x 3..4, y 4..5 (Minecraft z -5).
	CHECK(w.Surface(3.5f, 4.5f, 4.2f, s) && std::fabs(s - (5.0f + 13.0f / 15.0f)) < 1e-4f);
	CHECK(w.Surface(3.5f, 4.5f, 7.0f, s) && std::fabs(s - (5.0f + 13.0f / 15.0f)) < 1e-4f);  // above it
	CHECK(w.Surface(3.5f, 4.5f, 12.5f, s) && std::fabs(s - 13.0f) < 1e-4f);                // the high run
	CHECK(!w.Surface(3.5f, 4.5f, -3.0f, s));                                                 // far under
	CHECK(w.Surface(4.5f, 4.5f, 4.0f, s) && std::fabs(s - 4.2f) < 1e-4f);
	CHECK(!w.Surface(6.5f, 4.5f, 4.0f, s));  // lava isn't water
	CHECK(!w.Surface(5.5f, 4.5f, 4.0f, s));  // dry column
	CHECK(!w.Surface(3.5f, -4.5f, 4.0f, s));  // (the mirror image: Minecraft z +5)
}

// A car under full throttle (6 m/s2 a second from GTA) for 15 s at 60 frames a second.
static float TopUnderThrottle(float a_depth, std::uint8_t a_kind, float a_start = 0.0f)
{
	float v = a_start, last = -1.0f;
	for (int i = 0; i < 900; ++i) {
		v = std::min(v + 6.0f / 60.0f, 40.0f);  // GTA's engine (and its own top speed)
		v = Drag(v, last, a_depth, a_kind, 1.0f / 60.0f);
		last = v;
	}
	return v;
}

static void TestDrag()
{
	// Dry: nothing changes.
	CHECK(Drag(12.0f, 11.0f, 0.0f, proto::kLiquidWater, 0.016f) == 12.0f);
	CHECK(Drag(12.0f, 11.0f, 0.5f, proto::kLiquidNone, 0.016f) == 12.0f);
	// Deeper is slower; lava slower than water; deep water is a crawl.
	const float shallow = TopUnderThrottle(0.2f, proto::kLiquidWater), mid = TopUnderThrottle(0.5f, proto::kLiquidWater),
				deep = TopUnderThrottle(0.9f, proto::kLiquidWater), lava = TopUnderThrottle(0.2f, proto::kLiquidLava);
	std::printf("top speeds under throttle: shallow water %.1f, mid %.1f, deep %.1f m/s, shallow lava %.1f m/s\n", shallow, mid, deep, lava);
	CHECK(shallow > 6.0f && shallow < 12.0f);
	CHECK(mid > 2.5f && mid < shallow);
	CHECK(deep < 2.0f && deep > 0.5f);
	CHECK(lava < shallow);
	// Sluggish: from standstill, a second of throttle gains far less than dry (6 m/s).
	float v = 0.0f, last = -1.0f;
	for (int i = 0; i < 60; ++i) {
		v = Drag(v + 0.1f, last, 0.2f, proto::kLiquidWater, 1.0f / 60.0f);
		last = v;
	}
	CHECK(v > 1.0f && v < 3.5f);
	// Into water at 25 m/s: eased down, not stopped dead in a frame.
	const float one = Drag(25.0f, 25.0f, 0.5f, proto::kLiquidWater, 1.0f / 60.0f);
	CHECK(one < 25.0f && one > 22.0f);
	CHECK(TopUnderThrottle(0.5f, proto::kLiquidWater, 25.0f) < mid + 0.5f);
}

static void TestStall()
{
	const float dt = 1.0f / 60.0f;
	const StallTuning t{};
	Stall       s{};
	// Shallow water: never.
	for (int i = 0; i < 600; ++i) {
		CHECK(StallStep(s, true, 0.8f, dt, t) == StallEvent::kNone);
	}
	// Deep: runs on for t.after seconds, then dies once, then held off.
	CHECK(StallStep(s, true, 1.2f, dt, t) == StallEvent::kDeep);
	int frames = 1, stalledAt = -1;
	for (; frames < 600 && stalledAt < 0; ++frames) {
		if (StallStep(s, true, 1.2f, dt, t) == StallEvent::kStalls) {
			stalledAt = frames;
		}
	}
	CHECK(stalledAt > 0 && std::fabs(stalledAt * dt - t.after) < 2.0f * dt);
	CHECK(StallStep(s, true, 1.5f, dt, t) == StallEvent::kOff && s.off);
	// Out (shallower, or out of the water): starts again, once.
	CHECK(StallStep(s, true, 0.6f, dt, t) == StallEvent::kRestarts && !s.off);
	CHECK(StallStep(s, false, 0.0f, dt, t) == StallEvent::kNone);
	// A dip shorter than t.after, out, and in again: the clock starts over.
	Stall d{};
	for (int i = 0; i < static_cast<int>((t.after - 1.0f) / dt); ++i) {
		CHECK(StallStep(d, true, 1.2f, dt, t) != StallEvent::kStalls);
	}
	CHECK(StallStep(d, false, 0.0f, dt, t) == StallEvent::kNone && d.under == 0.0f);
	for (int i = 0; i < static_cast<int>((t.after - 1.0f) / dt); ++i) {
		CHECK(StallStep(d, true, 1.2f, dt, t) != StallEvent::kStalls);
	}
	CHECK(!d.off);
	// Lava is not water: no stall (it burns instead).
	Stall l{};
	for (int i = 0; i < 600; ++i) {
		CHECK(StallStep(l, false, 2.0f, dt, t) == StallEvent::kNone);
	}
}

int main()
{
	TestGrid();
	TestBurn();
	TestLiquids();
	TestDrag();
	TestWaterColumns();
	TestWade();
	TestStall();
	if (failures) {
		std::fprintf(stderr, "hazard_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("hazard_test: all passed\n");
	return 0;
}
