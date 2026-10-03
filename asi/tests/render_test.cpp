// Linux unit test for render/RenderMath.h: camera matrices, frustum culling, lighting terms,
// atlas mipmaps, section quad compression and the per-frame entity geometry.
#include "Coords.h"
#include "render/RenderMath.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

using namespace lc;
using namespace lc::render;

static int failures = 0;
#define CHECK(cond)                                                               \
	do {                                                                          \
		if (!(cond)) {                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                           \
		}                                                                         \
	} while (0)
#define NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) <= (eps))

static void TestSectionKey()
{
	std::unordered_set<std::uint64_t> keys;
	for (int x = -2; x <= 2; ++x) {
		for (int y = -2; y <= 2; ++y) {
			for (int z = -2; z <= 2; ++z) {
				keys.insert(SectionKey(x, y, z));
			}
		}
	}
	CHECK(keys.size() == 125);
	CHECK(SectionKey(-200, 20, 300) == SectionKey(-200, 20, 300));
	CHECK(SectionKey(1, 0, 0) != SectionKey(0, 1, 0));
}

static void Project(const Mat4& a_m, float x, float y, float z, float out[4])
{
	const float p[3] = { x, y, z };
	Transform(a_m, p, out);
}

static void TestClipFromBasis()
{
	// A camera looking north (+y) from the origin, z up.
	const float right[3] = { 1, 0, 0 }, fwd[3] = { 0, 1, 0 }, up[3] = { 0, 0, 1 };
	const Mat4  m = ClipFromBasis(right, fwd, up, 60.0f * kDegToRad, 16.0f / 9.0f, 0.5f, 1000.0f);
	float       c[4];
	Project(m, 0, 10, 0, c);
	NEAR(c[0], 0, 1e-5);
	NEAR(c[1], 0, 1e-5);
	NEAR(c[3], 10, 1e-4);  // w = distance along forward
	CHECK(c[2] / c[3] > 0.0f && c[2] / c[3] < 1.0f);
	Project(m, 0, 0.5f, 0, c);
	NEAR(c[2] / c[3], 0.0, 1e-5);  // near plane
	Project(m, 0, 1000, 0, c);
	NEAR(c[2] / c[3], 1.0, 1e-4);  // far plane
	Project(m, 3, 10, 0, c);
	CHECK(c[0] > 0);  // east is right
	Project(m, 0, 10, 3, c);
	CHECK(c[1] > 0);  // up is up
	// Vertical FOV: a point at the top edge of a 60 degree view lands on y = w.
	Project(m, 0, 10, 10.0f * std::tan(30.0f * kDegToRad), c);
	NEAR(c[1] / c[3], 1.0, 1e-4);
}

static void TestCameraRelative()
{
	// A world -> view matrix for a camera at p looking along +x: view = inverse(camera matrix).
	// Rows of the camera matrix (RAGE style): right, up, -forward (looking down -z), position.
	const float p[3] = { 1200.5f, -830.25f, 35.0f };
	Mat4        cam = Identity();
	const float r[3] = { 0, -1, 0 }, u[3] = { 0, 0, 1 }, back[3] = { -1, 0, 0 };
	for (int k = 0; k < 3; ++k) {
		cam.m[0][k] = r[k];
		cam.m[1][k] = u[k];
		cam.m[2][k] = back[k];
		cam.m[3][k] = p[k];
	}
	Mat4 view = Identity();  // inverse of a rigid transform: R^T and -p R^T
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			view.m[i][j] = cam.m[j][i];
		}
	}
	for (int j = 0; j < 3; ++j) {
		view.m[3][j] = -(p[0] * cam.m[j][0] + p[1] * cam.m[j][1] + p[2] * cam.m[j][2]);
	}
	// Right-handed perspective (looking down -z): w = -z_view.
	Mat4        proj{};
	const float n = 0.3f, f = 1500.0f, ys = 1.0f / std::tan(0.4f), xs = ys / 1.5f;
	proj.m[0][0] = xs;
	proj.m[1][1] = ys;
	proj.m[2][2] = f / (n - f);
	proj.m[2][3] = -1.0f;
	proj.m[3][2] = n * f / (n - f);
	const Mat4 full = Mul(view, proj);
	const Mat4 rel = CameraRelativeClip(view, proj);
	const float world[3] = { 1250.0f, -820.0f, 40.0f };
	float       a[4], b[4];
	Transform(full, world, a);
	const float relPos[3] = { world[0] - p[0], world[1] - p[1], world[2] - p[2] };
	Transform(rel, relPos, b);
	for (int k = 0; k < 4; ++k) {
		NEAR(a[k], b[k], 2e-3);
	}
	NEAR(b[3], 49.5, 1e-3);  // 49.5 m in front
	// Frustum from the camera-relative matrix: ahead visible, behind not, far to the side not.
	const Frustum fr = Frustum::FromClip(rel);
	const float   ahead[3] = { 30, 0, 0 }, behind[3] = { -30, 0, 0 }, side[3] = { 5, 200, 0 }, edge[3] = { 30, 40, 0 };
	CHECK(fr.SphereVisible(ahead, 1.0f));
	CHECK(!fr.SphereVisible(behind, 1.0f));
	CHECK(!fr.SphereVisible(side, 1.0f));
	CHECK(fr.SphereVisible(behind, 40.0f));  // a big sphere around the camera
	(void)edge;
}

