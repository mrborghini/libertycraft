// Linux unit test for combat/CombatMath.h (actor ids, damage scaling, weapon classes, hurt pacing,
// knockback, vehicles) and combat/BlockPush.h (Minecraft blocks stopping peds and vehicles).
#include "combat/BlockPush.h"
#include "combat/CombatMath.h"

#include <array>
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
			inside = inside && std::fabs(along) <= a_length * 0.5f + l.slack + 1e-3f && std::fabs(across) <= a_width * 0.5f + l.slack + 1e-3f;
		}
	}
	CHECK(inside);
	CHECK(l.slack <= kVehicleBoxSlack + 1e-4f || a_length > 8.0f);  // (only a long one runs out of boxes)
	bool covered = true;
	const float inset = a_length > 8.0f ? 0.35f : 0.3f;  // (a long one's end boxes are spread thin)
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

// A saloon's shape for VehiclePieces: 4.7 m long, 1.8 m wide (greenhouse 1.4 m), bonnet 0.95 m,
// roof 1.45 m, boot 1.0 m.
static VehicleShape Saloon()
{
	VehicleShape s;
	s.bottom = 0.0f;
	s.top = 1.45f;
	s.tail = -2.35f;
	s.nose = 2.35f;
	s.halfWidth = 0.9f;
	s.cabinHalfWidth = 0.7f;
	s.slices = kShapeSlices;
	for (int i = 0; i < s.slices; ++i) {
		const float y = s.tail + (static_cast<float>(i) + 0.5f) * s.SliceLength();
		float       t = 0.95f;  // bonnet
		if (y < -1.4f) {
			t = 1.0f;  // boot
		} else if (y < -0.9f) {
			t = 1.0f + (y + 1.4f) / 0.5f * 0.45f;  // rear window
		} else if (y < 0.4f) {
			t = 1.45f;  // roof
		} else if (y < 1.1f) {
			t = 1.45f - (y - 0.4f) / 0.7f * 0.5f;  // windscreen
		}
		s.tops[i] = t;
	}
	return s;
}

// The first of a_count boxes (world x/y centre, half side, from 0 up to a_tops) the segment
// a_o -> a_t enters (Minecraft's pick: the nearest box along the ray), or -1.
static int FirstBoxHit(const float a_o[3], const float a_t[3], const float* a_cx, const float* a_cy, const float* a_half, const float* a_tops, int a_count)
{
	int   best = -1;
	float bestT = 2.0f;
	for (int i = 0; i < a_count; ++i) {
		const float lo[3] = { a_cx[i] - a_half[i], a_cy[i] - a_half[i], 0.0f }, hi[3] = { a_cx[i] + a_half[i], a_cy[i] + a_half[i], a_tops[i] };
		float       t0 = 0.0f, t1 = 1.0f;
		bool        hit = true;
		for (int k = 0; k < 3 && hit; ++k) {
			const float d = a_t[k] - a_o[k];
			if (std::fabs(d) < 1e-9f) {
				hit = a_o[k] >= lo[k] && a_o[k] <= hi[k];
				continue;
			}
			float ta = (lo[k] - a_o[k]) / d, tb = (hi[k] - a_o[k]) / d;
			if (ta > tb) {
				std::swap(ta, tb);
			}
			t0 = std::max(t0, ta);
			t1 = std::min(t1, tb);
			hit = t0 <= t1;
		}
		if (hit && t0 < bestT) {
			bestT = t0;
			best = i;
		}
	}
	return best;
}

