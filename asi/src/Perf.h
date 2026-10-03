// Where the time goes: QueryPerformanceCounter timers around the per-frame work, summed and
// logged as one line a minute (10 s with Diagnostics). Port of SkyCraft's Perf.h without std::format.
#pragma once

#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace lc::Perf
{
	enum Slot : int
	{
		kFrame,  // the game's frame: from one processScriptsEvent to the next
		kTick,
		kCollision,
		kCamera,
		kPad,
		kDraw,
		kCount
	};

	inline constexpr const char* kNames[kCount] = { "frame", "tick", "collision", "camera", "pad", "draw" };

	struct Stat
	{
		std::atomic<std::int64_t> ticks{ 0 };
		std::atomic<std::int64_t> max{ 0 };
		std::atomic<std::int64_t> calls{ 0 };
	};
	inline Stat stats[kCount];

	inline std::int64_t Now()
	{
		LARGE_INTEGER t;
		::QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	inline double TicksPerMs()
	{
		static const double f = [] {
			LARGE_INTEGER q;
			::QueryPerformanceFrequency(&q);
			return double(q.QuadPart) / 1000.0;
		}();
		return f;
	}

	inline void Add(Slot a_slot, std::int64_t a_ticks)
	{
		auto& s = stats[a_slot];
		s.ticks.fetch_add(a_ticks, std::memory_order_relaxed);
		s.calls.fetch_add(1, std::memory_order_relaxed);
		auto m = s.max.load(std::memory_order_relaxed);
		while (a_ticks > m && !s.max.compare_exchange_weak(m, a_ticks, std::memory_order_relaxed)) {
		}
	}

	struct Scope
	{
		explicit Scope(Slot a_slot) :
			slot(a_slot), start(Now()) {}
		~Scope() { Add(slot, Now() - start); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

		Slot         slot;
		std::int64_t start;
	};

	// Once per frame on the main thread: now and then, the totals as one log line.
	inline void Report(bool a_enabled, bool a_diagnostics)
	{
		static std::int64_t last = 0, windowStart = 0;
		const auto          now = Now();
		if (last) {
			Add(kFrame, now - last);
		}
		last = now;
		if (!windowStart) {
			windowStart = now;
			return;
		}
		const double windowMs = double(now - windowStart) / TicksPerMs();
		if (windowMs < (a_diagnostics ? 10000.0 : 60000.0)) {
			return;
		}
		windowStart = now;
		const auto frames = std::max<std::int64_t>(1, stats[kFrame].calls.exchange(0));
		const auto frameTicks = stats[kFrame].ticks.exchange(0);
		const auto frameMax = stats[kFrame].max.exchange(0);
		char line[1024];
		int  n = std::snprintf(line, sizeof(line), "perf: %lld frames in %.1f s (%.0f fps), frame avg %.1f ms, worst %.1f ms", static_cast<long long>(frames),
			windowMs / 1000.0, double(frames) * 1000.0 / windowMs, double(frameTicks) / TicksPerMs() / double(frames), double(frameMax) / TicksPerMs());
		for (int i = kFrame + 1; i < kCount; ++i) {
			const auto calls = stats[i].calls.exchange(0);
			const auto ticks = stats[i].ticks.exchange(0);
			const auto max = stats[i].max.exchange(0);
			if (calls && n > 0 && n < int(sizeof(line))) {
				n += std::snprintf(line + n, sizeof(line) - n, " | %s %.2f ms/frame (worst %.1f ms, %.1f calls/frame)", kNames[i],
					double(ticks) / TicksPerMs() / double(frames), double(max) / TicksPerMs(), double(calls) / double(frames));
			}
		}
		if (a_enabled) {
			::lc::log::Write("perf", "%s", line);
		}
	}
}
