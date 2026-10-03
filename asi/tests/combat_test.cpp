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

// Every box of a vehicle's row stays within its outline grown by the slack, at any heading, and the
// row covers the outline except a shallow strip along its edges (where an aimed hit then lands a
// little deeper in, on the next box).
static void CheckLayout(float a_length, float a_width, float a_angleDeg)
{
	const float a = a_angleDeg * 3.14159265f / 180.0f;
	const float fx = std::cos(a), fy = std::sin(a);  // the vehicle's axis, world x/y
	const float e = std::fabs(fx) + std::fabs(fy);
	const auto  l = VehicleSegments(a_length, a_width, e);
	// Box centres in the world (right of the axis = (fy, -fx)), and their half sides.
	float cxs[proto::kActorVehicleSegments], cys[proto::kActorVehicleSegments], halves[proto::kActorVehicleSegments];
	for (std::uint32_t i = 0; i < l.count; ++i) {
		cxs[i] = fx * l.offset[i] + fy * l.side[i];
		cys[i] = fy * l.offset[i] - fx * l.side[i];
		halves[i] = l.size[i] * 0.5f;
	}
	bool inside = true;
	for (std::uint32_t i = 0; i < l.count; ++i) {
		const float cx = cxs[i], cy = cys[i], half = halves[i];
		for (int k = 0; k < 4; ++k) {  // the box's corners, in the vehicle's frame
			const float px = cx + (k & 1 ? half : -half), py = cy + (k & 2 ? half : -half);
			const float along = px * fx + py * fy, across = -px * fy + py * fx;
			inside = inside && std::fabs(along) <= a_length * 0.5f + kVehicleBoxSlack + 1e-3f && std::fabs(across) <= a_width * 0.5f + kVehicleBoxSlack + 1e-3f;
		}
	}
	CHECK(inside);
	bool covered = true;
	const float inset = 0.3f;
	for (float u = -a_length * 0.5f + inset; u <= a_length * 0.5f - inset + 1e-4f; u += 0.05f) {
		for (float v = -a_width * 0.5f + inset; v <= a_width * 0.5f - inset + 1e-4f; v += 0.05f) {
			const float px = fx * u - fy * v, py = fy * u + fx * v;
			bool        any = false;
			for (std::uint32_t i = 0; i < l.count && !any; ++i) {
				any = std::fabs(px - cxs[i]) <= halves[i] && std::fabs(py - cys[i]) <= halves[i];
			}
			covered = covered && any;
		}
	}
	CHECK(covered);
	if (!inside || !covered) {
		std::fprintf(stderr, "  layout %.1f x %.1f at %.0f deg: %u boxes of %.2f m (inside %d, covered %d)\n", a_length, a_width, a_angleDeg, l.count, l.boxWidth,
			inside, covered);
	}
}

