// Linux unit test for combat/CombatMath.h (actor ids, damage scaling, weapon classes, hurt pacing,
// knockback, vehicles) and combat/BlockPush.h (Minecraft blocks stopping peds and vehicles).
#include "combat/BlockPush.h"
#include "combat/CombatMath.h"

#include <vector>

#include <cmath>
#include <cstdio>

using namespace lc::combat;

static int failures = 0;
#define CHECK(cond)                                                               \
	do {                                                                          \
		if (!(cond)) {                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                           \
		}                                                                         \
	} while (0)

static bool Near(float a_a, float a_b, float a_eps = 1e-4f) { return std::fabs(a_a - a_b) <= a_eps; }

static void TestActorIds()
{
	std::uint32_t h = 0;
	CHECK(ActorIdFromHandle(0) == 0x4C000000u);  // never 0, even for handle 0
	CHECK(HandleFromActorId(ActorIdFromHandle(0x1234), h) && h == 0x1234);
	CHECK(HandleFromActorId(ActorIdFromHandle(0), h) && h == 0);
	CHECK(HandleFromActorId(ActorIdFromHandle(0xFFFFFF), h) && h == 0xFFFFFF);
	CHECK(!HandleFromActorId(0, h));
	CHECK(!HandleFromActorId(0x00001234u, h));  // a Skyrim-style form id isn't ours
	CHECK(!HandleFromActorId(0xFF001234u, h));
}

static void TestWeapons()
{
	CHECK(ClassifyWeapon(kWeaponUnarmed) == HurtClass::kMelee);
	CHECK(ClassifyWeapon(kWeaponBat) == HurtClass::kMelee);
	CHECK(ClassifyWeapon(kWeaponKnife) == HurtClass::kMelee);
	CHECK(ClassifyWeapon(kWeaponAnyMelee) == HurtClass::kMelee);
	CHECK(ClassifyWeapon(kWeaponPistol) == HurtClass::kProjectile);
	CHECK(ClassifyWeapon(14) == HurtClass::kProjectile);  // AK-47
	CHECK(ClassifyWeapon(kWeaponSniperM40A1) == HurtClass::kProjectile);
	CHECK(ClassifyWeapon(kWeaponMinigun) == HurtClass::kProjectile);
	CHECK(ClassifyWeapon(kWeaponUziDriveby) == HurtClass::kProjectile);
	CHECK(ClassifyWeapon(kWeaponGrenade) == HurtClass::kOther);
	CHECK(ClassifyWeapon(kWeaponMolotov) == HurtClass::kOther);
	CHECK(ClassifyWeapon(kWeaponRocketLauncher) == HurtClass::kOther);
	CHECK(ClassifyWeapon(kWeaponExplosion) == HurtClass::kOther);
	CHECK(ClassifyWeapon(kWeaponRunOverByCar) == HurtClass::kOther);
	CHECK(ClassifyWeapon(kWeaponFall) == HurtClass::kIgnore);
	CHECK(ClassifyWeapon(kWeaponDrowning) == HurtClass::kIgnore);
	CHECK(ClassifyWeapon(-1) == HurtClass::kOther);
	CHECK(HurtKindOf(HurtClass::kMelee) == proto::kHurtMelee);
	CHECK(HurtKindOf(HurtClass::kProjectile) == proto::kHurtProjectile);
	CHECK(HurtKindOf(HurtClass::kOther) == proto::kHurtOther);
}

