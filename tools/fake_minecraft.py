#!/usr/bin/env python3
"""Stand-in for the Minecraft side, for testing the GTA IV plugin (LibertyCraft.asi) without Minecraft.

Opens the bridge file, waits for the host's magic, then at 60 Hz plays a Minecraft player that is
in a world and on the ground: it acknowledges every teleport (SkyState.teleportSeq) by jumping to
the host's position, optionally walks a slow circle around it, takes yaw/pitch from SkyState,
stamps 20 Hz ticks like the real mod (tickQpc in 100 ns units of CLOCK_MONOTONIC_RAW) and beats
its heartbeat. It prints every input event the host pushes and a summary of the collision it
drains, and with --demo-section puts a block atlas and a few cubes at the player's feet into the
render ring.

    python3 tools/fake_minecraft.py [--seconds N] [--circle R] [--no-ack] [--screen]
                                    [--third-person] [--demo-section] [--link PATH] [-v]

Byte layout: protocol/libertycraft_protocol.h (SkyCraft v11 layout). Stdlib only.
"""

import argparse
import math
import mmap
import os
import struct
import sys
import time

MAGIC = 0x5954424C  # "LBTY"
VERSION = 11
BRIDGE = os.environ.get("LIBERTYCRAFT_LINK", "/dev/shm/libertycraft-bridge")

OFF_SKY = 0x100
OFF_MC = 0x200
OFF_OVL = 0x300
OFF_IN = 0x1000
OFF_EVENTS = 0x17000
OFF_COL = 0x20000
COL_BYTES = 32 << 20
OFF_PIX = OFF_COL + COL_BYTES
SLOT = 3840 * 2160 * 4
OFF_RENDER = OFF_PIX + SLOT * 3
RENDER_BYTES = 64 << 20
SIZE = OFF_RENDER + RENDER_BYTES
COL_DATA = COL_BYTES - 0x80
REN_DATA = RENDER_BYTES - 0x80
IN_ENTRIES = 4096

# McFlags
MC_IN_WORLD, MC_SCREEN_OPEN, MC_ON_GROUND = 1, 2, 4
# SkyFlags
SKY_FLAGS = {1: "InGame", 2: "MenuOpen", 4: "Loading"}

SKY_FMT = "<IIIIdddffIIIf"
MC_FMT = "<II3d4f2IQ3fI3dq3d3d7fIIf"
assert struct.calcsize(SKY_FMT) == 0x40
assert struct.calcsize(MC_FMT) == 0xC8

INPUT_TYPES = {1: "Key", 2: "MouseButton", 3: "Scroll", 4: "Cursor", 5: "Text", 6: "ReleaseAll", 7: "Hurt", 8: "OpenMenu"}
COL_TYPES = {0: "Pad", 1: "Clear", 2: "Region", 3: "Tris"}

# SDL3 scancodes -> names, for printing kInKey.
SDL_NAMES = {i: chr(ord("A") + i - 4) for i in range(4, 30)}
SDL_NAMES.update({30 + i: str(i + 1) for i in range(9)})
SDL_NAMES.update({39: "0", 40: "Enter", 41: "Esc", 42: "Backspace", 43: "Tab", 44: "Space", 45: "-", 46: "=", 47: "[", 48: "]", 49: "\\",
                  51: ";", 52: "'", 53: "`", 54: ",", 55: ".", 56: "/", 57: "CapsLock", 79: "Right", 80: "Left", 81: "Down", 82: "Up",
                  224: "LCtrl", 225: "LShift", 226: "LAlt", 227: "LWin", 228: "RCtrl", 229: "RShift", 230: "RAlt"})
SDL_NAMES.update({58 + i: f"F{i + 1}" for i in range(12)})


def now_ms():
    # Wine's GetTickCount64 == CLOCK_MONOTONIC_RAW in ms (what the host stamps its heartbeat with).
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW) // 1_000_000


def now_qpc():
    # Wine's QueryPerformanceCounter == CLOCK_MONOTONIC_RAW / 100 ns (10 MHz).
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW) // 100


