// Linux unit test for render/RenderMath.h: camera matrices, frustum culling, lighting terms,
// atlas mipmaps, section quad compression and the per-frame entity geometry.
#include "Coords.h"
#include "render/Lighting.h"
#include "render/RenderMath.h"
#include "render/Shadows.h"

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

// GTA IV's midday EXTRASUNNY-like values (timecycle colour x multiplier) and its tone mapping as
// read in game (1.0.8.0 + FusionFix, noon).
static LightingInputs Noon()
{
	LightingInputs in;
	const float dir[3] = { 0.3f, -0.2f, -0.933f };
	const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	for (int i = 0; i < 3; ++i) {
		in.sun.dir[i] = dir[i] / len;
	}
	const float col[4] = { 0.89f, 0.67f, 0.39f, 14.75f }, a0[4] = { 3.4f, 4.1f, 6.25f, 0 }, a1[4] = { 5.117f, 3.073f, -2.35f, 0 };  // as read in game
	const float fog[4] = { 73.0f, 1500.0f, 0.0f, 0.0f }, fc[4] = { 2.0f, 2.4f, 3.0f, 0 }, fn[4] = { 1.0f, 1.2f, 1.5f, 0 }, fx[4] = { 0.83f, 1.19f, 16.0f, 128.0f };
	std::memcpy(in.sun.colour, col, 16);
	std::memcpy(in.sun.amb0, a0, 16);
	std::memcpy(in.sun.amb1, a1, 16);
	std::memcpy(in.fog.params, fog, 16);
	std::memcpy(in.fog.colour, fc, 16);
	std::memcpy(in.fog.colourN, fn, 16);
	std::memcpy(in.fog.depthFx, fx, 16);
	in.tone.exposure = 1.972f;
	const float tmp[4] = { 0.9846f, 0.6986f, 0.5145f, 1 }, dsg[4] = { 0.7f, 1, 1.059f, 1 }, cc[4] = { 0.494f, 0.494f, 0.494f, 1 }, cs[4] = { 0, 0, 0, 1000 };
	std::memcpy(in.tone.tmp, tmp, 16);
	std::memcpy(in.tone.dsg, dsg, 16);
	std::memcpy(in.tone.cc, cc, 16);
	std::memcpy(in.tone.cs, cs, 16);
	in.sunOk = SunValid(in.sun);
	in.fogOk = FogValid(in.fog);
	in.depthFxOk = DepthFxValid(in.fog);
	in.toneOk = ToneValid(in.tone);
	in.adaptedTexture = true;
	return in;
}