static void TestDamage()
{
	CHECK(PedDamageFromMc(7.0f, 10.0f) == 70);  // diamond sword
	CHECK(PedDamageFromMc(1.0f, 10.0f) == 10);  // fist
	CHECK(PedDamageFromMc(0.01f, 10.0f) == 1);  // something always lands
	CHECK(PedDamageFromMc(0.0f, 10.0f) == 0);
	CHECK(PedDamageFromMc(-3.0f, 10.0f) == 0);
	CHECK(PedDamageFromMc(5.0f, 0.0f) == 0);
	CHECK(PedDamageFromMc(NAN, 10.0f) == 0);
	// GTA 20 -> MC 2 (one heart) -> kInHurt "host damage" 10 (Java divides by 5).
	CHECK(Near(McDamageFromGta(20.0f, 10.0f), 2.0f));
	CHECK(Near(HostDamageForMc(McDamageFromGta(20.0f, 10.0f)), 10.0f));
	CHECK(Near(HostDamageForMc(McDamageFromGta(20.0f, 10.0f)) / kJavaHostToMcDamage, 2.0f));
	CHECK(McDamageFromGta(20.0f, 0.0f) == 0.0f);
	CHECK(McDamageFromGta(-1.0f, 10.0f) == 0.0f);
}

static void TestHealth()
{
	CHECK(Near(HealthFraction(200, 200, 100), 1.0f));
	CHECK(Near(HealthFraction(150, 200, 100), 0.5f));
	CHECK(Near(HealthFraction(100, 200, 100), 0.0f));
	CHECK(Near(HealthFraction(0, 200, 100), 0.0f));
	CHECK(Near(HealthFraction(1000, 200, 100), 1.0f));
	CHECK(Near(HealthFraction(150, 100, 100), 1.0f));  // no span: alive = full
	CHECK(Near(Deficit(1000, 980, 50, 50), 20.0f));
	CHECK(Near(Deficit(1000, 990, 50, 40), 20.0f));
	CHECK(Near(Deficit(1000, 1010, 50, 60), 0.0f));  // gains don't count
}

static void TestKnockback()
{
	float x = 0, y = 0;
	CHECK(PushDirToGta(0.0f, 1.0f, x, y) && Near(x, 0.0f) && Near(y, -1.0f));  // MC south = GTA -y
	CHECK(PushDirToGta(3.0f, 0.0f, x, y) && Near(x, 1.0f) && Near(y, 0.0f));
	CHECK(!PushDirToGta(0.0f, 0.0f, x, y));
	CHECK(RagdollMs(0.0f, false) == 0);
	CHECK(RagdollMs(0.4f, false) == 800);
	CHECK(RagdollMs(0.4f, true) == 1200);
	CHECK(RagdollMs(0.0f, true) == 1200);
	CHECK(RagdollMs(0.1f, false) == 500);
	CHECK(RagdollMs(5.0f, false) == 3000);
	CHECK(Near(ExplosionRadius(4.0f, 1.0f), 4.0f));
	CHECK(Near(ExplosionRadius(4.0f, 100.0f), 30.0f));
	CHECK(ExplosionRadius(0.0f, 1.0f) == 0.0f);
	CHECK(Near(ExplosionShake(4.0f, 2.0f), 1.0f));
	CHECK(Near(ExplosionShake(4.0f, 24.0f), 0.0f));
	CHECK(Near(ExplosionShake(4.0f, 14.0f), 0.5f));
}