def open_bridge(path):
    """Map the bridge read-write, creating or growing it if needed. Never shrinks it."""
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        if os.fstat(fd).st_size < SIZE:
            os.ftruncate(fd, SIZE)
        return mmap.mmap(fd, SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    finally:
        os.close(fd)


class Bridge:
    def __init__(self, path):
        self.m = open_bridge(path)
        self.mc_seq = 0
        self.frame = 0

    def u32(self, off):
        return struct.unpack_from("<I", self.m, off)[0]

    def u64(self, off):
        return struct.unpack_from("<Q", self.m, off)[0]

    def put_u64(self, off, value):
        struct.pack_into("<Q", self.m, off, value)

    def header(self):
        return struct.unpack_from("<IIIIQQ", self.m, 0)  # magic, version, hostPid, mcPid, hostBeat, mcBeat

    def heartbeat(self):
        struct.pack_into("<I", self.m, 0x0C, os.getpid())
        self.put_u64(0x18, now_ms())

    def read_sky(self):
        for _ in range(64):
            s1 = self.u32(OFF_SKY)
            if s1 & 1:
                continue
            raw = bytes(self.m[OFF_SKY:OFF_SKY + 0x40])
            if self.u32(OFF_SKY) == s1:
                return struct.unpack(SKY_FMT, raw)
        return None

    def write_mc(self, fields):
        """fields: every McState member after seq, in order."""
        self.mc_seq += 1
        struct.pack_into("<I", self.m, OFF_MC, self.mc_seq * 2 - 1)
        body = struct.pack(MC_FMT, 0, *fields)[4:]
        self.m[OFF_MC + 4:OFF_MC + 0xC8] = body
        struct.pack_into("<I", self.m, OFF_MC, self.mc_seq * 2)

    def drain_input(self):
        head, tail = self.u64(OFF_IN), self.u64(OFF_IN + 0x40)
        events = []
        if head - tail > IN_ENTRIES:
            print(f"  input ring: lost {head - tail - IN_ENTRIES} events")
            tail = head - IN_ENTRIES
        while tail < head:
            events.append(struct.unpack_from("<HHiii", self.m, OFF_IN + 0x80 + (tail % IN_ENTRIES) * 16))
            tail += 1
        self.put_u64(OFF_IN + 0x40, tail)
        return events

    def drain_collision(self, stats, verbose):
        head, tail = self.u64(OFF_COL), self.u64(OFF_COL + 0x40)
        base = OFF_COL + 0x80
        while tail < head:
            pos = tail % COL_DATA
            typ, n = struct.unpack_from("<II", self.m, base + pos)
            if typ == 0:
                tail += COL_DATA - pos
                stats["pads"] += 1
                continue
            p = base + pos + 8
            if typ == 1:
                (epoch,) = struct.unpack_from("<I", self.m, p)
                print(f"  collision: Clear epoch {epoch}")
                stats["clears"] += 1
            elif typ in (2, 3):
                mnx, mny, mnz, mxx, mxy, mxz, epoch, count = struct.unpack_from("<iiiiiiII", self.m, p)
                if typ == 2:
                    stats["regions"] += 1
                    stats["blocks"] += count
                    if count:
                        x, y, z = struct.unpack_from("<iii", self.m, p + 32)
                        stats["last"] = f"region ({mnx},{mny},{mnz})..({mxx},{mxy},{mxz}) epoch {epoch}: {count} blocks, first at ({x},{y},{z})"
                else:
                    stats["tri_msgs"] += 1
                    stats["tris"] += count
                    if count:
                        v = struct.unpack_from("<9fI", self.m, p + 32)
                        stats["last_tri"] = f"tri ({v[0]:.1f},{v[1]:.2f},{v[2]:.1f}) ({v[3]:.1f},{v[4]:.2f},{v[5]:.1f}) ({v[6]:.1f},{v[7]:.2f},{v[8]:.1f}) flags {v[9]:#x}"
                if verbose:
                    print(f"  collision: {COL_TYPES[typ]} ({mnx},{mny},{mnz})..({mxx},{mxy},{mxz}) epoch {epoch} count {count}")
            else:
                print(f"  collision: unknown message type {typ} ({n} bytes)")
            tail += (8 + n + 7) & ~7
            stats["bytes"] += (8 + n + 7) & ~7
        self.put_u64(OFF_COL + 0x40, tail)

    def write_render(self, typ, payload):
        """Render ring producer (Minecraft's side): like the host's collision writer, with pad records."""
        n = len(payload)
        msg = (8 + n + 7) & ~7
        if msg > REN_DATA // 2:
            raise ValueError("render message too large")
        base = OFF_RENDER + 0x80
        for _ in range(2000):
            head, tail = self.u64(OFF_RENDER), self.u64(OFF_RENDER + 0x40)
            pos = head % REN_DATA
            pad = REN_DATA - pos if pos + msg > REN_DATA else 0
            if REN_DATA - (head - tail) >= msg + pad:
                break
            time.sleep(0.001)
        else:
            print("  render ring full: the host isn't draining it")
            return False
        if pad:
            struct.pack_into("<II", self.m, base + pos, 0, 0)
            head += pad
            pos = 0
        struct.pack_into("<II", self.m, base + pos, typ, n)
        self.m[base + pos + 8:base + pos + 8 + n] = payload
        self.put_u64(OFF_RENDER, head + msg)
        return True


def describe_input(typ, code, a, b, c):
    name = INPUT_TYPES.get(typ, f"type{typ}")
    if typ == 1:
        return f"Key {SDL_NAMES.get(code, code)} (sdl {code}) {'down' if a else 'up'}"
    if typ == 2:
        return f"MouseButton {({1: 'L', 2: 'M', 3: 'R', 4: 'X1', 5: 'X2'}).get(code, code)} {'down' if a else 'up'}"
    if typ == 3:
        return f"Scroll {a}"
    if typ == 4:
        return f"Cursor {a},{b}"
    if typ == 5:
        return f"Text U+{a:04X} {chr(a)!r}"
    if typ == 7:
        return f"Hurt kind {code} damage {a / 100:.2f} attacker {b:08X} flags {c:#x}"
    return name


def cube_vertices(lx, ly, lz):
    """36 RenVertex for a unit cube at section-local block (lx, ly, lz), counter-clockwise from outside."""
    faces = [  # (direction ordinal, 4 corners)
        (0, [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)]),  # down
        (1, [(0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)]),  # up
        (2, [(0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)]),  # north (-z)
        (3, [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]),  # south (+z)
        (4, [(0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)]),  # west (-x)
        (5, [(1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)]),  # east (+x)
    ]
    uvs = [(0.0, 1.0), (0.0, 0.0), (1.0, 0.0), (1.0, 1.0)]
    out = b""
    for ordinal, corners in faces:
        for k in (0, 1, 2, 0, 2, 3):
            x, y, z = corners[k]
            u, v = uvs[k]
            out += struct.pack("<5fIII", lx + x, ly + y, lz + z, u, v, 0xFFFFFFFF, 15 | (15 << 8), (ordinal + 1) << 4)
    return out