static void TestGtaLighting()
{
	LightingInputs in = Noon();
	CHECK(in.sunOk && in.fogOk && in.depthFxOk && in.toneOk);

	// Garbage (an unrelated pass's registers, all zero) is refused.
	GtaSun zero;
	CHECK(!SunValid(zero));
	CHECK(!FogValid(GtaFog{}) && !ToneValid(GtaTone{}));
	GtaSun bad = in.sun;
	bad.dir[0] = 5.0f;
	CHECK(!SunValid(bad));
	bad = in.sun;
	bad.amb0[1] = std::nanf("");
	CHECK(!SunValid(bad));
	// gLightAmbient1 is Amb1 minus Amb0: negative channels are fine, a negative down-face ambient isn't.
	CHECK(in.sun.amb1[2] < 0.0f && SunValid(in.sun));
	bad = in.sun;
	bad.amb1[2] = -7.0f;
	CHECK(!SunValid(bad));
	GtaFog badFog = in.fog;
	badFog.params[1] = badFog.params[0];
	CHECK(!FogValid(badFog));

	// The tone mapping variant is found by its TexelSize register (c76 in the full variant).
	float regs[32][4]{};
	auto  set = [&](unsigned a_reg, std::initializer_list<float> a_v) { std::copy(a_v.begin(), a_v.end(), regs[a_reg - 64]); };
	set(66, { 1.972f, 0, 0, 0 });
	set(76, { 1.0f / 1920, 1.0f / 1080, 0, 0 });
	set(81, { 0.9846f, 0.6986f, 0.5145f, 1 });
	set(82, { 0.7f, 1, 1.059f, 1 });
	set(83, { 0.494f, 0.494f, 0.494f, 1 });
	set(84, { 0, 0, 0, 1000 });
	GtaTone tone;
	CHECK(FindTone(regs, 1.0f / 1920, 1.0f / 1080, tone) == 81);
	NEAR(tone.exposure, 1.972f, 1e-6);
	NEAR(tone.tmp[1], 0.6986f, 1e-6);
	NEAR(tone.cc[2], 0.494f, 1e-6);
	CHECK(FindTone(regs, 1.0f / 1280, 1.0f / 720, tone) == 0);  // another target size: not this
	float plain[32][4]{};
	std::memcpy(plain[66 - 64], regs[66 - 64], 16);
	std::copy_n(regs[76 - 64], 4, plain[72 - 64]);  // the plainest variant: TexelSize c72, ToneMapParams c73
	std::copy_n(regs[81 - 64], 16, plain[73 - 64]);
	CHECK(FindTone(plain, 1.0f / 1920, 1.0f / 1080, tone) == 73);

	// The shader parameters: towards the sun, HDR light, GTA's tone mapping.
	ExposureTuning t;
	LightingParams p = MakeLighting(in, t);
	CHECK(p.sunDir[3] == 1.0f && p.fogColor[3] == 1.0f && p.grade[3] == 1.0f);
	NEAR(p.sunDir[2], -in.sun.dir[2], 1e-6);
	NEAR(p.sun[0], in.sun.colour[0] * in.sun.colour[3], 1e-5);
	NEAR(p.tone[0], 1.972f * 0.6986f, 1e-5);
	NEAR(p.grade[1], 2.0f * 0.494f, 1e-6);
	// Without GTA's sun the shader keeps Minecraft's lighting.
	LightingInputs none = in;
	none.sunOk = false;
	CHECK(MakeLighting(none, t).sunDir[3] == 0.0f);
	// Without the tone mapping: the stand-in adapted luminance (a floor at night).
	LightingInputs stand = in;
	stand.adaptedTexture = false;
	LightingParams ps = MakeLighting(stand, t);
	CHECK(ps.grade[3] == 0.0f);
	NEAR(ps.tone[3], ReferenceLight(in.sun) / t.key, 1e-4);
	GtaSun night = in.sun;
	night.colour[3] = 1.0f;
	for (int i = 0; i < 3; ++i) {
		night.amb0[i] *= 0.3f;
		night.amb1[i] *= 0.3f;
	}
	CHECK(ReferenceLight(night) < t.floor);
	NEAR(StandInAdaptedLuminance(night, t), t.floor / t.key, 1e-4);

	// A sunlit top face in the open beats a side facing away from the sun; a roofed-over top (no
	// sky light) gets no sun and less ambient.
	const float up[3] = { 0, 0, 1 }, none3[3] = { 0, 0, 0 };
	float       away[3] = { -0.3f, 0.2f, 0 };
	const float al = std::sqrt(away[0] * away[0] + away[1] * away[1]);
	away[0] /= al;
	away[1] /= al;
	float top[3], side[3], roofed[3], unknown[3];
	LightFace(p, up, 1.0f, top);
	LightFace(p, away, 1.0f, side);
	LightFace(p, up, 0.0f, roofed);
	LightFace(p, none3, 1.0f, unknown);
	CHECK(Luminance(side) < Luminance(top));
	CHECK(Luminance(roofed) < 0.35f * Luminance(top));
	CHECK(Luminance(unknown) > Luminance(roofed));

	// Tone mapping: linear in the exposure, darker with a brighter adapted luminance, saturation
	// pulls colours to grey, RenderExposure scales the result.
	const float grey[3] = { 0.5f, 0.5f, 0.5f }, red[3] = { 1.0f, 0.2f, 0.2f };
	float       o1[3], o2[3], o3[3];
	ToneMap(p, 1.0f, grey, o1);
	ToneMap(p, 2.0f, grey, o2);
	CHECK(o2[0] < o1[0] && o2[0] > 0.0f);
	// grey: x = 0.5 * 1.972 * 0.6986 = 0.6888, + bloom (0.6888 - 0.9846 < 0: none), * 2 * 0.494, * 0.6888^0.059
	NEAR(o1[0], 0.6888f * 0.988f * std::pow(0.6888f, 0.059f), 1e-3);
	// brighter than the bloom threshold: x + (x - 0.9846) * 0.5145 / 4
	const float bright[3] = { 1.0f, 1.0f, 1.0f };
	float       ob[3];
	ToneMap(p, 1.0f, bright, ob);
	const float xb = 1.3776f + (1.3776f - 0.9846f) * 0.5145f / 4.0f;
	NEAR(ob[0], xb * 0.988f * std::pow(xb > 1.0f ? 1.0f : xb, 0.059f), 1e-3);
	ToneMap(p, 3.0f, red, o3);
	CHECK(o3[1] > 0.0f && o3[0] / o3[1] < red[0] / red[1]);  // less saturated (0.7)
	LightingParams twice = p;
	twice.misc[0] = 2.0f;
	float o4[3];
	ToneMap(twice, 1.0f, grey, o4);
	NEAR(o4[0], 2.0f * o1[0], 1e-5);

	// Fog: nothing in front of the start (w = 0), all of it past the end, the colour ramps near to far.
	float ramp = -1.0f;
	NEAR(FogAmount(p, 50.0f, &ramp), 0.0f, 1e-6);
	NEAR(ramp, 0.0f, 1e-6);
	NEAR(FogAmount(p, 2000.0f, &ramp), 1.0f, 1e-6);
	NEAR(ramp, 1.0f, 1e-6);
	NEAR(FogAmount(p, 0.5f * (73.0f + 1500.0f)), 0.5f, 1e-4);
	LightingParams nearW = p;
	nearW.fog[3] = 1.0f;  // all near-ramp: full at the start distance
	NEAR(FogAmount(nearW, 73.0f), 1.0f, 1e-6);
	LightingParams off = p;
	off.fogColor[3] = 0.0f;
	NEAR(FogAmount(off, 5000.0f), 0.0f, 1e-6);
}