static void CheckPieces(const VehicleShape& a_s, float a_angleDeg)
{
	const float a = a_angleDeg * 3.14159265f / 180.0f;
	const float fx = std::cos(a), fy = std::sin(a);  // the vehicle's axis, world x/y
	const float e = std::fabs(fx) + std::fabs(fy);
	const auto  l = VehiclePieces(a_s, e);
	CHECK(l.count <= proto::kActorVehicleSegments && l.bodyCount < l.count);  // a body row and a cabin row
	float       cx[proto::kActorVehicleSegments], cy[proto::kActorVehicleSegments], half[proto::kActorVehicleSegments];
	const float mid = a_s.Middle();
	for (std::uint32_t i = 0; i < l.count; ++i) {
		cx[i] = fx * (mid + l.offset[i]) + fy * (a_s.centreX + l.side[i]);
		cy[i] = fy * (mid + l.offset[i]) - fx * (a_s.centreX + l.side[i]);
		half[i] = l.size[i] * 0.5f;
	}
	// No box reaches above the bonnet or the boot (the body row: nowhere over the body; the cabin
	// row's mean may stand a little proud of the windscreen's slope), and none sticks out of the body
	// by more than the slack, seen from above (the cabin row: of the greenhouse).
	bool under = true, inside = true;
	for (std::uint32_t i = 0; i < l.count; ++i) {
		for (float u = -half[i]; u <= half[i] + 1e-4f; u += half[i] / 8.0f) {
			for (float v = -half[i]; v <= half[i] + 1e-4f; v += half[i] / 8.0f) {
				const float px = cx[i] + u, py = cy[i] + v;
				const float along = px * fx + py * fy, across = -px * fy + py * fx;
				const float hw = i < l.bodyCount ? a_s.halfWidth + l.slack : a_s.cabinHalfWidth + 0.13f;
				inside = inside && along <= a_s.nose + l.slack + 1e-3f && along >= a_s.tail - l.slack - 1e-3f && std::fabs(across) <= hw + 1e-3f;
				if (along > 1.1f || along < -1.4f || i < l.bodyCount) {
					const float top = ShapeTop(a_s, along - 0.06f, along + 0.06f, TopOf::kHighest, 0.0f);
					under = under && (top <= 0.0f || l.height[i] <= top + 0.06f);
				}
			}
		}
	}
	CHECK(under);
	CHECK(inside);
	// The roof's middle and the bonnet are covered (hits on them land): some box there reaches 1.3 m, 0.9 m.
	auto coveredTo = [&](float a_along, float a_across, float a_z) {
		const float px = fx * a_along + fy * a_across, py = fy * a_along - fx * a_across;
		for (std::uint32_t i = 0; i < l.count; ++i) {
			if (std::fabs(px - cx[i]) <= half[i] && std::fabs(py - cy[i]) <= half[i] && l.height[i] >= a_z) {
				return true;
			}
		}
		return false;
	};
	CHECK(coveredTo(-0.25f, 0.0f, 1.3f));
	CHECK(coveredTo(1.6f, 0.0f, 0.9f) && coveredTo(1.6f, 0.5f, 0.9f));
	// A cop in cover beside the front wheel, the player 1.5 m off the other side: aimed over the
	// bonnet at the cop's chest (1.25 m) or a crouching cop's head (1.05 m), Minecraft's pick doesn't
	// stop at the car. Aimed at the door below the window, it does.
	auto world = [&](float a_along, float a_across, float a_z, float* a_out) {
		a_out[0] = fx * a_along + fy * a_across;
		a_out[1] = fy * a_along - fx * a_across;
		a_out[2] = a_z;
	};
	float eye[3], chest[3], head[3], door[3];
	world(1.5f, -2.4f, 1.62f, eye);
	world(1.5f, 1.3f, 1.25f, chest);
	world(1.5f, 1.3f, 1.05f, head);
	world(-0.2f, 0.0f, 0.7f, door);
	const int n = static_cast<int>(l.count);
	CHECK(FirstBoxHit(eye, chest, cx, cy, half, l.height, n) < 0);
	CHECK(FirstBoxHit(eye, head, cx, cy, half, l.height, n) < 0);
	CHECK(FirstBoxHit(eye, door, cx, cy, half, l.height, n) >= 0);
	if (!under || !inside) {
		std::fprintf(stderr, "  pieces at %.0f deg: %u boxes (%u body), under %d, inside %d\n", a_angleDeg, l.count, l.bodyCount, under, inside);
	}
}