// Minecraft's convention end to end: LivingEntity.knockback(power, xd, zd) moves the victim along
// -(xd, zd), where (xd, zd) = attacker - victim; HostActorEntity sends that motion direction as b/c.
static void TestKnockbackChain()
{
	// The attacker stands at MC (0, 0), the victim 5 blocks south of it at MC (0, 5): Minecraft calls
	// knockback(0.4, 0 - 0, 0 - 5) and the Java side sends -(xd, zd) normalised = (0, 1).
	const float xd = 0.0f - 0.0f, zd = 0.0f - 5.0f;
	const float len = std::sqrt(xd * xd + zd * zd);
	const float b = -xd / len, c = -zd / len;
	float gx = 0, gy = 0;
	CHECK(PushDirToGta(b, c, gx, gy));
	// In GTA the victim is at (0, -5) from the attacker (MC south = GTA -y): away from the attacker is -y.
	CHECK(Near(gx, 0.0f) && Near(gy, -1.0f));
	// The ped's own frame: facing north (heading 0) the push is straight back; facing west (90)
	// a push west is straight ahead, and a push north is to its right.
	float lx = 0, ly = 0;
	WorldToPedLocal(0.0f, -1.0f, 0.0f, lx, ly);
	CHECK(Near(lx, 0.0f) && Near(ly, -1.0f));
	WorldToPedLocal(-1.0f, 0.0f, 90.0f, lx, ly);
	CHECK(Near(lx, 0.0f) && Near(ly, 1.0f));
	WorldToPedLocal(0.0f, 1.0f, 90.0f, lx, ly);
	CHECK(Near(lx, 1.0f) && Near(ly, 0.0f));
	// Round trip for any heading: local -> world with right = (cos h, sin h), forward = (-sin h, cos h).
	for (float h = 0.0f; h < 360.0f; h += 37.0f) {
		const float wx = 0.6f, wy = -0.8f;
		WorldToPedLocal(wx, wy, h, lx, ly);
		const float r = h * 3.14159265f / 180.0f;
		CHECK(Near(lx * std::cos(r) + ly * -std::sin(r), wx, 1e-4f) && Near(lx * std::sin(r) + ly * std::cos(r), wy, 1e-4f));
	}
	CHECK(Near(AngleBetween(1, 0, 0, 1), 90.0f, 1e-3f));
	CHECK(Near(AngleBetween(1, 0, -2, 0), 180.0f, 1e-3f));
	CHECK(Near(AngleBetween(1, 1, 2, 2), 0.0f, 1e-2f));
}

static void TestVehicles()
{
	std::uint32_t h = 0, piece = 0;
	CHECK(VehicleActorId(0x1234, 2) == 0x560048D2u);
	CHECK(VehicleFromActorId(VehicleActorId(0x1234, 2), h, piece) && h == 0x1234 && piece == 2);
	CHECK(VehicleFromActorId(VehicleActorId(0x3FFFFF, 3), h, piece) && h == 0x3FFFFF && piece == 3);
	CHECK(!VehicleFromActorId(ActorIdFromHandle(0x1234), h, piece));  // a ped isn't a vehicle
	CHECK(!HandleFromActorId(VehicleActorId(0x1234, 0), h));           // and a vehicle isn't a ped
	CHECK(VehicleActorId(0, 0) != 0);

	// A saloon: 4.8 x 1.9 m -> 3 pieces, front first.
	auto l = VehicleSegments(4.8f, 1.9f);
	CHECK(l.count == 3 && Near(l.offset[0], 1.6f) && Near(l.offset[1], 0.0f) && Near(l.offset[2], -1.6f) && Near(l.boxWidth, 1.9f));
	// A bus: 12 x 2.6 m -> 4 pieces of 3 m, boxes 3 m wide so they touch.
	l = VehicleSegments(12.0f, 2.6f);
	CHECK(l.count == 4 && Near(l.offset[0], 4.5f) && Near(l.offset[3], -4.5f) && Near(l.boxWidth, 3.0f));
	// Something square: one piece.
	l = VehicleSegments(2.0f, 2.0f);
	CHECK(l.count == 1 && Near(l.offset[0], 0.0f) && Near(l.boxWidth, 2.0f));
	// Degenerate dimensions still give something sane.
	l = VehicleSegments(0.0f, 0.0f);
	CHECK(l.count == 1 && l.boxWidth >= 0.5f);

	CHECK(Near(VehicleDamageFromMc(7.0f, 15.0f), 105.0f));
	CHECK(VehicleDamageFromMc(0.0f, 15.0f) == 0.0f && VehicleDamageFromMc(5.0f, 0.0f) == 0.0f);
	CHECK(WindowForHit(0, 3, true) == 0 && WindowForHit(0, 3, false) == 1);
	CHECK(WindowForHit(2, 3, true) == 2 && WindowForHit(2, 3, false) == 3);
	CHECK(WindowForHit(1, 4, true) == 0 && WindowForHit(2, 4, true) == 2);
	CHECK(WindowForHit(0, 1, false) == 1);
}

