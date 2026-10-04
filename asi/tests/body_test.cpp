// Linux unit test for render/Body.h: the Minecraft body's parts on GTA's skeleton (standing,
// seated, an arm raised), the head bone's axis map and the posed mesh.
#include "render/Body.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace lc::render::body;
namespace proto = ::libertycraft::proto;

static int failures = 0;
#define CHECK(cond)                                                               \
	do {                                                                          \
		if (!(cond)) {                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                           \
		}                                                                         \
	} while (0)
#define NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) <= (eps))

static void NearV(V3 a, V3 b, float eps, int line)
{
	if (Length(a - b) > eps) {
		std::fprintf(stderr, "FAIL %s:%d: (%.3f %.3f %.3f) vs (%.3f %.3f %.3f)\n", __FILE__, line, a.x, a.y, a.z, b.x, b.y, b.z);
		++failures;
	}
}
#define NEARV(a, b, eps) NearV((a), (b), (eps), __LINE__)

// The standing body placed in GTA facing heading a_h (degrees, 0 north, counter-clockwise), feet at a_at.
struct Placement
{
	M3 r;
	V3 at;
	V3 operator()(V3 a_p) const { return at + Apply(r, a_p); }
};

static Placement Place(float a_h, V3 a_at)
{
	const float h = a_h * 3.14159265f / 180.0f;
	Placement   p;
	p.r.c[0] = { -std::cos(h), -std::sin(h), 0.0f };  // left
	p.r.c[1] = { 0.0f, 0.0f, 1.0f };                  // up
	p.r.c[2] = { -std::sin(h), std::cos(h), 0.0f };   // forward
	p.at = a_at;
	return p;
}

// A GTA skeleton exactly where Minecraft's standing model has its joints; the head bone's axes are
// GTA IV's (x up the neck, y forward, z left).
static Skeleton FromModel(const Placement& a_p)
{
	Skeleton s;
	s.hipL = a_p(HipJoint(true));
	s.hipR = a_p(HipJoint(false));
	s.ankleL = a_p(HipJoint(true) + LegEnd()) + V3{ 0, 0, kAnkleHeight };
	s.ankleR = a_p(HipJoint(false) + LegEnd()) + V3{ 0, 0, kAnkleHeight };
	s.kneeL = (s.hipL + s.ankleL) * 0.5f;
	s.kneeR = (s.hipR + s.ankleR) * 0.5f;
	s.knees = true;
	s.shoulderL = a_p(ShoulderJoint(true));
	s.shoulderR = a_p(ShoulderJoint(false));
	s.handL = a_p(ShoulderJoint(true) + ArmEnd(true));
	s.handR = a_p(ShoulderJoint(false) + ArmEnd(false));
	s.neck = a_p(NeckJoint());
	s.head = a_p(NeckJoint() + V3{ 0, kHeadPx * kPx - kSkullTop, 0 });  // the top of the head where Minecraft's is
	s.headPos = true;
	s.headAxes[0] = a_p.r.c[1];
	s.headAxes[1] = a_p.r.c[2];
	s.headAxes[2] = a_p.r.c[0];
	s.headOk = true;
	return s;
}

static float Det(const Part& a_p)
{
	const V3 x{ a_p.m[0][0], a_p.m[1][0], a_p.m[2][0] }, y{ a_p.m[0][1], a_p.m[1][1], a_p.m[2][1] }, z{ a_p.m[0][2], a_p.m[1][2], a_p.m[2][2] };
	return Dot(Cross(x, y), z);
}

static void TestArc()
{
	const V3 a{ 0, 0, -1 }, b = Normalize(V3{ 0, 1, -1 }, {});
	const M3 r = Arc(a, b, { 1, 0, 0 });
	NEARV(Apply(r, a), b, 1e-5f);
	NEARV(Apply(r, V3{ 1, 0, 0 }), (V3{ 1, 0, 0 }), 1e-5f);  // the axis of the turn stays
	const M3 half = Arc(a, V3{ 0, 0, 1 }, { 1, 0, 0 });
	NEARV(Apply(half, a), (V3{ 0, 0, 1 }), 1e-5f);
	NEARV(Apply(half, V3{ 1, 0, 0 }), (V3{ 1, 0, 0 }), 1e-5f);  // half a turn about x
	const M3 none = Arc(b, b, { 1, 0, 0 });
	NEARV(Apply(none, V3{ 0.3f, -0.2f, 0.9f }), (V3{ 0.3f, -0.2f, 0.9f }), 1e-5f);
}