static Vertex V(float x, float y, float z)
{
	Vertex v{};
	v.x = x;
	v.y = y;
	v.z = z;
	return v;
}

static void TestMountSplit()
{
	// A boat-sized triangle pair at the rider's feet and a cow 6 blocks away, in two texture batches.
	std::vector<proto::RenBatch> batches = { { 7, 0, 6, 0 }, { 9, 6, 3, 1 } };
	const double                 origin[3] = { 100.0, 64.0, 200.0 }, feet[3] = { 101.5, 64.2, 200.5 };
	std::vector<Vertex>          verts = {
        V(1.0f, 0.0f, 0.0f), V(2.0f, 0.0f, 0.0f), V(1.5f, 0.5f, 1.0f),   // boat (around the feet)
        V(7.0f, 0.0f, 0.0f), V(8.0f, 0.0f, 0.0f), V(7.5f, 1.0f, 0.0f),   // cow
        V(1.2f, 0.8f, 0.4f), V(1.8f, 0.8f, 0.4f), V(1.5f, 1.4f, 0.6f),   // the rider's paddle (blended batch)
	};
	std::vector<proto::RenBatch> restB, mountB;
	std::vector<Vertex>          rest, mount;
	SplitMount(batches, verts, origin, feet, restB, rest, mountB, mount);
	CHECK(mount.size() == 6 && rest.size() == 3);
	CHECK(mountB.size() == 2 && restB.size() == 1);
	CHECK(mountB[0].texture == 7 && mountB[0].first == 0 && mountB[0].count == 3 && mountB[0].flags == 0);
	CHECK(mountB[1].texture == 9 && mountB[1].first == 3 && mountB[1].count == 3 && mountB[1].flags == 1);
	CHECK(restB[0].texture == 7 && restB[0].first == 0 && restB[0].count == 3);
	CHECK(rest[0].x == 7.0f);
	CHECK(InMountBox(0.0f, 0.0f, 0.0f) && !InMountBox(3.0f, 0.0f, 0.0f) && !InMountBox(0.0f, 3.0f, 0.0f));
}

// ---- sun shadows (render/Shadows.h) ------------------------------------------------------------
// A GTA-like shadow set: an orthographic light basis (u, v across the light, depth towards the sun),
// four cascades side by side in the atlas around a camera at a_cam looking along a_fwd.
struct FakeShadow
{
	GtaShadowSet set;
	double       u[3], v[3], l[3];  // light basis (unit)
	double       size[4];           // metres across each cascade
	double       centre[4][2];      // each cascade's centre in (u, v) metres
	float        splits[3] = { 10.0f, 30.0f, 85.0f };
	float        fade = 256.0f;
};