static void TestVehicles()
{
	std::uint32_t h = 0, piece = 0;
	CHECK(VehicleActorId(0x1234, 2) == 0x56012342u);
	CHECK(VehicleFromActorId(VehicleActorId(0x1234, 2), h, piece) && h == 0x1234 && piece == 2);
	CHECK(VehicleFromActorId(VehicleActorId(0xFFFFF, 15), h, piece) && h == 0xFFFFF && piece == 15);
	CHECK(!VehicleFromActorId(ActorIdFromHandle(0x1234), h, piece));  // a ped isn't a vehicle
	CHECK(!HandleFromActorId(VehicleActorId(0x1234, 0), h));           // and a vehicle isn't a ped
	CHECK(VehicleActorId(0, 0) != 0);
	CHECK(proto::kActorVehicleSegments == 16);

	// A saloon (4.8 x 1.9 m) along an axis: 4 boxes 2.3 m wide (0.2 m slack each side), the end ones
	// centred 1.45 m from the middle, front first.
	auto l = VehicleSegments(4.8f, 1.9f, 1.0f);
	CHECK(l.count == 4 && Near(l.offset[0], 1.45f) && Near(l.offset[3], -1.45f) && Near(l.boxWidth, 2.3f));
	// The same car at 45 degrees: smaller boxes (1.63 m), more of them.
	l = VehicleSegments(4.8f, 1.9f, std::sqrt(2.0f));
	CHECK(l.count == 5 + 4 && Near(l.boxWidth, 2.3f / std::sqrt(2.0f), 1e-3f) && Near(l.side[5], -0.5f) && Near(l.offset[5], 1.95f));
	// Something square: one box.
	l = VehicleSegments(2.0f, 2.0f, 1.0f);
	CHECK(l.count == 1 && Near(l.offset[0], 0.0f) && Near(l.boxWidth, 2.4f));
	// Degenerate dimensions still give something sane; a long one is capped at 16 boxes.
	l = VehicleSegments(0.0f, 0.0f, 1.0f);
	CHECK(l.count == 1 && l.boxWidth >= 0.5f);
	l = VehicleSegments(40.0f, 2.5f, std::sqrt(2.0f));
	CHECK(l.count == proto::kActorVehicleSegments);
	for (float angle = 0.0f; angle <= 90.0f; angle += 7.5f) {
		CheckLayout(4.8f, 1.9f, angle);   // a saloon
		CheckLayout(5.4f, 2.0f, angle);   // a big car
		CheckLayout(12.0f, 2.6f, angle);  // a bus
		CheckLayout(2.2f, 0.8f, angle);   // a motorbike
	}

	CHECK(Near(VehicleDamageFromMc(7.0f, 15.0f), 105.0f));
	CHECK(VehicleDamageFromMc(0.0f, 15.0f) == 0.0f && VehicleDamageFromMc(5.0f, 0.0f) == 0.0f);
}