static void TestHeadMap()
{
	const Placement p = Place(30.0f, { 5, 6, 7 });
	const Skeleton  s = FromModel(p);
	HeadMap         m;
	CHECK(CalibrateHead(s.headAxes, p.r, m));
	CHECK(m == kNikoHead);
	const M3 h = HeadFrame(s.headAxes, m);
	for (int i = 0; i < 3; ++i) {
		NEARV(h.c[i], p.r.c[i], 1e-5f);
	}
	// A head turned 60 degrees: no clear match.
	const Placement turned = Place(90.0f, {});
	HeadMap         m2;
	CHECK(!CalibrateHead(FromModel(turned).headAxes, Place(30.0f, {}).r, m2));
	// Axes that don't span the body's (a bone squashed flat): no match.
	const V3 flat[3] = { s.headAxes[0], s.headAxes[0], s.headAxes[2] };
	HeadMap  m3;
	CHECK(!CalibrateHead(flat, p.r, m3));
	HeadMap bad;
	bad.axis[0] = 1, bad.axis[1] = -1, bad.axis[2] = 3;
	CHECK(!bad.Valid());
}

// GTA's joints where Minecraft's are: every part is the plain placement.
static void TestStandingIdentity()
{
	for (const float heading : { 0.0f, 30.0f, -135.0f }) {
		const Placement p = Place(heading, { 100.0f, -40.0f, 12.0f });
		const Skeleton  s = FromModel(p);
		HeadMap         m;
		CHECK(CalibrateHead(s.headAxes, p.r, m));
		const Pose pose = Solve(s, m);
		NEAR(pose.lift, 0.0f, 1e-4f);
		NEAR(pose.scale, 1.0f, 1e-4f);
		NEAR(pose.headScale, 1.0f, 1e-4f);
		NEAR(pose.legScale, 1.0f, 1e-4f);
		CHECK(pose.headFromBone);
		CHECK(m == kNikoHead);  // the test's bone has GTA's axes
		const V3 probes[] = { { 0, 0, 0 }, { 0.2f, 1.0f, 0.1f }, { -0.3f, 1.2f, -0.1f }, { 0.1f, 1.8f, 0.2f }, { -0.1f, 0.3f, 0.1f } };
		for (std::uint32_t part = 1; part < proto::kPartCount; ++part) {
			NEAR(Det(pose.part[part]), 1.0f, 1e-4f);
			for (const V3 q : probes) {
				NEARV(Transform(pose.part[part], q), p(q), 2e-4f);
			}
		}
	}
}

