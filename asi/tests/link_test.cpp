// Linux unit test for LinkCore.h (the mapping's seqlocks and rings) and Coords.h.
// Runs producer/consumer pairs on real threads over a heap buffer laid out like the mapping.
#include "Coords.h"
#include "LinkCore.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace proto = libertycraft::proto;
using namespace lc;

static int failures = 0;
#define CHECK(cond)                                                                 \
	do {                                                                            \
		if (!(cond)) {                                                              \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
			++failures;                                                             \
		}                                                                           \
	} while (0)

struct Buffer
{
	explicit Buffer(std::size_t a_bytes) : data(static_cast<std::uint8_t*>(std::aligned_alloc(4096, a_bytes))), size(a_bytes) { std::memset(data, 0, a_bytes); }
	~Buffer() { std::free(data); }
	std::uint8_t* data;
	std::size_t   size;
};

static void TestSeqlock()
{
	alignas(64) proto::SkyState shared{};
	std::atomic<bool> stop{ false };
	std::atomic<std::uint64_t> torn{ 0 }, reads{ 0 };
	std::thread writer([&] {
		proto::SkyState s{};
		for (std::uint32_t i = 1; i <= 2'000'000; ++i) {
			s.flags = s.worldId = s.collisionEpoch = s.teleportSeq = s.viewportW = s.viewportH = i;
			s.posX = s.posY = s.posZ = double(i);
			s.yaw = s.pitch = s.gameHour = float(i % 1000);
			core::SeqlockWrite(&shared, &s, sizeof(s));
		}
		stop = true;
	});
	std::thread reader([&] {
		proto::SkyState s{};
		while (!stop) {
			if (core::SeqlockRead(&shared, &s, sizeof(s))) {
				++reads;
				const auto i = s.flags;
				if (s.worldId != i || s.collisionEpoch != i || s.teleportSeq != i || s.viewportH != i || s.posX != double(i) || s.posZ != double(i) ||
					(i && s.gameHour != float(i % 1000)) || (s.seq & 1)) {
					++torn;
				}
			}
		}
	});
	writer.join();
	reader.join();
	std::printf("seqlock: %llu consistent reads, %llu torn\n", (unsigned long long)reads.load(), (unsigned long long)torn.load());
	CHECK(torn == 0);
	CHECK(reads > 0);
	CHECK(shared.seq == 2u * 2'000'000u);
}

static void TestInputRing()
{
	Buffer buf(proto::kInputRingDataOff + proto::kInputRingEntries * sizeof(proto::InputEvent));
	const auto ring = core::InputRing(buf.data);
	constexpr int kCount = 3'000'000;
	std::atomic<int> errors{ 0 };
	std::thread producer([&] {
		for (int i = 0; i < kCount;) {
			proto::InputEvent e{ proto::kInKey, static_cast<std::uint16_t>(i & 0xFFFF), i, -i, i ^ 0x5A5A };
			if (ring.Push(&e)) {
				++i;
			}
		}
	});
	std::thread consumer([&] {
		proto::InputEvent e{};
		for (int expect = 0; expect < kCount;) {
			if (ring.Pop(&e)) {
				if (e.a != expect || e.b != -expect || e.c != (expect ^ 0x5A5A) || e.code != (expect & 0xFFFF) || e.type != proto::kInKey) {
					++errors;
				}
				++expect;
			}
		}
	});
	producer.join();
	consumer.join();
	std::printf("input ring: %d events through a %u-entry ring, %d errors\n", kCount, proto::kInputRingEntries, errors.load());
	CHECK(errors == 0);
	// full ring drops
	Buffer small(proto::kInputRingDataOff + proto::kInputRingEntries * sizeof(proto::InputEvent));
	const auto r2 = core::InputRing(small.data);
	proto::InputEvent e{};
	int pushed = 0;
	while (r2.Push(&e)) {
		++pushed;
	}
	CHECK(pushed == int(proto::kInputRingEntries));
	// event ring consumer skips ahead when lapped
	Buffer ev(proto::kEventRingDataOff + proto::kEventRingEntries * sizeof(proto::McEvent));
	const auto er = core::EventRing(ev.data);
	for (std::uint32_t i = 0; i < proto::kEventRingEntries + 10; ++i) {  // a producer that ignores tail
		proto::McEvent m{ i, 0, 0, 0, 0, 0, 0, 0 };
		std::memcpy(er.Slot(i), &m, sizeof(m));
		core::StoreRelease(er.Head(), std::uint64_t(i + 1));
	}
	proto::McEvent out{};
	CHECK(er.Pop(&out) && out.type == 10);
}