static void TestLighting()
{
	NEAR(LightCurve(0.0f), 0.0, 1e-6);
	NEAR(LightCurve(1.0f), 1.0, 1e-6);
	CHECK(LightCurve(0.5f) < 0.5f && LightCurve(0.5f) > 0.1f);
	NEAR(FaceShade(1), 0.5, 1e-6);  // down
	NEAR(FaceShade(2), 1.0, 1e-6);  // up
	NEAR(FaceShade(3), 0.8, 1e-6);
	NEAR(FaceShade(4), 0.8, 1e-6);
	NEAR(FaceShade(5), 0.6, 1e-6);
	NEAR(FaceShade(6), 0.6, 1e-6);
	NEAR(FaceShade(0), 1.0, 1e-6);
	NEAR(DayFactor(12.0f), 1.0, 1e-6);
	NEAR(DayFactor(0.0f), 0.2, 1e-6);
	NEAR(DayFactor(24.0f), 0.2, 1e-6);
	NEAR(DayFactor(-1.0f), 0.2, 1e-6);
	CHECK(DayFactor(5.5f) < DayFactor(6.0f) && DayFactor(6.0f) < DayFactor(6.5f));
	CHECK(DayFactor(19.5f) > DayFactor(20.0f));
	NEAR(DayFactor(6.0f), 0.6, 1e-6);  // halfway through dawn
}

static void TestMips()
{
	CHECK(MipLevels(2048, 2576, 5) == 5);
	CHECK(MipLevels(4, 4, 5) == 3);
	CHECK(MipLevels(1, 1, 5) == 1);
	CHECK(RegionMipLevels(16, 32, 16, 16, 5) == 5);
	CHECK(RegionMipLevels(8, 0, 16, 16, 5) == 4);
	CHECK(RegionMipLevels(1, 0, 16, 16, 5) == 1);
	CHECK(RegionMipLevels(0, 0, 32, 2, 5) == 2);
	// 2x2 -> 1x1: alpha-weighted colour (the transparent black texels don't darken it).
	const std::uint8_t src[16] = { 200, 100, 50, 255, 0, 0, 0, 0, 200, 100, 50, 255, 0, 0, 0, 0 };
	std::uint8_t       dst[4]{};
	Downsample(src, 2, 2, 8, dst, 4);
	CHECK(dst[0] == 200 && dst[1] == 100 && dst[2] == 50);
	CHECK(dst[3] == 128);
	// Fully transparent: plain average.
	const std::uint8_t clear[16] = { 10, 20, 30, 0, 30, 40, 50, 0, 10, 20, 30, 0, 30, 40, 50, 0 };
	Downsample(clear, 2, 2, 8, dst, 4);
	CHECK(dst[0] == 20 && dst[1] == 30 && dst[2] == 40 && dst[3] == 0);
	// Odd sizes clamp.
	std::vector<std::uint8_t> odd(3 * 3 * 4, 255), half(1 * 1 * 4);
	Downsample(odd.data(), 3, 3, 12, half.data(), 4);
	CHECK(half[0] == 255 && half[3] == 255);
}

static Vertex V(float x, float y, float z, std::uint32_t flags)
{
	return Vertex{ x, y, z, 0.0f, 0.0f, 0xFFFFFFFFu, 0xF0Fu, flags };
}

static void AddQuad(std::vector<Vertex>& a_v, float a_x, std::uint32_t a_flags)
{
	const Vertex c[4] = { V(a_x, 0, 0, a_flags), V(a_x + 1, 0, 0, a_flags), V(a_x + 1, 1, 0, a_flags), V(a_x, 1, 0, a_flags) };
	for (int k : { 0, 1, 2, 0, 2, 3 }) {
		a_v.push_back(c[k]);
	}
}