// Niko standing: his hips -> neck is shorter than Minecraft's and his legs longer: the upper body
// shrinks to his torso, the legs stretch to his, the soles stand on the ground.
static void TestStandingNiko()
{
	Skeleton s;  // facing north: left is west (-x)
	s.hipL = { -0.1f, 0, 0.95f }, s.hipR = { 0.1f, 0, 0.95f };
	s.ankleL = { -0.1f, 0, kAnkleHeight }, s.ankleR = { 0.1f, 0, kAnkleHeight };
	s.kneeL = { -0.1f, 0.03f, 0.5f }, s.kneeR = { 0.1f, 0.03f, 0.5f };
	s.knees = true;
	s.shoulderL = { -0.2f, 0, 1.45f }, s.shoulderR = { 0.2f, 0, 1.45f };
	s.handL = { -0.25f, 0.05f, 0.8f }, s.handR = { 0.25f, 0.05f, 0.8f };
	s.neck = { 0, 0, 1.55f };
	s.head = { 0, 0, 1.65f };
	s.headPos = true;
	const Pose  pose = Solve(s, HeadMap{});
	const float k = 0.6f / (kNeckY - kHipY);
	NEAR(pose.scale, k, 1e-4f);
	NEAR(pose.legScale, LegLength(s, true) / kLegLength, 1e-4f);
	CHECK(pose.legScale > 1.3f && pose.legScale < 1.4f);
	CHECK(!pose.headFromBone);
	NEARV(pose.torso.c[0], (V3{ -1, 0, 0 }), 1e-5f);
	NEARV(pose.torso.c[2], (V3{ 0, 1, 0 }), 1e-5f);
	// A bent knee: the straight leg reaches a little further, the figure rises by that much.
	const float straight = Length(s.ankleL - s.hipL) + kAnkleHeight;
	NEAR(pose.lift, pose.legScale * kLegLength - straight, 1e-4f);
	// Soles on the ground under the hips; the head's top where the scaled body puts it.
	NEARV(Transform(pose.part[proto::kPartRightLeg], HipJoint(false) + LegEnd()), (V3{ k * kLegX, 0, 0 }), 1e-4f);
	NEARV(Transform(pose.part[proto::kPartLeftLeg], HipJoint(true) + LegEnd()), (V3{ -k * kLegX, 0, 0 }), 1e-4f);
	// The head shrinks so its top is Niko's (his head bone + kSkullTop), Minecraft's head being twice a person's.
	CHECK(pose.headScale < k && pose.headScale > 0.6f * k);
	NEAR(Transform(pose.part[proto::kPartHead], V3{ 0, kNeckY + 8 * kPx, 0 }).z, 1.65f + kSkullTop + pose.lift, 1e-4f);
	// MinecraftBodyScale: bigger all round, the legs still reach the ground.
	const Pose big = Solve(s, HeadMap{}, 1.2f);
	NEAR(big.scale, 1.2f * k, 1e-4f);
	NEAR(big.legScale, pose.legScale, 1e-4f);
	NEAR(std::min(Transform(big.part[proto::kPartLeftLeg], HipJoint(true) + LegEnd()).z, Transform(big.part[proto::kPartRightLeg], HipJoint(false) + LegEnd()).z), 0.0f,
		1e-4f);
	NEAR(Det(pose.part[proto::kPartBody]), k * k * k, 1e-4f);
	NEAR(Det(pose.part[proto::kPartLeftLeg]), k * k * pose.legScale, 1e-4f);
	// The arms point at the hands, down and a little forward.
	for (const bool left : { false, true }) {
		const Part& arm = pose.part[left ? proto::kPartLeftArm : proto::kPartRightArm];
		const V3    joint = Transform(arm, ShoulderJoint(left));
		const V3    end = Transform(arm, ShoulderJoint(left) + ArmEnd(left));
		CHECK(Dot(Normalize(end - joint, {}), Normalize((left ? s.handL : s.handR) - joint, {})) > 0.9999f);
		NEAR(Length(end - joint), k * Length(ArmEnd(left)), 1e-4f);
		CHECK(end.y > joint.y);
		NEAR(Det(arm), k * k * k, 1e-4f);
	}
}