static void TestByteRingThreaded()
{
	constexpr std::uint64_t kData = 1 << 16;  // small ring: lots of wraps and pads
	Buffer buf(core::ByteRing::kDataOff + kData);
	const core::ByteRing ring{ buf.data, kData };
	constexpr int kMessages = 400'000;
	std::atomic<int> errors{ 0 };
	std::atomic<std::uint64_t> pads{ 0 };
	std::thread producer([&] {
		std::mt19937 rng(1234);
		std::vector<std::uint8_t> payload;
		for (int i = 0; i < kMessages;) {
			const std::uint32_t bytes = 4 + rng() % 3000;
			payload.resize(bytes);
			std::memcpy(payload.data(), &i, 4);
			for (std::uint32_t k = 4; k < bytes; ++k) {
				payload[k] = static_cast<std::uint8_t>(i * 31 + k);
			}
			for (;;) {
				const auto head = core::LoadRelaxed(ring.Head());
				const auto r = ring.Write(proto::kColRegion, payload.data(), bytes);
				if (r == core::ByteRing::WriteResult::kOk) {
					if ((core::LoadRelaxed(ring.Head()) - head) != core::ByteRing::MessageBytes(bytes)) {
						++pads;
					}
					break;
				}
				if (r == core::ByteRing::WriteResult::kTooLarge) {
					++errors;
					break;
				}
				std::this_thread::yield();
			}
			++i;
		}
	});
	std::thread consumer([&] {
		int expect = 0;
		while (expect < kMessages) {
			ring.Drain(
				[&](std::uint32_t a_type, const std::uint8_t* a_p, std::uint32_t a_bytes) {
					int id = -1;
					std::memcpy(&id, a_p, 4);
					bool ok = a_type == proto::kColRegion && id == expect && a_bytes >= 4;
					for (std::uint32_t k = 4; ok && k < a_bytes; ++k) {
						ok = a_p[k] == static_cast<std::uint8_t>(id * 31 + k);
					}
					if (!ok) {
						++errors;
					}
					++expect;
				},
				1 << 20);
		}
	});
	producer.join();
	consumer.join();
	std::printf("byte ring: %d messages through %llu bytes, %llu pad records, %d errors\n", kMessages, (unsigned long long)kData,
		(unsigned long long)pads.load(), errors.load());
	CHECK(errors == 0);
	CHECK(pads > 0);
	CHECK(ring.Pending() == 0);
}