// Where a hit lands on a car (ADMIRAL-like model box: 1.98 x 5.0 x 1.5 m, origin 0.6 m above ground).
static void TestCarStrikes()
{
	const float lo[3] = { -0.99f, -2.5f, -0.6f }, hi[3] = { 0.99f, 2.5f, 0.9f };
	auto strike = [&](int a_face, float a_x, float a_y, float a_z) {
		const float p[3] = { a_x, a_y, a_z };
		return ClassifyCarStrike(a_face, p, lo, hi);
	};
	// The driver's door window (left side, glass height, middle) and the door below it.
	auto s = strike(kFaceLeft, -0.99f, 0.3f, 0.5f);
	CHECK(s.window == kWindowLF && s.seat == kSeatDriver);
	s = strike(kFaceLeft, -0.99f, 0.3f, -0.1f);
	CHECK(s.window == kWindowNone && s.seat == kSeatNone);
	// The rear doors' windows, either side; the passenger's front window.
	CHECK(strike(kFaceLeft, -0.99f, -1.0f, 0.5f).window == kWindowLR && strike(kFaceLeft, -0.99f, -1.0f, 0.5f).seat == 1);
	CHECK(strike(kFaceRight, 0.99f, -1.0f, 0.5f).window == kWindowRR && strike(kFaceRight, 0.99f, -1.0f, 0.5f).seat == 2);
	CHECK(strike(kFaceRight, 0.99f, 0.3f, 0.5f).window == kWindowRF && strike(kFaceRight, 0.99f, 0.3f, 0.5f).seat == 0);
	// Level with the bonnet or the boot, on the side: body.
	CHECK(strike(kFaceLeft, -0.99f, 2.2f, 0.5f).window == kWindowNone);
	CHECK(strike(kFaceLeft, -0.99f, -2.3f, 0.5f).window == kWindowNone);
	// From the front: the windscreen above the bonnet (whoever sits on that side), the grille below.
	CHECK(strike(kFaceFront, -0.4f, 2.5f, 0.6f).window == kWindscreen && strike(kFaceFront, -0.4f, 2.5f, 0.6f).seat == kSeatDriver);
	CHECK(strike(kFaceFront, 0.4f, 2.5f, 0.6f).seat == 0);
	CHECK(strike(kFaceFront, 0.0f, 2.5f, 0.0f).window == kWindowNone);
	CHECK(strike(kFaceRear, 0.4f, -2.5f, 0.6f).window == kWindscreenRear && strike(kFaceRear, 0.4f, -2.5f, 0.6f).seat == 2);
	// From above: the roof is body, the windscreen ahead of it glass.
	CHECK(strike(kFaceTop, 0.0f, 0.0f, 0.9f).window == kWindowNone);
	CHECK(strike(kFaceTop, 0.0f, 1.0f, 0.9f).window == kWindscreen);

	// The whole chain: a car at GTA (100, 200, 10.6), facing east (heading 270: right = -y,
	// forward = +x). Its left side faces north (+y = MC -z). An arrow flying south (MC +z, yaw 0) at
	// glass height enters the left face: the driver's window. One flying 1 m lower hits the door.
	const float pos[3] = { 100.0f, 200.0f, 10.6f };
	const float right[3] = { 0.0f, -1.0f, 0.0f }, fwd[3] = { 1.0f, 0.0f, 0.0f }, up[3] = { 0.0f, 0.0f, 1.0f };
	int   face = -1;
	float local[3];
	// The arrow is at the stand-in's box (0.3 m outside the body, MC z = -(200 + 1.3)), 0.3 m ahead of the middle.
	const double glass[3] = { 100.3, 10.6 + 0.5, -(200.0 + 1.3) };
	CHECK(StrikeOnCar(glass, 0.0f, 0.0f, pos, right, fwd, up, lo, hi, face, local));
	CHECK(face == kFaceLeft && Near(local[1], 0.3f, 1e-3f) && Near(local[2], 0.5f, 1e-3f));
	CHECK(ClassifyCarStrike(face, local, lo, hi).window == kWindowLF);
	const double door[3] = { 100.3, 10.6 - 0.5, -(200.0 + 1.3) };
	CHECK(StrikeOnCar(door, 0.0f, 0.0f, pos, right, fwd, up, lo, hi, face, local) && ClassifyCarStrike(face, local, lo, hi).window == kWindowNone);
	// Flying west along the car's flank, 0.25 m clear of it (a stand-in box's slack): misses the car.
	const double graze[3] = { 103.0, 10.6, -(200.0 + 1.24) };
	CHECK(!StrikeOnCar(graze, 90.0f, 0.0f, pos, right, fwd, up, lo, hi, face, local));
	// A sword swung down at the windscreen from in front (east of it, looking west and 30 degrees down).
	const double swing[3] = { 103.0, 10.6 + 1.0, -200.0 };
	CHECK(StrikeOnCar(swing, 90.0f, 30.0f, pos, right, fwd, up, lo, hi, face, local) && face == kFaceFront);
	CHECK(ClassifyCarStrike(face, local, lo, hi).window == kWindscreen);
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

static void TestCrimes()
{
	// Hurting a cop: 1 star, killing one 2, whoever watches.
	CHECK(WantedAfterAttack(0, true, false, 0, 0) == 1);
	CHECK(WantedAfterAttack(0, true, true, 0, 0) == 2);
	// An assault on a civilian: only if the police see it.
	CHECK(WantedAfterAttack(0, false, false, 0, 5) == 0);
	CHECK(WantedAfterAttack(0, false, false, 1, 0) == 1);
	// A killing: 2 seen by the police, 1 by bystanders, nothing unseen.
	CHECK(WantedAfterAttack(0, false, true, 1, 3) == 2);
	CHECK(WantedAfterAttack(0, false, true, 0, 3) == 1);
	CHECK(WantedAfterAttack(0, false, true, 0, 0) == 0);
	// Never lowers what the player has.
	CHECK(WantedAfterAttack(4, true, true, 3, 3) == 4);
	CHECK(ReactionOf(true, false, 7) == Reaction::kFight && ReactionOf(false, true, 7) == Reaction::kFight);
	int fight = 0;
	for (std::uint32_t seed = 0; seed < 3000; ++seed) {
		fight += ReactionOf(false, false, seed << 8) == Reaction::kFight;
	}
	CHECK(fight > 800 && fight < 1200);  // about one civilian in three
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
	TestCarStrikes();
	TestCrimes();
	TestBlockPush();
	TestPacer();
	if (failures) {
		std::fprintf(stderr, "combat_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("combat_test: all passed\n");
	return 0;
}