// Seated in a car facing east: thighs forward, shins down, hands forward on a wheel.
static void TestSeated()
{
	// Facing east (heading -90): forward +x, left +y.
	Skeleton s;
	s.hipL = { 0.0f, 0.1f, 0.5f }, s.hipR = { 0.0f, -0.1f, 0.5f };
	s.ankleL = { 0.45f, 0.12f, 0.1f }, s.ankleR = { 0.45f, -0.12f, 0.1f };
	s.shoulderL = { -0.05f, 0.2f, 1.0f }, s.shoulderR = { -0.05f, -0.2f, 1.0f };
	s.handL = { 0.45f, 0.18f, 0.85f }, s.handR = { 0.45f, -0.18f, 0.85f };
	s.neck = { -0.05f, 0, 1.1f };
	const Pose pose = Solve(s, HeadMap{});
	CHECK(pose.torso.c[2].x > 0.99f);   // facing east
	CHECK(pose.torso.c[0].y > 0.99f);   // left is north
	CHECK(std::fabs(pose.lift) < 0.1f);  // the hips stay on the seat
	for (const bool left : { false, true }) {
		const Part& leg = pose.part[left ? proto::kPartLeftLeg : proto::kPartRightLeg];
		const V3    hip = Transform(leg, HipJoint(left));
		const V3    sole = Transform(leg, HipJoint(left) + LegEnd());
		const V3    gta = Normalize((left ? s.ankleL : s.ankleR) - (left ? s.hipL : s.hipR), {});
		CHECK(Dot(Normalize(sole - hip, {}), gta) > 0.9999f);
		NEAR(Length(sole - hip), kLegLength * pose.legScale, 1e-4f);
		CHECK(Det(leg) > 0.0f);
		// The leg's front (the standing body's +z) faces up now that it points forward.
		const V3 front = Transform(leg, HipJoint(left) + V3{ 0, 0, 1 }) - hip;
		CHECK(front.z > 0.6f);
		// Hands meet the wheel: the arm's end lies on the line to GTA's hand.
		const Part& arm = pose.part[left ? proto::kPartLeftArm : proto::kPartRightArm];
		const V3    joint = Transform(arm, ShoulderJoint(left));
		const V3    end = Transform(arm, ShoulderJoint(left) + ArmEnd(left));
		CHECK(Dot(Normalize(end - joint, {}), Normalize((left ? s.handL : s.handR) - joint, {})) > 0.9999f);
		CHECK(end.x > joint.x + 0.3f);  // reaching forward
	}
	// The lower sole meets GTA's.
	const float sole = std::min(Transform(pose.part[proto::kPartLeftLeg], HipJoint(true) + LegEnd()).z,
		Transform(pose.part[proto::kPartRightLeg], HipJoint(false) + LegEnd()).z);
	NEAR(sole, 0.1f - kAnkleHeight, 1e-4f);
}

// The right arm straight up: the Minecraft arm points up too, its front still forward.
static void TestArmRaised()
{
	const Placement p = Place(0.0f, {});
	Skeleton        s = FromModel(p);
	s.handR = s.shoulderR + V3{ 0, 0, 0.6f };
	const Pose  pose = Solve(s, HeadMap{});
	const Part& arm = pose.part[proto::kPartRightArm];
	const V3    joint = Transform(arm, ShoulderJoint(false));
	const V3    end = Transform(arm, ShoulderJoint(false) + ArmEnd(false));
	CHECK(Dot(Normalize(end - joint, {}), Normalize(s.handR - joint, {})) > 0.9999f);
	CHECK(end.z > joint.z + 0.5f);
	NEAR(Det(arm), 1.0f, 1e-4f);
	// The other parts don't move.
	NEARV(Transform(pose.part[proto::kPartLeftArm], ShoulderJoint(true) + ArmEnd(true)), p(ShoulderJoint(true) + ArmEnd(true)), 2e-4f);
	NEARV(Transform(pose.part[proto::kPartBody], V3{ 0, 1.0f, 0 }), p(V3{ 0, 1.0f, 0 }), 2e-4f);
}