static void TestVehicleShapes()
{
	const VehicleShape s = Saloon();
	CHECK(Near(ShapeTop(s, 1.5f, 2.0f, TopOf::kLowest, -1.0f), 0.95f) && Near(ShapeTop(s, -0.5f, 0.2f, TopOf::kHighest, -1.0f), 1.45f));
	CHECK(ShapeTop(s, 3.0f, 4.0f, TopOf::kLowest, -1.0f) == -1.0f);  // past the nose
	// Along an axis: five body boxes about as tall as the bonnet and the boot, then the cabin row at
	// the roof's height.
	const auto l = VehiclePieces(s, 1.0f);
	CHECK(l.bodyCount == 5 && l.count > 5);
	CHECK(Near(l.height[0], 0.95f) && Near(l.height[l.bodyCount - 1], 1.0f));
	for (std::uint32_t i = 0; i < l.bodyCount; ++i) {
		CHECK(l.height[i] <= 1.1f && l.height[i] >= 0.95f - 1e-4f);
	}
	float tallest = 0.0f;
	for (std::uint32_t i = l.bodyCount; i < l.count; ++i) {
		tallest = std::max(tallest, l.height[i]);
		CHECK(l.size[i] <= 1.4f + 1e-3f);
	}
	CHECK(tallest > 1.3f);
	for (float angle = 0.0f; angle <= 90.0f; angle += 7.5f) {
		CheckPieces(s, angle);
	}
	// A long coupe (as measured in game: a Buccaneer), whose middle body box lies wholly under the
	// cabin: the cabin row still covers the roof.
	VehicleShape c = s;
	c.tail = -2.9f;
	c.nose = 3.0f;
	c.top = 1.25f;
	static constexpr float kCoupe[8] = { 0.70f, 0.81f, 1.22f, 1.25f, 1.24f, 0.89f, 0.89f, 0.82f };
	for (int i = 0; i < c.slices; ++i) {
		c.tops[i] = kCoupe[i * 8 / c.slices];
	}
	const auto lc = VehiclePieces(c, 1.0f);
	float      roof = 0.0f;
	for (std::uint32_t i = lc.bodyCount; i < lc.count; ++i) {
		roof = std::max(roof, lc.height[i]);
	}
	CHECK(lc.count > lc.bodyCount && roof > 1.2f);
	CHECK(lc.height[0] <= 0.82f + 1e-4f && lc.height[1] <= 0.89f + 1e-4f);  // the bonnet stays low
	// Guessed from a model box: a car gets the saloon profile, mirrors off; anything else stays a box.
	const float        lo[3] = { -1.0f, -2.5f, -0.6f }, hi[3] = { 1.0f, 2.5f, 0.9f };
	const VehicleShape g = GuessShape(lo, hi, true);
	CHECK(Near(g.halfWidth, 0.92f) && g.cabinHalfWidth > 0.6f && g.tops[0] < g.tops[kShapeSlices / 2] && g.tops[kShapeSlices - 1] < g.tops[kShapeSlices / 2]);
	CHECK(Near(g.tops[kShapeSlices / 2], 0.9f) && Near(g.tops[kShapeSlices - 1], -0.6f + 1.5f * 0.66f, 1e-3f));
	const VehicleShape b = GuessShape(lo, hi, false);
	const auto         lb = VehiclePieces(b, 1.0f);
	CHECK(b.cabinHalfWidth == 0.0f && lb.count == lb.bodyCount && Near(lb.height[0], 1.5f));
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

	// A saloon (4.8 x 1.9 m) along an axis: 4 boxes 2.0 m wide (5 cm slack each side), the end ones
	// centred 1.45 m from the middle, front first.
	auto l = VehicleSegments(4.8f, 1.9f, 1.0f);
	CHECK(l.count == 4 && Near(l.offset[0], 1.45f) && Near(l.offset[3], -1.45f) && Near(l.boxWidth, 2.0f));
	// The same car at 45 degrees: smaller boxes (1.41 m), more of them, and the corners.
	l = VehicleSegments(4.8f, 1.9f, std::sqrt(2.0f));
	CHECK(l.count == 7 + 4 && Near(l.boxWidth, 2.0f / std::sqrt(2.0f), 1e-3f) && Near(l.side[7], -0.5f) && Near(l.offset[7], 1.95f));
	// Something square: one box.
	l = VehicleSegments(2.0f, 2.0f, 1.0f);
	CHECK(l.count == 1 && Near(l.offset[0], 0.0f) && Near(l.boxWidth, 2.1f));
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

static void TestBulletRays()
{
	lc::blocks::SolidGrid g;
	std::array<std::uint8_t, 512> bits{};
	auto set = [&](int a_x, int a_y, int a_z) {
		const int b = (a_x & 15) + 16 * (a_z & 15) + 256 * (a_y & 15);
		bits[b >> 3] |= static_cast<std::uint8_t>(1u << (b & 7));
	};
	// A wall at x = 5 (y 64..66, z 0..4), in section (0, 4, 0).
	for (int y = 64; y <= 66; ++y) {
		for (int z = 0; z <= 4; ++z) {
			set(5, y, z);
		}
	}
	g.Set(0, 4, 0, bits.data());
	double t = 0.0;
	int    axis = 0, sign = 0;
	// Straight at it along +x: enters at x = 5, through the face looking toward -x.
	const double a0[3] = { 1.5, 65.2, 2.5 }, a1[3] = { 9.5, 65.2, 2.5 };
	CHECK(lc::blocks::RayFirstSolid(g, a0, a1, t, axis, sign) && Near(static_cast<float>(t), 3.5f / 8.0f, 1e-5f) && axis == 0 && sign == -1);
	// From the other side, along -x: the face at x = 6, looking toward +x.
	CHECK(lc::blocks::RayFirstSolid(g, a1, a0, t, axis, sign) && Near(static_cast<float>(t), 3.5f / 8.0f, 1e-5f) && axis == 0 && sign == 1);
	// Over the wall (y 67.5), and stopping short of it: clear.
	const double o0[3] = { 1.5, 67.5, 2.5 }, o1[3] = { 9.5, 67.5, 2.5 }, s1[3] = { 4.9, 65.2, 2.5 };
	CHECK(!lc::blocks::RayFirstSolid(g, o0, o1, t, axis, sign) && !lc::blocks::RayFirstSolid(g, a0, s1, t, axis, sign));
	// Diagonal, down onto the top of the wall: the face at y = 67 looking up.
	const double d0[3] = { 5.5, 70.0, 2.5 }, d1[3] = { 5.5, 60.0, 2.5 };
	CHECK(lc::blocks::RayFirstSolid(g, d0, d1, t, axis, sign) && Near(static_cast<float>(t), 0.3f, 1e-5f) && axis == 1 && sign == 1);
	// Starting inside a block: t 0.
	const double i0[3] = { 5.5, 65.5, 2.5 };
	CHECK(lc::blocks::RayFirstSolid(g, i0, a1, t, axis, sign) && t == 0.0 && axis == -1);
	// A long shot at an angle, crossing section borders and negative coordinates, finds it too.
	const double l0[3] = { -40.3, 65.5, -30.7 }, l1[3] = { 45.0, 65.5, 33.6 };
	const bool hit = lc::blocks::RayFirstSolid(g, l0, l1, t, axis, sign);
	const double hx = l0[0] + (l1[0] - l0[0]) * t, hz = l0[2] + (l1[2] - l0[2]) * t;
	CHECK(hit && hx >= 4.99 && hx <= 6.01 && hz >= -0.01 && hz <= 5.01);
}

static void TestBumps()
{
	// A walk only nudges; a sprint makes the ped stumble; a sprint-jump, a fall or an elytra pass
	// knocks it down, and from 10 m/s on hurts it, more the faster.
	CHECK(BumpOf(0.0f).kind == BumpOutcome::kNudge && BumpOf(4.3f).kind == BumpOutcome::kNudge && BumpOf(4.3f).gtaDamage == 0.0f);
	CHECK(BumpOf(5.6f).kind == BumpOutcome::kStumble && BumpOf(5.6f).ragdollMs > 0 && BumpOf(5.6f).gtaDamage == 0.0f);
	CHECK(BumpOf(9.0f).kind == BumpOutcome::kKnockdown && BumpOf(9.0f).gtaDamage == 0.0f);
	const auto e20 = BumpOf(20.0f), e30 = BumpOf(30.0f);
	CHECK(Near(e20.gtaDamage, 80.0f) && e30.gtaDamage > e20.gtaDamage && e30.force > e20.force && e30.ragdollMs >= e20.ragdollMs);
	CHECK(BumpOf(23.0f).gtaDamage > 100.0f);  // lethal for a full-health ped (200, dies at 100)
	CHECK(BumpOf(1000.0f).force <= 18.0f && BumpOf(1000.0f).ragdollMs <= 4000 && BumpOf(1000.0f).gtaDamage <= 400.0f);
	CHECK(BumpOf(-3.0f).kind == BumpOutcome::kNudge);
	// Nudges: the overlap and a bit, capped.
	CHECK(Near(NudgeStep(0.1f), 0.13f) && Near(NudgeStep(2.0f), 0.25f) && NudgeStep(-1.0f) == 0.0f);
	// Corpses: a walk drags, faster throws further; hits push by knockback and damage.
	CHECK(CorpseBumpForce(4.3f) > 4.0f && CorpseBumpForce(20.0f) > CorpseBumpForce(4.3f) && CorpseBumpForce(500.0f) <= 14.0f);
	CHECK(CorpseHitForce(0.4f, 7.0f, false) > CorpseHitForce(0.0f, 1.0f, false) && CorpseHitForce(0.4f, 7.0f, true) < CorpseHitForce(0.4f, 7.0f, false));
	CHECK(CorpseHitForce(5.0f, 100.0f, false) <= 20.0f);
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
	// Killing police escalates: 1 kill 2 stars, 3 kills 3, 6 kills 4, 10 kills 5, 15 kills 6.
	CHECK(WantedForCopKills(0) == 0 && WantedForCopKills(1) == 2 && WantedForCopKills(2) == 2 && WantedForCopKills(3) == 3);
	CHECK(WantedForCopKills(6) == 4 && WantedForCopKills(10) == 5 && WantedForCopKills(15) == 6 && WantedForCopKills(40) == 6);
	CHECK(WantedAfterAttack(2, true, true, 1, 1, 3) == 3 && WantedAfterAttack(3, true, true, 1, 1, 6) == 4);
	CHECK(WantedAfterAttack(5, true, true, 1, 1, 3) == 5);  // never lower
	// The crime: bow hits are shooting, swords stabbing, fists hitting; cops apart.
	CHECK(CrimeForAttack(proto::kHitProjectile, proto::kWeaponArrow, true) == kCrimeShootCop);
	CHECK(CrimeForAttack(0, proto::kWeaponArrow, false) == kCrimeShootPed);
	CHECK(CrimeForAttack(0, proto::kWeaponBlade, true) == kCrimeStabCop && CrimeForAttack(0, proto::kWeaponUnarmed, false) == kCrimeHitPed);
	CHECK(CrimeForAttack(0, proto::kWeaponBlunt, true) == kCrimeHitCop && CrimeForAttack(proto::kHitExplosion, 0, true) == kCrimeCauseExplosion);
	CHECK(ReactionOf(true, false, 7) == Reaction::kFight && ReactionOf(false, true, 7) == Reaction::kFight);
	int fight = 0;
	for (std::uint32_t seed = 0; seed < 3000; ++seed) {
		fight += ReactionOf(false, false, seed << 8) == Reaction::kFight;
	}
	CHECK(fight > 800 && fight < 1200);  // about one civilian in three
}

static void TestVitals()
{
	// Java packs health * 100 | max * 100 << 16 and armour | absorption << 8 | valid.
	const std::uint32_t hw = 1450u | (2000u << 16), aw = 12u | (4u << 8) | proto::kMcVitalsValid;
	auto v = DecodeVitals(hw, aw);
	CHECK(v.valid && Near(v.health, 14.5f) && Near(v.maxHealth, 20.0f) && v.armour == 12 && Near(v.absorption, 4.0f));
	CHECK(!DecodeVitals(0, 0).valid);
	CHECK(!DecodeVitals(hw, 12u).valid);  // a writer that doesn't know the convention
	// The HUD arc (health - 100) / (200 - 100) shows Minecraft's fraction.
	auto arc = [](float a_health) { return (a_health - 100.0f) / 100.0f; };
	CHECK(Near(arc(HudHealth(1.0f)), 1.0f) && Near(arc(HudHealth(0.5f)), 0.5f) && Near(arc(HudHealth(0.725f)), 0.725f, 1e-4f));
	CHECK(Near(HudHealth(0.0f), 100.0f));   // dead: empty
	CHECK(Near(HudHealth(-1.0f), 100.0f));
	CHECK(Near(HudHealth(2.0f), 200.0f));   // never above full
	CHECK(MirroredArmour(0, 100) == 0 && MirroredArmour(20, 100) == 100 && MirroredArmour(7, 100) == 35 && MirroredArmour(30, 100) == 100);
	// Hurt directions: an attacker due north of the player (GTA +y) is MC yaw 180 (facing -z).
	auto yawOf = [](std::uint32_t a_flags) { return static_cast<int>((a_flags >> proto::kHurtDirectionShift) & 0x1FFu); };
	CHECK((HurtDirectionFlags(0.0f, 5.0f) & proto::kHurtHasDirection) && yawOf(HurtDirectionFlags(0.0f, 5.0f)) == 180);
	CHECK(yawOf(HurtDirectionFlags(0.0f, -5.0f)) == 0);    // south: +z, yaw 0
	CHECK(yawOf(HurtDirectionFlags(5.0f, 0.0f)) == 270);   // east: +x, yaw -90 = 270
	CHECK(yawOf(HurtDirectionFlags(-5.0f, 0.0f)) == 90);   // west
	CHECK(HurtDirectionFlags(0.0f, 0.0f) == 0);
	CHECK(HurtSourceYaw(HurtDirectionFlags(5.0f, 0.0f)) == 270 && HurtSourceYaw(0) == -1);
	// The shield covers the half in front: looking north (MC yaw 180), an attacker north-east (225) is
	// covered, one due east (270) or south (0) isn't; across the 0/360 seam too.
	CHECK(ShieldCovers(180.0f, 225.0f) && !ShieldCovers(180.0f, 270.0f) && !ShieldCovers(180.0f, 0.0f));
	CHECK(ShieldCovers(-10.0f, 350.0f) && ShieldCovers(350.0f, 20.0f) && !ShieldCovers(-90.0f, 90.0f));
	// The pacer keeps the direction of the biggest hit.
	HurtPacer p;
	p.Add(proto::kHurtProjectile, 10.0f, 7, HurtDirectionFlags(0.0f, 5.0f));
	p.Add(proto::kHurtMelee, 30.0f, 9, HurtDirectionFlags(5.0f, 0.0f));
	HurtPacer::Batch b;
	CHECK(p.Tick(0.0f, b) && b.attacker == 9 && yawOf(b.flags) == 270);
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
	TestVehicleShapes();
	TestCarStrikes();
	TestCrimes();
	TestVitals();
	TestBlockPush();
	TestBulletRays();
	TestBumps();
	TestPacer();
	if (failures) {
		std::fprintf(stderr, "combat_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("combat_test: all passed\n");
	return 0;
}