static void TestByteRingExact()
{
	constexpr std::uint64_t kData = 256;
	Buffer buf(core::ByteRing::kDataOff + kData);
	const core::ByteRing ring{ buf.data, kData };
	std::uint8_t payload[200]{};
	CHECK(ring.Write(1, payload, 121) == core::ByteRing::WriteResult::kTooLarge);
	CHECK(ring.Write(1, payload, 120) == core::ByteRing::WriteResult::kOk);  // [0,128)
	CHECK(ring.Write(2, payload, 100) == core::ByteRing::WriteResult::kOk);  // [128,240)
	CHECK(ring.Write(3, payload, 8) == core::ByteRing::WriteResult::kOk);    // [240,256): ends exactly at the end
	CHECK(core::LoadRelaxed(ring.Head()) == 256);
	CHECK(ring.Write(4, payload, 0) == core::ByteRing::WriteResult::kFull);  // no room at all
	std::vector<std::uint32_t> seen;
	ring.Drain([&](std::uint32_t t, const std::uint8_t*, std::uint32_t) { seen.push_back(t); }, 128);  // stops after ~128 bytes
	CHECK(seen.size() == 1 && seen[0] == 1);
	CHECK(core::LoadRelaxed(ring.Tail()) == 128);
	// Now 128 free at the front, head at 256 (pos 0). A 120-byte payload fits without a pad.
	CHECK(ring.Write(5, payload, 120) == core::ByteRing::WriteResult::kOk);
	ring.Drain([&](std::uint32_t t, const std::uint8_t*, std::uint32_t) { seen.push_back(t); }, 1 << 20);
	CHECK(seen.size() == 4 && seen[1] == 2 && seen[2] == 3 && seen[3] == 5);
	CHECK(ring.Pending() == 0);
	// A message that doesn't fit before the end gets a pad record and starts at 0.
	// head = tail = 384 (pos 128). Write 160 bytes: 128 + 160 > 256 -> pad 128, write at 0.
	CHECK(ring.Write(6, payload, 120) == core::ByteRing::WriteResult::kOk);  // [128,256) -> head 512
	ring.Drain([&](std::uint32_t, const std::uint8_t*, std::uint32_t) {}, 1 << 20);
	CHECK(ring.Write(7, payload, 40) == core::ByteRing::WriteResult::kOk);   // pos 0, 48 bytes -> head 560
	CHECK(ring.Write(8, payload, 120) == core::ByteRing::WriteResult::kOk);  // pos 48, 128 bytes -> head 688
	CHECK(ring.Write(9, payload, 72) == core::ByteRing::WriteResult::kOk);   // pos 176, 80 bytes: ends at the ring end, free 80
	CHECK(ring.Write(10, payload, 8) == core::ByteRing::WriteResult::kFull);
	ring.Drain([&](std::uint32_t, const std::uint8_t*, std::uint32_t) {}, 1 << 20);  // tail 768
	CHECK(ring.Write(11, payload, 40) == core::ByteRing::WriteResult::kOk);   // pos 0 -> 48
	CHECK(ring.Write(12, payload, 120) == core::ByteRing::WriteResult::kOk);  // pos 48 -> 176
	ring.Drain([&](std::uint32_t, const std::uint8_t*, std::uint32_t) {}, 1 << 20);  // tail 944
	// 128 bytes at pos 176 would cross the end: an 80-byte pad record, then the message at 0.
	CHECK(ring.Write(13, payload, 120) == core::ByteRing::WriteResult::kOk);
	CHECK(core::LoadRelaxed(ring.Head()) == 944 + 80 + 128);
	CHECK(reinterpret_cast<const proto::ColMsgHeader*>(ring.Data() + 176)->type == proto::kColPad);
	seen.clear();
	const auto consumed = ring.Drain([&](std::uint32_t t, const std::uint8_t*, std::uint32_t) { seen.push_back(t); }, 1 << 20);
	CHECK(consumed == 80 + 128 && seen.size() == 1 && seen[0] == 13 && ring.Pending() == 0);
}

static void TestOverlay()
{
	std::uint32_t state = 0, front = 2;
	CHECK(!core::OverlayAcquire(&state, front));
	// writer publishes slot 1 (its back) and takes the old middle (0) as its new back
	const auto old = core::Exchange(&state, 1u | proto::kOverlayDirty);
	CHECK((old & 3) == 0);
	CHECK(core::OverlayAcquire(&state, front));
	CHECK(front == 1);
	CHECK(state == 2);  // our old front is the new middle, clean
	CHECK(!core::OverlayAcquire(&state, front));
}