static void TestBlockPush()
{
	using namespace lc::blocks;
	// A wall along z at x = 5, rows 64 to 66, from z = -5 to 5 (it crosses section borders and the
	// z = 0 line, so negative coordinates are covered).
	SolidGrid g;
	auto set = [&](int x, int y, int z) {
		std::uint8_t bits[512]{};
		// Rebuild the section's bits from what's already there plus this block.
		for (int ly = 0; ly < 16; ++ly)
			for (int lz = 0; lz < 16; ++lz)
				for (int lx = 0; lx < 16; ++lx) {
					const int wx = (x >> 4) * 16 + lx, wy = (y >> 4) * 16 + ly, wz = (z >> 4) * 16 + lz;
					if (g.Solid(wx, wy, wz) || (wx == x && wy == y && wz == z)) {
						const int bit = lx + 16 * lz + 256 * ly;
						bits[bit >> 3] |= std::uint8_t(1u << (bit & 7));
					}
				}
		g.Set(x >> 4, y >> 4, z >> 4, bits);
	};
	for (int z = -5; z <= 5; ++z)
		for (int y = 64; y <= 66; ++y)
			set(5, y, z);
	CHECK(g.Solid(5, 64, -5) && g.Solid(5, 66, 5) && !g.Solid(4, 64, 0) && !g.Solid(5, 67, 0) && !g.Solid(5, 64, 6));
	CHECK(g.ColumnBlocked(5, -3, 60, 64) && !g.ColumnBlocked(5, -3, 67, 70));

	std::int32_t y0 = 0, y1 = 0;
	BodyRows(64.0, 1.8, 0.3, y0, y1);
	CHECK(y0 == 64 && y1 == 65);

	// A ped 0.2 into the wall from the west is put back against it.
	double x = 4.75, z = 0.5;
	CHECK(PushCircleOut(g, x, z, 0.45, y0, y1));
	CHECK(std::fabs(x - 4.55) < 1e-6 && std::fabs(z - 0.5) < 1e-9);
	// Clear of it: nothing happens.
	x = 4.5;
	CHECK(!PushCircleOut(g, x, z, 0.45, y0, y1) && x == 4.5);
	// Standing on top of it: nothing either.
	BodyRows(67.0, 1.8, 0.3, y0, y1);
	x = 5.5;
	CHECK(!PushCircleOut(g, x, z, 0.45, y0, y1));
	// Centre inside the wall: out through the nearer side (west here).
	BodyRows(64.0, 1.8, 0.3, y0, y1);
	x = 5.3;
	CHECK(PushCircleOut(g, x, z, 0.45, y0, y1) && std::fabs(x - 4.55) < 1e-6);
	// Round the end of the wall at z = 5..6: pushed diagonally off the corner.
	x = 4.8, z = 6.2;
	CHECK(PushCircleOut(g, x, z, 0.45, y0, y1));
	CHECK(std::hypot(x - 5.0, z - 6.0) >= 0.45 - 1e-6 && x < 4.8 && z > 6.2);

	// Walking east into it, the wall ends sooner to the south (+z, at z = 6) when standing at z = 3:
	// follow it that way (tangent (-nz, nx) with n = (-1, 0) is (0, -1), so south is side -1).
	CHECK(DetourSide(g, 4.55, 3.5, -1.0, 0.0, 1.0, 0.0, y0, y1) == -1);
	CHECK(DetourSide(g, 4.55, -3.5, -1.0, 0.0, 1.0, 0.0, y0, y1) == 1);
	// Walking along it (not into it): no detour.
	CHECK(DetourSide(g, 4.55, 0.0, -1.0, 0.0, 0.0, 1.0, y0, y1) == 0);

	// A car heading east (+x) with its nose 0.2 into the wall: pushed back west.
	auto c = VehicleContact(g, 5.2 - 2.4, 0.0, 1.0, 0.0, 2.4, 0.95, y0, y1);
	CHECK(c.hit && c.nx < -0.99 && std::fabs(c.nz) < 1e-6 && c.depth > 0.15 && c.depth < 0.25);
	// The same car parked alongside the wall (heading north, 0.1 clear): no contact.
	c = VehicleContact(g, 5.0 - 0.95 - 0.1, 0.0, 0.0, -1.0, 2.4, 0.95, y0, y1);
	CHECK(!c.hit);
	// And 0.1 into it: pushed west.
	c = VehicleContact(g, 5.0 - 0.95 + 0.1, 0.0, 0.0, -1.0, 2.4, 0.95, y0, y1);
	CHECK(c.hit && c.nx < -0.99 && c.depth > 0.05);
	// Driving into it loses the speed into the wall; driving away keeps it.
	double vx = 10.0, vz = 1.0;
	CHECK(BlockVelocity(vx, vz, -1.0, 0.0, 0.1) && std::fabs(vx + 1.0) < 1e-9 && vz == 1.0);
	vx = -3.0;
	CHECK(!BlockVelocity(vx, vz, -1.0, 0.0, 0.1) && vx == -3.0);

	// Dropping the section and clearing.
	const auto gen = g.Generation();
	g.Set(0, 4, -1, nullptr);
	CHECK(!g.Solid(5, 64, -3) && g.Solid(5, 64, 3) && g.Generation() != gen);
	g.Clear();
	CHECK(g.Empty() && !g.Solid(5, 64, 3));
}