static void Normalize(double a_v[3])
{
	const double l = std::sqrt(a_v[0] * a_v[0] + a_v[1] * a_v[1] + a_v[2] * a_v[2]);
	for (int i = 0; i < 3; ++i) {
		a_v[i] /= l;
	}
}

static FakeShadow MakeFake(const double a_cam[3], const float a_fwd[3])
{
	FakeShadow f;
	f.l[0] = 0.3, f.l[1] = -0.4, f.l[2] = 0.85;
	Normalize(f.l);
	// u = normalize(z x l), v = l x u
	f.u[0] = -f.l[1], f.u[1] = f.l[0], f.u[2] = 0.0;
	Normalize(f.u);
	f.v[0] = f.l[1] * f.u[2] - f.l[2] * f.u[1];
	f.v[1] = f.l[2] * f.u[0] - f.l[0] * f.u[2];
	f.v[2] = f.l[0] * f.u[1] - f.l[1] * f.u[0];
	const double cascadeSize[4] = { 24.0, 70.0, 180.0, 520.0 };
	const double ahead[4] = { 5.0, 20.0, 57.0, 170.0 };
	auto dotv = [](const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	// gShadowMatrix: x = P.u / S0, y = P.v / S0 (cascade 0's size), w = P.l (metres towards the sun) + 100.
	float(*m)[4] = f.set.r + shreg::kMatrix;
	for (int r = 0; r < 3; ++r) {
		m[r][0] = float(f.u[r] / cascadeSize[0]);
		m[r][1] = float(f.v[r] / cascadeSize[0]);
		m[r][2] = 0.0f;
		m[r][3] = float(f.l[r]);
	}
	m[3][0] = m[3][1] = 0.0f;
	m[3][3] = 100.0f;
	// Cascade k: uv = sc.xy * scale + offset puts its centre (ahead of the camera) mid-quarter.
	float scale[4][2], offset[4][2];
	for (int k = 0; k < 4; ++k) {
		f.size[k] = cascadeSize[k];
		const double c[3] = { a_cam[0] + a_fwd[0] * ahead[k], a_cam[1] + a_fwd[1] * ahead[k], a_cam[2] + a_fwd[2] * ahead[k] };
		f.centre[k][0] = dotv(c, f.u);
		f.centre[k][1] = dotv(c, f.v);
		const double ratio = cascadeSize[0] / cascadeSize[k];  // sc is in cascade-0 units
		scale[k][0] = float(ratio * 0.25);
		scale[k][1] = float(-ratio);
		offset[k][0] = float(0.125 + 0.25 * k - (f.centre[k][0] / cascadeSize[0]) * scale[k][0]);
		offset[k][1] = float(0.5 - (f.centre[k][1] / cascadeSize[0]) * scale[k][1]);
	}
	// Incremental registers: value(k) = dot((1, k>=1, k>=2, k>=3), reg).
	auto incr = [](const float (&a_v)[4][2], int a_c, float a_out[4]) {
		a_out[0] = a_v[0][a_c];
		for (int k = 1; k < 4; ++k) {
			a_out[k] = a_v[k][a_c] - a_v[k - 1][a_c];
		}
	};
	incr(scale, 0, f.set.r[shreg::kParam0]);
	incr(scale, 1, f.set.r[shreg::kParam4]);
	incr(offset, 0, f.set.r[shreg::kParam8]);
	incr(offset, 1, f.set.r[shreg::kParam14]);
	const double camDepth = a_cam[0] * a_fwd[0] + a_cam[1] * a_fwd[1] + a_cam[2] * a_fwd[2];
	for (int i = 0; i < 3; ++i) {
		f.set.r[shreg::kFacet][i] = float(f.splits[i] + camDepth);
	}
	f.set.r[shreg::kFacet][3] = float(f.fade + camDepth);
	f.set.r[shreg::kParam18][2] = 1.0f / 1024.0f;
	f.set.r[shreg::kParam18][3] = f.fade;
	return f;
}

static void TestShadowSet()
{
	const double cam[3] = { 893.7, -500.1, 20.1 };
	const float  fwd[3] = { 0.6f, 0.8f, 0.0f };
	FakeShadow   f = MakeFake(cam, fwd);
	CHECK(ShadowSetValid(f.set));
	float b[4];
	CascadeBounds(f.set, b);
	NEAR(b[0], 10.0, 0.01);
	NEAR(b[1], 30.0, 0.01);
	NEAR(b[2], 85.0, 0.01);
	NEAR(b[3], 256.0, 0.01);
	FakeShadow bad = f;
	bad.set.r[shreg::kMatrix][0] = std::nanf("");
	CHECK(!ShadowSetValid(bad.set));
	bad = f;
	bad.set.r[shreg::kFacet][1] = bad.set.r[shreg::kFacet][0] - 1.0f;  // cascades must grow
	CHECK(!ShadowSetValid(bad.set));
	bad = f;
	bad.set.r[shreg::kParam18][3] = 0.0f;  // no fade distance
	CHECK(!ShadowSetValid(bad.set));
	CHECK(!ShadowSetValid(GtaShadowSet{}));
	// Made for this camera; not for one 3 m further along, or looking elsewhere.
	CHECK(ShadowSetFitsCamera(f.set, cam, fwd));
	const double moved[3] = { cam[0] + 3.0 * fwd[0], cam[1] + 3.0 * fwd[1], cam[2] };
	CHECK(!ShadowSetFitsCamera(f.set, moved, fwd));
	const float other[3] = { -0.8f, 0.6f, 0.0f };
	CHECK(!ShadowSetFitsCamera(f.set, cam, other));

	// Tuning: FusionFix's defaults when its constants weren't seen; its values when they were.
	FusionShadowTune ff;
	ShadowTuning     t = TuningFrom(ff);
	NEAR(t.softness, 1.5, 1e-6);
	NEAR(t.bias, 5.0, 1e-6);
	CHECK(t.taps16 && !t.chss);
	ff.known = true;
	ff.r[1][0] = 3.0f, ff.r[1][1] = -8.0f, ff.r[1][2] = 1.0f, ff.r[1][3] = 0.2f;  // c218
	ff.r[3][2] = 4.0f / 3.0f, ff.r[3][3] = -1.0f / 3.0f;                            // c220
	ff.r[4][0] = 0.0f;                                                              // c221: Definition off
	ff.r[6][1] = 1.2f;                                                              // c223.y
	t = TuningFrom(ff);
	NEAR(t.softness, 3.0, 1e-6);
	NEAR(t.bias, 8.0, 1e-6);
	NEAR(t.blend, 0.2, 1e-6);
	NEAR(t.fov, 1.2, 1e-6);
	CHECK(!t.taps16 && t.chss);
	NEAR(t.ndlScale, 4.0 / 3.0, 1e-6);
	NEAR(t.ndlOffset, -1.0 / 3.0, 1e-6);
}

// The GTA sun pass's lookup straight from the registers, in absolute double world coordinates.
static void GtaReference(const FakeShadow& a_f, const float a_fwd[3], const double a_p[3], int& a_cascade, double a_uv[2], double& a_zr)
{
	const auto& r = a_f.set.r;
	const double d = a_fwd[0] * a_p[0] + a_fwd[1] * a_p[1] + a_fwd[2] * a_p[2];
	const double fl[4] = { 1.0, d >= r[shreg::kFacet][0] ? 1.0 : 0.0, d >= r[shreg::kFacet][1] ? 1.0 : 0.0, d >= r[shreg::kFacet][2] ? 1.0 : 0.0 };
	auto dot4 = [&](const float* a_r) { return fl[0] * a_r[0] + fl[1] * a_r[1] + fl[2] * a_r[2] + fl[3] * a_r[3]; };
	a_cascade = int(fl[1] + fl[2] + fl[3]);
	const float(*m)[4] = r + shreg::kMatrix;
	double sc[3];
	const int comp[3] = { 0, 1, 3 };
	for (int c = 0; c < 3; ++c) {
		sc[c] = a_p[0] * m[0][comp[c]] + a_p[1] * m[1][comp[c]] + a_p[2] * m[2][comp[c]] + m[3][comp[c]];
	}
	a_uv[0] = sc[0] * dot4(r[shreg::kParam0]) + dot4(r[shreg::kParam8]);
	a_uv[1] = sc[1] * dot4(r[shreg::kParam4]) + dot4(r[shreg::kParam14]);
	a_zr = 0.5 - 0.0005 * sc[2];
}

static void TestShadowLookup()
{
	const double cam[3] = { 893.7, -500.1, 20.1 };
	const float  fwd[3] = { 0.6f, 0.8f, 0.0f };
	FakeShadow   f = MakeFake(cam, fwd);
	ShadowParams p = MakeShadowParams(f.set, ShadowTuning{}, cam, fwd, true);
	CHECK(p.fwd[3] == 1.0f);
	NEAR(p.split[0], 10.0, 1e-3);  // camera-relative thresholds
	NEAR(p.split[2], 85.0, 1e-3);
	const float none[3] = { 0.0f, 0.0f, 0.0f };
	// Points at various depths: the camera-relative float lookup matches GTA's absolute one.
	const float pts[][3] = { { 3.0f, 4.0f, -1.5f }, { 1.0f, 15.0f, -1.0f }, { -6.0f, 50.0f, 3.0f }, { 50.0f, 90.0f, -10.0f }, { -2.0f, 0.5f, 0.2f } };
	const int   expect[] = { 0, 1, 2, 3, 0 };
	for (int i = 0; i < 5; ++i) {
		const ShadowCoord c = ShadowLookup(p, pts[i], none);
		const double      abs[3] = { cam[0] + pts[i][0], cam[1] + pts[i][1], cam[2] + pts[i][2] };
		int               k = -1;
		double            uv[2], zr;
		GtaReference(f, fwd, abs, k, uv, zr);
		CHECK(c.cascade == expect[i]);
		CHECK(c.cascade == k);
		NEAR(c.uv[0], uv[0], 2e-5);
		NEAR(c.uv[1], uv[1], 2e-5);
		NEAR(c.zr, zr, 1e-5);
		// Inside its quarter of the atlas.
		CHECK(c.uv[0] >= 0.25f * k && c.uv[0] <= 0.25f * (k + 1));
		CHECK(c.uv[1] >= 0.0f && c.uv[1] <= 1.0f);
		CHECK((k == 3 || c.clampU[1] <= 0.25f * (k + 1)) && (k == 0 || c.clampU[0] == 0.25f * k));  // the last one is open to the right
	}
	// Towards the sun means a smaller zr (nearer the light: what lies on the way to the sun shades).
	const float  up[3] = { 3.0f, 4.0f, -1.5f };
	const float  toward[3] = { up[0] + float(f.l[0]) * 2.0f, up[1] + float(f.l[1]) * 2.0f, up[2] + float(f.l[2]) * 2.0f };
	const auto   a = ShadowLookup(p, up, none), b = ShadowLookup(p, toward, none);
	CHECK(b.zr < a.zr);
	NEAR(a.uv[0], b.uv[0], 1e-5);  // same texel: straight along the light
	NEAR(a.uv[1], b.uv[1], 1e-5);
	// The normal offset moves a sunlit receiver towards the sun.
	const float n[3] = { 0.0f, 0.0f, 1.0f };
	CHECK(ShadowLookup(p, up, n).zr < a.zr);
	// Fade: none near the camera, full at the fade distance.
	NEAR(a.fade, (std::sqrt(9.0 + 16.0 + 2.25) / 256.0) * (std::sqrt(9.0 + 16.0 + 2.25) / 256.0), 1e-6);
	const float far[3] = { 0.0f, 300.0f, 0.0f };
	NEAR(ShadowLookup(p, far, none).fade, 1.0, 1e-6);
}

static void TestShadowCasters()
{
	const double  cam[3] = { -120.0, 1450.5, 35.0 };
	const float   fwd[3] = { -0.8f, 0.0f, -0.6f };
	FakeShadow    f = MakeFake(cam, fwd);
	ShadowParams  p = MakeShadowParams(f.set, ShadowTuning{}, cam, fwd, true);
	const float   none[3] = { 0.0f, 0.0f, 0.0f };
	std::uint32_t w = 4096, h = 1024;
	// A point drawn into its cascade lands on the texel the lookup samples for it, and with zr as written.
	const float pts[][3] = { { -4.0f, 1.0f, -3.0f }, { -16.0f, 2.0f, -12.0f }, { -40.0f, -5.0f, -30.0f }, { -150.0f, 20.0f, -100.0f } };
	for (const auto& pt : pts) {
		const ShadowCoord c = ShadowLookup(p, pt, none);
		float             rows[3][4];
		CasterRows(p, c.cascade, w, h, rows);
		float uv[2];
		CasterToUv(rows, pt, w, h, uv);
		NEAR(uv[0], c.uv[0], 2e-5);
		NEAR(uv[1], c.uv[1], 2e-5);
		const float p4[4] = { pt[0], pt[1], pt[2], 1.0f };
		const float zr = kZrBias + kZrScale * (p4[0] * rows[2][0] + p4[1] * rows[2][1] + p4[2] * rows[2][2] + rows[2][3]);
		NEAR(zr, c.zr, 1e-6);
		// The section culling agrees: a sphere around the point reaches its cascade.
		CHECK(SphereInCascade(p, c.cascade, pt, 1.0f));
		float lo, hi;
		CascadeU(c.cascade, lo, hi);
		CHECK(c.uv[0] >= lo && c.uv[0] <= hi);
	}
	// Far off to the side: in no cascade's quarter.
	const float side[3] = { 0.0f, 5000.0f, 0.0f };
	for (int k = 0; k < kCascades; ++k) {
		CHECK(!SphereInCascade(p, k, side, 14.0f));
	}
}

static void TestRayBasis()
{
	const float right[3] = { 0.6f, -0.8f, 0.0f }, fwdv[3] = { 0.8f, 0.6f, 0.0f }, up[3] = { 0, 0, 1 };
	const Mat4  m = ClipFromBasis(right, fwdv, up, 55.0f * kDegToRad, 16.0f / 9.0f, 0.1f, 2000.0f);
	float       ray[3][4];
	CHECK(RayBasis(m.m, ray));
	const float ndcs[][2] = { { 0.0f, 0.0f }, { 0.5f, -0.25f }, { -1.0f, 1.0f } };
	for (const auto& n : ndcs) {
		const float w = 37.0f;
		float       rel[3];
		for (int i = 0; i < 3; ++i) {
			rel[i] = (n[0] * ray[0][i] + n[1] * ray[1][i] + ray[2][i]) * w;
		}
		float c[4];
		Transform(m, rel, c);
		NEAR(c[3], w, 1e-3);
		NEAR(c[0] / c[3], n[0], 1e-5);
		NEAR(c[1] / c[3], n[1], 1e-5);
	}
	Mat4 zero{};
	CHECK(!RayBasis(zero.m, ray));
}

static void TestDarken()
{
	const float amb[3] = { 0.4f, 0.5f, 0.7f }, sun[3] = { 3.0f, 2.8f, 2.4f };
	float       out[3];
	DarkenFactor(amb, sun, 0.8f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, out);  // no block shadow: unchanged
	NEAR(out[0], 1.0, 1e-6);
	DarkenFactor(amb, sun, 0.8f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, out);  // GTA already shades it: no double darkening
	NEAR(out[1], 1.0, 1e-6);
	DarkenFactor(amb, sun, 0.8f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, out);  // the blocks shade a sunlit pixel
	NEAR(out[0], 0.4 / (0.4 + 3.0 * 0.8), 1e-5);
	CHECK(out[2] > out[0]);  // the shadow keeps the sky's blue
	DarkenFactor(amb, sun, 0.8f, 1.0f, 0.0f, 0.0f, 0.8f, 1.0f, out);  // GTA's tone mapping gamma
	NEAR(out[0], std::pow(0.4 / (0.4 + 3.0 * 0.8), 0.8), 1e-5);
	DarkenFactor(amb, sun, 0.8f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, out);  // all fog: no shadow
	NEAR(out[0], 1.0, 1e-6);
	DarkenFactor(amb, sun, 0.8f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, out);  // strength 0
	NEAR(out[0], 1.0, 1e-6);
	DarkenFactor(amb, sun, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, out);  // facing away from the sun
	NEAR(out[0], 1.0, 1e-6);
	const float none[3] = { 0, 0, 0 };
	DarkenFactor(none, none, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, out);  // no light at all: left alone
	NEAR(out[0], 1.0, 1e-6);
	ShadowParams p;
	p.ndl[0] = 4.0f / 3.0f, p.ndl[1] = -1.0f / 3.0f;
	const float n[3] = { 0, 0, 1 }, l[3] = { 0, 0.6f, 0.8f };
	NEAR(GtaNdl(p, n, l), 0.8 * 4.0 / 3.0 - 1.0 / 3.0, 1e-6);
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
	TestGtaLighting();
	TestMountSplit();
	TestShadowSet();
	TestShadowLookup();
	TestShadowCasters();
	TestRayBasis();
	TestDarken();
	if (failures) {
		std::fprintf(stderr, "render_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("render_test: all passed\n");
	return 0;
}
