"""Watch Minecraft's physics ticks and frames through the live bridge; report stalls.

    python3 tools/tick_monitor.py [seconds]

The bridge is LIBERTYCRAFT_LINK or /dev/shm/libertycraft-bridge. Ported from SkyCraft (MIT).
"""
import mmap, os, struct, sys, time
PATH = os.environ.get("LIBERTYCRAFT_LINK", "/dev/shm/libertycraft-bridge")
# McState.tickQpc is in Wine's QueryPerformanceCounter units: CLOCK_MONOTONIC_RAW / 100 ns.
F = 10_000_000
def qpc():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW) // 100
try:
    with open(PATH, "rb") as f:
        m = mmap.mmap(f.fileno(), 0x1000, access=mmap.ACCESS_READ)
except (OSError, ValueError) as e:
    sys.exit(f"can't map {PATH} ({e}); is the host or Minecraft running?")
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
last_tick = None; last_tick_seen = None; ticks = []; stalls = []
last_frame = None; last_frame_t = qpc(); frame_stalls = []
end = time.time() + secs
while time.time() < end:
    tq = struct.unpack_from("<q", m, 0x200 + 0x68)[0]
    fr = struct.unpack_from("<Q", m, 0x200 + 0x38)[0]
    now = qpc()
    if tq != last_tick:
        if last_tick is not None and tq > last_tick:
            iv = (tq - last_tick) * 1000 / F
            arrive_late = (now - tq) * 1000 / F
            ticks.append((iv, arrive_late))
            if iv > 70 or arrive_late > 8:
                stalls.append((time.strftime('%H:%M:%S'), round(iv, 1), round(arrive_late, 1)))
        last_tick = tq
    if fr != last_frame:
        gap = (now - last_frame_t) * 1000 / F
        if last_frame is not None and gap > 20:
            frame_stalls.append((time.strftime('%H:%M:%S'), round(gap, 1)))
        last_frame = fr; last_frame_t = now
    time.sleep(0.0005)
ivs = sorted(t[0] for t in ticks); lates = sorted(t[1] for t in ticks)
if ivs:
    print(f"ticks: {len(ivs)}  interval median {ivs[len(ivs)//2]:.1f}ms p99 {ivs[int(len(ivs)*.99)]:.1f} max {ivs[-1]:.1f}")
    print(f"tick published after its timestamp: median {lates[len(lates)//2]:.1f}ms p99 {lates[int(len(lates)*.99)]:.1f} max {lates[-1]:.1f}")
print(f"tick anomalies (interval>70ms or published >8ms late): {len(stalls)}")
for s in [x for x in stalls if x[1] > 70][:20]: print("   BIG", s)
for s in stalls[:10]: print("  ", s)
print(f"MC frame stalls >20ms: {len(frame_stalls)}")
for s in frame_stalls[:40]: print("  ", s)