def demo_section(bridge, feet):
    w = h = 32
    pixels = bytearray()
    for y in range(h):
        for x in range(w):
            on = ((x // 8) + (y // 8)) % 2 == 0
            pixels += bytes((230, 60, 200, 255) if on else (30, 30, 30, 255))
    bridge.write_render(1, struct.pack("<II", w, h) + bytes(pixels))
    fx, fy, fz = (math.floor(c) for c in feet)
    blocks = [(fx + 2, fy, fz), (fx + 2, fy + 1, fz), (fx, fy, fz + 2), (fx - 2, fy, fz - 1)]
    sections = {}
    for bx, by, bz in blocks:
        s = (bx >> 4, by >> 4, bz >> 4)
        sections.setdefault(s, b"")
        sections[s] += cube_vertices(bx - s[0] * 16, by - s[1] * 16, bz - s[2] * 16)
    for (sx, sy, sz), verts in sections.items():
        bridge.write_render(2, struct.pack("<iiiI", sx, sy, sz, len(verts) // 32) + verts)
    print(f"demo: wrote a 32x32 atlas and {len(blocks)} cubes in {len(sections)} section(s) around MC block {fx},{fy},{fz}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--link", default=BRIDGE, help="bridge file (default %(default)s)")
    ap.add_argument("--seconds", type=float, default=0, help="run time, 0 = until Ctrl+C")
    ap.add_argument("--circle", type=float, default=0.0, help="walk a circle of this radius (blocks) around the teleport point")
    ap.add_argument("--no-ack", action="store_true", help="never acknowledge teleports (the host must not start puppeting)")
    ap.add_argument("--screen", action="store_true", help="report a Minecraft screen open (cursor + text input)")
    ap.add_argument("--third-person", action="store_true", help="report F5 third-person camera, 4 blocks back")
    ap.add_argument("--demo-section", action="store_true", help="write a block atlas and a few cubes at the feet into the render ring")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every collision message")
    args = ap.parse_args()

    bridge = Bridge(args.link)
    print(f"fake Minecraft pid {os.getpid()} on {args.link} ({SIZE >> 20} MiB); waiting for the host's magic...")
    while True:
        magic, version, host_pid, _, beat, _ = bridge.header()
        if magic == MAGIC:
            print(f"host found: pid {host_pid}, protocol v{version}{'' if version == VERSION else ' (MISMATCH, expected 11)'}, "
                  f"heartbeat {now_ms() - beat} ms old")
            break
        bridge.heartbeat()
        time.sleep(0.25)

    start = time.monotonic()
    ack = 0
    origin = None  # MC feet after the last teleport
    pos = cur = prev = None
    tick_qpc = 0
    next_tick = 0.0
    walk = walk_o = 0.0
    last_print = 0.0
    last_sky = None
    demo_done = False
    last_host_pid = host_pid
    stats = dict(clears=0, regions=0, blocks=0, tri_msgs=0, tris=0, pads=0, bytes=0, last="", last_tri="")
    try:
        while args.seconds <= 0 or time.monotonic() - start < args.seconds:
            t = time.monotonic()
            bridge.heartbeat()
            magic, _, host_pid, mc_pid, host_beat, _ = bridge.header()
            if host_pid != last_host_pid or mc_pid == 0:
                print(f"host (re)started: pid {last_host_pid} -> {host_pid}")
                last_host_pid = host_pid
                bridge.heartbeat()

            sky = bridge.read_sky()
            if sky:
                _, flags, world, epoch, x, y, z, yaw, pitch, tp, vw, vh, hour = sky
                if tp != ack and not args.no_ack and (flags & 1):
                    origin = (x, y, z)
                    pos = cur = prev = origin
                    ack = tp
                    print(f"teleport #{tp} acknowledged: MC {x:.2f} {y:.2f} {z:.2f} (world {world}, epoch {epoch})")
                if args.demo_section and not demo_done and (flags & 1):
                    demo_section(bridge, (x, y, z))
                    demo_done = True
            else:
                flags = 0

            # 20 Hz ticks: the feet move along a circle (or stand still) on the tick rhythm.
            if origin and t >= next_tick:
                next_tick = t + 0.05 if t - next_tick > 0.1 else next_tick + 0.05
                prev = cur
                walk_o = walk
                if args.circle > 0:
                    ang = (t - start) * 0.5  # rad/s
                    cur = (origin[0] + args.circle * math.cos(ang), origin[1], origin[2] + args.circle * math.sin(ang))
                    walk += math.dist(prev, cur) * 0.6
                else:
                    cur = origin
                tick_qpc = now_qpc()
            if origin:
                f = min(1.0, (now_qpc() - tick_qpc) / 500_000) if tick_qpc else 1.0
                pos = tuple(p + (c - p) * f for p, c in zip(prev, cur))
                in_world = MC_IN_WORLD | MC_ON_GROUND | (MC_SCREEN_OPEN if args.screen else 0)
                syaw, spitch = (sky[7], sky[8]) if sky else (0.0, 0.0)
                eye = 1.62
                bridge.frame += 1
                bridge.write_mc((
                    in_world, pos[0], pos[1], pos[2], syaw, spitch, eye, 0.5, ack, 2, bridge.frame, 70.0, walk, 0.0, 0,
                    pos[0], pos[1] + eye, pos[2], tick_qpc, prev[0], prev[1], prev[2], cur[0], cur[1], cur[2],
                    eye, eye, walk_o, walk, 0.0, 0.0, 50.0, 0, 1 if args.third_person else 0, 4.0 if args.third_person else 0.0))

            for typ, code, a, b, c in bridge.drain_input():
                print(f"  input: {describe_input(typ, code, a, b, c)}")
            bridge.drain_collision(stats, args.verbose)

            if t - last_print >= 1.0:
                last_print = t
                if sky:
                    fl = "|".join(n for bit, n in SKY_FLAGS.items() if sky[1] & bit) or "-"
                    line = (f"sky: flags {fl} world {sky[2]} epoch {sky[3]} pos {sky[4]:.2f} {sky[5]:.2f} {sky[6]:.2f} yaw {sky[7]:.1f} "
                            f"pitch {sky[8]:.1f} tp #{sky[9]} viewport {sky[10]}x{sky[11]} hour {sky[12]:.2f}; host heartbeat "
                            f"{now_ms() - host_beat} ms old")
                    if line != last_sky:
                        print(line)
                        last_sky = line
                if stats["regions"] or stats["tri_msgs"] or stats["clears"]:
                    print(f"collision (1 s): {stats['regions']} regions / {stats['blocks']} blocks, {stats['tri_msgs']} tri msgs / "
                          f"{stats['tris']} tris, {stats['pads']} pads, {stats['bytes'] >> 10} KiB")
                    if stats["last"]:
                        print(f"  last {stats['last']}")
                    if stats["last_tri"]:
                        print(f"  last {stats['last_tri']}")
                    for k in stats:
                        stats[k] = "" if isinstance(stats[k], str) else 0
            time.sleep(max(0.0, 1 / 60 - (time.monotonic() - t)))
    except KeyboardInterrupt:
        pass
    print("fake Minecraft exiting (the bridge file stays)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
