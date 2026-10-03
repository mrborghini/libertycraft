// Linux unit test for combat/CombatMath.h (actor ids, damage scaling, weapon classes, hurt pacing).
#include "combat/CombatMath.h"

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
	TestPacer();
	if (failures) {
		std::fprintf(stderr, "combat_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("combat_test: all passed\n");
	return 0;
}