static void TestHeaderReset()
{
	Buffer ctl(proto::kOffOverlayPixels);
	Buffer ren(proto::kRenRingDataOff);
	auto* h = reinterpret_cast<proto::Header*>(ctl.data);
	h->mcPid = 1234;
	h->mcHeartbeatMs = 99;
	reinterpret_cast<std::uint64_t*>(ctl.data + proto::kOffInputRing)[0] = 77;
	reinterpret_cast<std::uint64_t*>(ren.data)[0] = 55;
	core::ResetHostRegions(ctl.data, ren.data, 4321, 1000);
	CHECK(h->magic == proto::kMagic && h->version == proto::kVersion && h->skyrimPid == 4321 && h->skyrimHeartbeatMs == 1000);
	CHECK(h->mcPid == 0);
	CHECK(h->mcHeartbeatMs == 99);  // Minecraft's own field stays
	CHECK(reinterpret_cast<std::uint64_t*>(ctl.data + proto::kOffInputRing)[0] == 0);
	CHECK(reinterpret_cast<std::uint64_t*>(ren.data)[0] == 0);
}

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) < eps; }

static void TestCoords()
{
	const auto mc = GtaToMc(10.0, 20.0, 30.0);
	CHECK(mc.x == 10.0 && mc.y == 30.0 && mc.z == -20.0);
	const auto g = McToGta(mc);
	CHECK(g.x == 10.0 && g.y == 20.0 && g.z == 30.0);
	CHECK(Near(McYawToGtaHeading(0.0f), 180.0f));   // MC south = GTA heading 180
	CHECK(Near(McYawToGtaHeading(90.0f), 90.0f));   // west = west
	CHECK(Near(McYawToGtaHeading(180.0f), 0.0f));   // north
	CHECK(Near(McYawToGtaHeading(-90.0f), 270.0f));  // east
	for (float y = -180.0f; y < 180.0f; y += 7.5f) {
		CHECK(Near(GtaHeadingToMcYaw(McYawToGtaHeading(y)), y, 1e-3));
	}
	// A ped with heading h faces (-sin h, cos h); MC yaw faces mc (-sin yaw, 0, cos yaw).
	for (float yaw = -180.0f; yaw < 180.0f; yaw += 15.0f) {
		const float h = McYawToGtaHeading(yaw) * kDegToRad;
		const auto  mcDir = GtaToMc(-std::sin(h), std::cos(h), 0.0);
		CHECK(Near(mcDir.x, -std::sin(yaw * kDegToRad)) && Near(mcDir.z, std::cos(yaw * kDegToRad)));
		for (float pitch = -80.0f; pitch <= 80.0f; pitch += 40.0f) {
			const auto b = LookBasis(yaw, pitch);
			// forward matches Minecraft's look vector (-sin y cos p, -sin p, cos y cos p)
			const auto f = GtaToMc(b.forward[0], b.forward[1], b.forward[2]);
			const float yr = yaw * kDegToRad, pr = pitch * kDegToRad;
			CHECK(Near(f.x, -std::sin(yr) * std::cos(pr)) && Near(f.y, -std::sin(pr)) && Near(f.z, std::cos(yr) * std::cos(pr)));
			// orthonormal and right-handed: right x forward = up
			auto dot = [](const float* a, const float* c) { return a[0] * c[0] + a[1] * c[1] + a[2] * c[2]; };
			CHECK(Near(dot(b.right, b.forward), 0.0) && Near(dot(b.right, b.up), 0.0) && Near(dot(b.forward, b.up), 0.0));
			CHECK(Near(dot(b.right, b.right), 1.0) && Near(dot(b.up, b.up), 1.0) && Near(dot(b.forward, b.forward), 1.0));
			CHECK(b.up[2] > 0.0f);
			// right points to the viewer's right: for yaw 0 (facing south) that is west (-x)
			if (yaw == 0.0f) {
				CHECK(Near(b.right[0], -1.0) && Near(b.right[1], 0.0));
			}
		}
	}
}

int main()
{
	TestSeqlock();
	TestInputRing();
	TestByteRingThreaded();
	TestByteRingExact();
	TestOverlay();
	TestHeaderReset();
	TestCoords();
	if (failures) {
		std::printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("all link/coords tests passed\n");
	return 0;
}