static void TestPacer()
{
	HurtPacer p;
	HurtPacer::Batch b;
	// The first hit goes out at once.
	p.Add(proto::kHurtProjectile, 10.0f, 0x4C000101);
	CHECK(p.Tick(1.0f / 60.0f, b) && Near(b.damage, 10.0f) && b.kind == proto::kHurtProjectile && b.attacker == 0x4C000101 && b.hits == 1);
	// Two more inside Minecraft's invulnerability window: summed, sent when it ends.
	float t = 0.0f;
	p.Add(proto::kHurtMelee, 5.0f, 0x4C000202);
	CHECK(!p.Tick(0.1f, b));
	t += 0.1f;
	p.Add(proto::kHurtProjectile, 15.0f, 0);
	CHECK(!p.Tick(0.1f, b));
	t += 0.1f;
	bool sent = false;
	while (t < 1.0f && !sent) {
		sent = p.Tick(0.05f, b);
		t += 0.05f;
	}
	CHECK(sent && Near(b.damage, 20.0f) && b.hits == 2);
	CHECK(b.kind == proto::kHurtProjectile);  // the biggest hit's kind
	CHECK(b.attacker == 0x4C000202);          // the biggest had none: keep the one we know
	CHECK(t >= HurtPacer::kWindow - 0.11f);
	// Crumbs (fire ticks) wait until they add up.
	HurtPacer q;
	q.Add(proto::kHurtOther, 0.2f, 0);
	CHECK(!q.Tick(1.0f, b));
	q.Add(proto::kHurtOther, 0.2f, 0);
	CHECK(!q.Tick(1.0f, b));
	q.Add(proto::kHurtOther, 0.2f, 0);
	CHECK(q.Tick(1.0f, b) && Near(b.damage, 0.6f) && b.hits == 3);
	// Nothing pending: nothing sent; zero and negative damage ignored.
	q.Add(proto::kHurtOther, 0.0f, 0);
	q.Add(proto::kHurtOther, -5.0f, 0);
	CHECK(!q.Tick(1.0f, b) && q.Pending() == 0.0f);
	q.Add(proto::kHurtMelee, 3.0f, 0);
	q.Reset();
	CHECK(!q.Tick(1.0f, b));
}

int main()
{
	TestActorIds();
	TestWeapons();
	TestDamage();
	TestHealth();
	TestKnockback();
	TestKnockbackChain();
	TestVehicles();
	TestBlockPush();
	TestPacer();
	if (failures) {
		std::fprintf(stderr, "combat_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("combat_test: all passed\n");
	return 0;
}