static void TestSectionMesh()
{
	std::vector<Vertex> v;
	AddQuad(v, 0, kFlagTranslucent);
	AddQuad(v, 1, kFlagCutout);
	AddQuad(v, 2, kFlagCutout | (2u << 4));
	CHECK(IsQuadList(v.data(), uint32_t(v.size())));
	SectionMesh mesh;
	BuildSectionMesh(v.data(), uint32_t(v.size()), mesh);
	CHECK(mesh.quads);
	CHECK(mesh.opaque == 8 && mesh.translucent == 4);
	CHECK(mesh.vertices.size() == 12);
	CHECK(mesh.vertices[0].x == 1.0f && mesh.vertices[3].x == 1.0f && mesh.vertices[3].y == 1.0f);  // corners 0 1 2 3 of quad 1
	CHECK((mesh.vertices[8].flags & kFlagTranslucent) != 0);
	// Not a quad list (a lone triangle at the end): plain triangles, still split.
	v.push_back(V(9, 9, 9, kFlagCutout));
	v.push_back(V(9, 9, 10, kFlagCutout));
	v.push_back(V(9, 10, 9, kFlagCutout));
	CHECK(!IsQuadList(v.data(), uint32_t(v.size())));
	BuildSectionMesh(v.data(), uint32_t(v.size()), mesh);
	CHECK(!mesh.quads);
	CHECK(mesh.opaque == 15 && mesh.translucent == 6);
	// A stray vertex count is cut to whole triangles.
	BuildSectionMesh(v.data(), 4, mesh);
	CHECK(mesh.vertices.size() == 3);
}

static void TestEntities()
{
	static proto::WorldEntities e{};
	std::memset(&e, 0, sizeof(e));
	e.count = 3;
	e.entities[0].kind = proto::kWeCrack;
	e.entities[0].x = 10, e.entities[0].y = 64, e.entities[0].z = -5;
	e.entities[0].ext[0] = e.entities[0].ext[1] = e.entities[0].ext[2] = 1.0f;
	e.entities[1].kind = proto::kWeItem;
	e.entities[1].scale = 0.5f;
	e.entities[2].kind = proto::kWeBlock;
	e.entities[2].scale = 0.25f;
	e.hasSelection = 1;
	e.selMin[0] = 10, e.selMin[1] = 64, e.selMin[2] = -5;
	e.selMax[0] = 11, e.selMax[1] = 65, e.selMax[2] = -4;
	const double  o[3] = { 8, 60, -8 };
	EntityBuilder b;
	b.Build(e, o);
	CHECK(b.cracks.size() == 36);
	CHECK(b.solid.size() == 6 + 36);
	CHECK(b.outline.size() == 24);
	CHECK((b.cracks[0].flags & kFlagTranslucent) != 0);
	NEAR(b.outline[0].x, 2.0 - 0.002, 1e-5);
	NEAR(b.outline[0].y, 4.0 - 0.002, 1e-5);
	CHECK(b.outline[0].color == 0x73000000u && (b.outline[0].flags & kFlagUntextured));
	// A crack box spans exactly its extent from its corner.
	float lo = 1e9f, hi = -1e9f;
	for (const auto& v : b.cracks) {
		lo = std::min(lo, v.x);
		hi = std::max(hi, v.x);
	}
	NEAR(lo, 2.0, 1e-5);
	NEAR(hi, 3.0, 1e-5);
	b.Clear();
	CHECK(b.solid.empty() && b.cracks.empty() && b.outline.empty());
}

static void TestOverlayRect()
{
	float r[4];
	CrosshairRect(2, 1.0f, 1920, 1080, r);
	NEAR(r[0], 960 - 24, 1e-4);
	NEAR(r[1], 540 - 24, 1e-4);
	NEAR(r[2], 960 + 24, 1e-4);
	NEAR(r[3], 540 + 56, 1e-4);
	CrosshairRect(0, 1.0f, 1920, 1080, r);
	CHECK(r[0] == 0 && r[2] == 0);
}

static void TestAxes()
{
	// The vertex shader turns Minecraft (x, y, z) into GTA (x, -z, y): McToGta.
	const GtaVec g = McToGta(1.0, 2.0, 3.0);
	CHECK(g.x == 1.0 && g.y == -3.0 && g.z == 2.0);
}

int main()
{
	TestSectionKey();
	TestClipFromBasis();
	TestCameraRelative();
	TestLighting();
	TestMips();
	TestSectionMesh();
	TestEntities();
	TestOverlayRect();
	TestAxes();
	if (failures) {
		std::fprintf(stderr, "render_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("render_test: all passed\n");
	return 0;
}