static void TestPoseMesh()
{
	const Placement p = Place(-90.0f, { 1, 2, 3 });
	const Skeleton  s = FromModel(p);
	const Pose      pose = Solve(s, HeadMap{});
	std::vector<proto::RenBatch> batches = { { 7, 0, 3, (proto::kPartHead << proto::kRagdollPartShift) },
		{ 7, 3, 3, (proto::kPartRightArm << proto::kRagdollPartShift) | proto::kRagdollHeld | 1u }, { 7, 6, 3, 15u << proto::kRagdollPartShift } };
	std::vector<Vertex> in(9);
	for (std::size_t i = 0; i < in.size(); ++i) {
		in[i].x = 0.1f * float(i);
		in[i].y = 1.0f + 0.05f * float(i);
		in[i].z = -0.1f;
		in[i].flags = 1u | (i == 4 ? (3u << 4) : (7u << 4));
	}
	std::vector<Vertex> out;
	PoseMesh(batches, in, pose, out);
	CHECK(out.size() == in.size());
	for (std::size_t i = 0; i < in.size(); ++i) {
		const V3 g = p(V3{ in[i].x, in[i].y, in[i].z });
		NEAR(out[i].x, g.x, 2e-4f);
		NEAR(out[i].y, g.z, 2e-4f);
		NEAR(out[i].z, -g.y, 2e-4f);
		CHECK(((out[i].flags >> 4) & 7) == 7);
		CHECK((out[i].flags & 1) == 1);
	}
	CHECK(PartOf(batches[1].flags) == proto::kPartRightArm);
	CHECK(PartOf(batches[2].flags) == proto::kPartNone);
	PartBounds bounds[proto::kPartCount];
	MeasureParts(batches, in, bounds);
	CHECK(bounds[proto::kPartHead].vertices == 3);
	NEAR(bounds[proto::kPartRightArm].lo[0], 0.3f, 1e-5f);
	NEAR(bounds[proto::kPartRightArm].hi[1], 1.25f, 1e-5f);

	// The held items hidden (in a vehicle): their vertices collapse onto one point; the rest stay.
	Pose hidden = pose;
	HideHeld(hidden);
	std::vector<Vertex> out2;
	PoseMesh(batches, in, hidden, out2);
	for (std::size_t i = 0; i < in.size(); ++i) {
		const bool held = i >= 3 && i < 6;
		if (held) {
			NEAR(out2[i].x, out2[3].x, 1e-6f);
			NEAR(out2[i].y, out2[3].y, 1e-6f);
			NEAR(out2[i].z, out2[3].z, 1e-6f);
		} else {
			NEAR(out2[i].x, out[i].x, 1e-6f);
			NEAR(out2[i].z, out[i].z, 1e-6f);
		}
	}
	CHECK(std::fabs(out[3].x - out[5].x) + std::fabs(out[3].y - out[5].y) + std::fabs(out[3].z - out[5].z) > 0.1f);  // (posed as usual they are apart)
}

// GTA's camera inside the body: the parts it is in (or within the margin of) collapse, the rest stay.
static void TestCameraInside()
{
	const Placement p = Place(30.0f, { 4, -2, 7 });
	const Skeleton  s = FromModel(p);
	Pose            pose = Solve(s, kNikoHead);
	const V3        headMid = p(NeckJoint() + V3{ 0, 4 * kPx, 0 });
	NEAR(DistanceToPart(pose, proto::kPartHead, headMid), 0.0f, 1e-5f);
	CHECK(DistanceToPart(pose, proto::kPartBody, headMid) > 0.1f);
	// 0.5 m in front of the face: outside every part.
	const V3 front = p(NeckJoint() + V3{ 0, 4 * kPx, 0.5f });
	NEAR(DistanceToPart(pose, proto::kPartHead, front), 0.5f - 5 * kPx, 1e-3f);
	Pose clear = pose;
	CHECK(HideNearCamera(clear, front, 0.15f) == 0u);
	// In the head: only the head goes.
	Pose inHead = pose;
	const std::uint32_t mask = HideNearCamera(inHead, headMid, 0.15f);
	CHECK(mask == (1u << proto::kPartHead));
	NEAR(Det(inHead.part[proto::kPartHead]), 0.0f, 1e-9f);
	NEAR(Det(inHead.part[proto::kPartBody]), Det(pose.part[proto::kPartBody]), 1e-6f);
	// Just above the neck, inside the head's and within 0.15 m of the body's: both go.
	Pose neck = pose;
	CHECK(HideNearCamera(neck, p(NeckJoint() + V3{ 0, 0.02f, 0 }), 0.15f) == ((1u << proto::kPartHead) | (1u << proto::kPartBody)));
	// The whole body: everything collapses (held items too) once a part is near; nothing near, nothing.
	Pose all = pose;
	CHECK(HideNearCamera(all, headMid, 0.15f, true) == (1u << proto::kPartHead));
	for (std::uint32_t part = 0; part < proto::kPartCount; ++part) {
		NEAR(Det(all.part[part]), 0.0f, 1e-9f);
	}
	Pose none = pose;
	CHECK(HideNearCamera(none, front, 0.15f, true) == 0u);
	NEAR(Det(none.part[proto::kPartBody]), Det(pose.part[proto::kPartBody]), 1e-6f);
}

int main()
{
	TestArc();
	TestHeadMap();
	TestStandingIdentity();
	TestStandingNiko();
	TestSeated();
	TestArmRaised();
	TestPoseMesh();
	TestCameraInside();
	if (failures) {
		std::fprintf(stderr, "body_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("body_test: all passed\n");
	return 0;
}
