// Linux unit test for render/OverlayTiles.h: the overlay's dirty tiles as the plugin reads them.
#include "render/OverlayTiles.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace lc::render::overlaytiles;
namespace proto = ::libertycraft::proto;

static int failures = 0;
#define CHECK(cond)                                                               \
	do {                                                                          \
		if (!(cond)) {                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                           \
		}                                                                         \
	} while (0)

static proto::OverlaySlotHdr Header(std::uint64_t a_frame, std::uint64_t a_base, const Mask* a_tiles, std::uint32_t a_w = 100, std::uint32_t a_h = 70)
{
	proto::OverlaySlotHdr h{};
	h.width = a_w;
	h.height = a_h;
	h.flags = 1u | (a_tiles ? proto::kOverlayFlagTiles : 0u);
	h.frameId = a_frame;
	auto* raw = reinterpret_cast<std::uint8_t*>(&h);
	std::memcpy(raw + proto::kOverlayHdrBaseFrameOff, &a_base, sizeof(a_base));
	if (a_tiles) {
		std::memcpy(raw + proto::kOverlayHdrTilesOff, a_tiles->w, sizeof(a_tiles->w));
	}
	return h;
}

static void TestHeaderLayout()
{
	// The reserved bytes hold base (8) and tiles (32) exactly.
	CHECK(proto::kOverlayHdrBaseFrameOff == offsetof(proto::OverlaySlotHdr, reserved));
	CHECK(proto::kOverlayHdrTilesOff + kWords * 4 == sizeof(proto::OverlaySlotHdr));
	Mask m;
	m.Set(5);
	m.Set(255);
	const auto h = Header(9, 7, &m);
	CHECK(HeaderBase(h) == 7);
	CHECK(HeaderTiles(h).Get(5) && HeaderTiles(h).Get(255) && HeaderTiles(h).Count() == 2);
}

static void TestReader()
{
	Reader r;
	Mask   a, b;
	a.Set(3);
	b.Set(40);
	// The first frame: whole, whatever it says.
	r.Acquired(Header(10, 9, &a));
	CHECK(r.all);
	r.Uploaded();
	CHECK(!r.all && !r.pending.Any());
	// Frames that follow the one it took: just their tiles, gathered while nothing is uploaded.
	r.Acquired(Header(12, 10, &a));
	r.Acquired(Header(13, 12, &b));
	CHECK(!r.all && r.pending.Get(3) && r.pending.Get(40) && r.pending.Count() == 2);
	r.Uploaded();
	// A frame based on one it never took (it missed one): whole.
	r.Acquired(Header(20, 15, &a));
	CHECK(r.all);
	r.Uploaded();
	// No tile flag: whole. A new size: whole.
	r.Acquired(Header(21, 20, nullptr));
	CHECK(r.all);
	r.Uploaded();
	r.Acquired(Header(22, 21, &a, 200, 70));
	CHECK(r.all);
	r.Uploaded();
	r.Acquired(Header(23, 22, &a, 200, 70));
	CHECK(!r.all && r.pending.Count() == 1);
	// A new texture: whole again.
	r.Reset();
	CHECK(r.all);
}

static void TestSpans()
{
	// 100 x 70: tiles 7 x 5 pixels, 15 x 14 of them (the last ones short). Every pixel of a set tile is
	// covered once, nothing else; the bounds hold them all.
	const std::uint32_t w = 100, h = 70;
	Mask                m;
	const std::uint32_t set[] = { 0, 1, 2, 5, 14, 16 * 3 + 7, 16 * 13 + 14, 16 * 13 + 13, 15, 16 * 14 + 2 };  // 15 and row 14 are past the frame
	for (auto t : set) {
		m.Set(t);
	}
	std::vector<int> hits(w * h, 0);
	ForEachSpan(m, w, h, [&](std::uint32_t a_x0, std::uint32_t a_x1, std::uint32_t a_y0, std::uint32_t a_y1) {
		for (std::uint32_t y = a_y0; y < a_y1; ++y) {
			for (std::uint32_t x = a_x0; x < a_x1; ++x) {
				++hits[y * w + x];
			}
		}
	});
	bool ok = true;
	for (std::uint32_t y = 0; y < h; ++y) {
		for (std::uint32_t x = 0; x < w; ++x) {
			const bool want = m.Get((y / TileH(h)) * kGrid + x / TileW(w));
			ok &= hits[y * w + x] == (want ? 1 : 0);
		}
	}
	CHECK(ok);
	Rect r;
	CHECK(Bounds(m, w, h, r));
	CHECK(r.x0 == 0 && r.y0 == 0 && r.x1 == 100 && r.y1 == 70);
	Mask one;
	one.Set(16 * 3 + 7);
	CHECK(Bounds(one, w, h, r) && r.x0 == 49 && r.x1 == 56 && r.y0 == 15 && r.y1 == 20);
	Mask none;
	none.Set(15);  // only a tile past the frame
	CHECK(!Bounds(none, w, h, r));
}

int main()
{
	TestHeaderLayout();
	TestReader();
	TestSpans();
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("overlay_test: ok\n");
	return 0;
}
