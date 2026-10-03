#!/usr/bin/env python3
"""Stand-in for the Minecraft side, for testing the GTA IV plugin (LibertyCraft.asi) without Minecraft.

Opens the bridge file, waits for the host's magic, then at 60 Hz plays a Minecraft player that is
in a world and on the ground: it acknowledges every teleport (SkyState.teleportSeq) by jumping to
the host's position, optionally walks a slow circle around it, takes yaw/pitch from SkyState,
stamps 20 Hz ticks like the real mod (tickQpc in 100 ns units of CLOCK_MONOTONIC_RAW) and beats
its heartbeat. It prints every input event the host pushes and a summary of the collision it
drains, and with --demo-section puts a block atlas, a pillar and a tower of blocks next to the player,
a selection outline with cracks, a dropped item and an entity (plus a body with --demo-avatar) into
the render ring.

    python3 tools/fake_minecraft.py [--seconds N] [--circle R] [--no-ack] [--screen]
                                    [--third-person] [--demo-section [--demo-avatar]] [--link PATH] [-v]
                                    [--actors] [--hit-nearest-actor DMG] [--explode-ahead [EVERY]]
                                    [--die-after S] [--combat-delay S] [--combat-interval S]
                                    [--walk-to X Y Z [--walk-via X Y Z ...] [--walk-pause S]
                                     [--walk-speed B] [--walk-delay S] [--walk-back S]]

Walking (Stream P, doors and street furniture): --walk-to moves the player in straight lines (no
collision) from the first teleport point through the --walk-via points to X Y Z, standing
--walk-pause s at each via point. A teleport that lands within 3 blocks of the walker (a world
change on the way) keeps it on its path.

Combat (Stream X): --actors prints the host's ActorTable every 2 s; --hit-nearest-actor sends
kEvHitActor (DMG Minecraft damage, sword, base knockback) to the living actor nearest the player
every --combat-interval s; --explode-ahead sends a TNT kEvExplosion 6 blocks in front of the player
(once, or every EVERY s); --die-after sends kEvPlayerDied. They start --combat-delay s after the
first teleport acknowledgement.

NPCs vs blocks and vehicles (Stream Q2): --wall-ring R puts a square ring of stone blocks R blocks out
around the player (as meshes and kRenSolids; it moves along when the player is teleported far), which
GTA IV's peds and traffic should not get through; --hit-kind vehicle aims --hit-nearest-actor at the
nearest vehicle piece, and --hit-projectile makes the hits arrows (they also reach the people inside).

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
OFF_ACTORS = 0x12000
EV_ENTRIES = 512
EV_HIT_ACTOR, EV_PLAYER_DIED, EV_EXPLOSION, EV_HIT_POINT = 1, 2, 3, 6
ACTOR_FMT = "<II7fHH24s"  # formId, flags, x, y, z, yaw, width, height, healthFrac, level, pad, name
ACTOR_DEAD = 2
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
assert struct.calcsize(ACTOR_FMT) == 64

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
        # Pack first: the host spins only briefly on an odd seq, so keep the write window tiny.
        body = struct.pack(MC_FMT, 0, *fields)[4:]
        self.mc_seq += 1
        struct.pack_into("<I", self.m, OFF_MC, self.mc_seq * 2 - 1)
        self.m[OFF_MC + 4:OFF_MC + 0xC8] = body
        struct.pack_into("<I", self.m, OFF_MC, self.mc_seq * 2)

    def read_actors(self):
        """The host's ActorTable (seqlock): a list of (formId, flags, x, y, z, yaw, w, h, healthFrac, name), or None."""
        for _ in range(64):
            s1 = self.u32(OFF_ACTORS)
            if s1 & 1:
                continue
            count = min(self.u32(OFF_ACTORS + 4), 256)
            raw = bytes(self.m[OFF_ACTORS + 0x40:OFF_ACTORS + 0x40 + 64 * count])
            if self.u32(OFF_ACTORS) == s1:
                out = []
                for i in range(count):
                    f = struct.unpack_from(ACTOR_FMT, raw, i * 64)
                    out.append(f[:9] + (f[11].split(b"\0")[0].decode(errors="replace"),))
                return out
        return None

    def push_event(self, typ, form, a=0.0, b=0.0, c=0.0, d=0.0, flags=0, weapon=0):
        """McEvent into the event ring (Minecraft produces, the host consumes)."""
        head, tail = self.u64(OFF_EVENTS), self.u64(OFF_EVENTS + 0x40)
        if head - tail >= EV_ENTRIES:
            return False
        struct.pack_into("<II4fII", self.m, OFF_EVENTS + 0x80 + (head % EV_ENTRIES) * 32, typ, form, a, b, c, d, flags, weapon)
        self.put_u64(OFF_EVENTS, head + 1)
        return True

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


# Demo atlas: 4 x 2 tiles of 16 px (column, row).
TILE_GRASS_TOP, TILE_GRASS_SIDE, TILE_DIRT, TILE_STONE, TILE_GLASS, TILE_BLUE, TILE_MARKER, TILE_CRACK = range(8)
OFF_WORLD_ENTITIES = 0x1C000
WE_FMT = "<II3f2ff3f12fI"  # kind, id, x, y, z, yaw, pitch, scale, ext[3], uv[3][4], tint
assert struct.calcsize(WE_FMT) == 96


def tile_uv(tile):
    """Atlas rect {u0, v0, u1, v1} of a demo tile."""
    c, r = tile % 4, tile // 4
    return (c / 4, r / 2, (c + 1) / 4, (r + 1) / 2)


def demo_atlas():
    """64x32 RGBA8, top row first: grass top/side, dirt, stone, glass (cutout), blue (translucent), marker, cracks."""
    w, h = 64, 32
    px = bytearray(w * h * 4)
    rnd = 12345

    def noise():
        nonlocal rnd
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        return (rnd >> 16) % 32

    for y in range(h):
        for x in range(w):
            tile, tx, ty = (x // 16) + 4 * (y // 16), x % 16, y % 16
            n = noise()
            dirt = (134 - n, 96 - n // 2, 67 - n // 2, 255)
            if tile == TILE_GRASS_TOP:
                c = (95 - n, 159 - n, 53 - n // 2, 255)
            elif tile == TILE_GRASS_SIDE:
                c = (95 - n, 159 - n, 53 - n // 2, 255) if ty < 4 - (tx * 7 % 3) else dirt
            elif tile == TILE_DIRT:
                c = dirt
            elif tile == TILE_STONE:
                c = (125 - n, 125 - n, 125 - n, 255)
            elif tile == TILE_GLASS:
                edge = tx in (0, 15) or ty in (0, 15)
                c = (220, 240, 255, 255) if edge else (0, 0, 0, 0)
            elif tile == TILE_BLUE:
                c = (40, 90, 230, 140)
            elif tile == TILE_MARKER:
                c = (230, 60, 200, 255) if ((tx // 4) + (ty // 4)) % 2 == 0 else (30, 30, 30, 255)
            else:
                c = (20, 20, 20, 200) if n < 9 else (0, 0, 0, 0)
            px[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(c)
    return w, h, bytes(px)


# Each face: Direction ordinal and its corners BL, BR, TR, TL seen from outside (counter-clockwise).
CUBE_FACES = [
    (0, [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)]),  # down
    (1, [(0, 1, 1), (1, 1, 1), (1, 1, 0), (0, 1, 0)]),  # up
    (2, [(1, 0, 0), (0, 0, 0), (0, 1, 0), (1, 1, 0)]),  # north (-z)
    (3, [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]),  # south (+z)
    (4, [(0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)]),  # west (-x)
    (5, [(1, 0, 1), (1, 0, 0), (1, 1, 0), (1, 1, 1)]),  # east (+x)
]
FACE_UV = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]


def cube_vertices(lx, ly, lz, tiles=(TILE_MARKER,) * 3, size=1.0, translucent=False, light=15 | (15 << 8), normals=True):
    """36 RenVertex for a cube at (lx, ly, lz) (section- or origin-relative), tiles = (side, top, bottom);
    quads as triangles (0 1 2) (0 2 3), counter-clockwise from outside, like the real mod."""
    out = b""
    for ordinal, corners in CUBE_FACES:
        tile = tiles[1] if ordinal == 1 else tiles[2] if ordinal == 0 else tiles[0]
        u0, v0, u1, v1 = tile_uv(tile)
        flags = (2 if translucent else 1) | (((ordinal + 1) << 4) if normals else 0)
        for k in (0, 1, 2, 0, 2, 3):
            x, y, z = corners[k]
            fu, fv = FACE_UV[k]
            out += struct.pack("<5fIII", lx + x * size, ly + y * size, lz + z * size, u0 + (u1 - u0) * fu, v0 + (v1 - v0) * fv,
                               0xFFFFFFFF, light, flags)
    return out


def facing_axes(yaw):
    """Minecraft yaw -> the nearest block axes (forward, right) as integer (x, z) pairs."""
    fx, fz = -math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    f = (1 if fx > 0 else -1, 0) if abs(fx) >= abs(fz) else (0, 1 if fz > 0 else -1)
    return f, (-f[1], f[0])


def demo_blocks(fx, fy, fz, yaw=0.0):
    """(x, y, z, (side, top, bottom), translucent) in front of the player (yaw): a 3x3 grass-topped stone
    pillar 4 blocks ahead (5 high, glass and a blue translucent block on top), and a 3x3 marker tower 25 ahead
    and 6 to the right, 30 high."""
    (ax, az), (rx, rz) = facing_axes(yaw)
    at = lambda ahead, right: (fx + ax * ahead + rx * right, fz + az * ahead + rz * right)
    blocks = []
    for a in range(4, 7):
        for r in range(-1, 2):
            x, z = at(a, r)
            for dy in range(5):
                top = dy == 4
                tiles = (TILE_GRASS_SIDE, TILE_GRASS_TOP, TILE_DIRT) if top else (TILE_STONE,) * 3
                blocks.append((x, fy + dy, z, tiles, False))
    x, z = at(5, 0)
    blocks.append((x, fy + 5, z, (TILE_GLASS,) * 3, False))
    blocks.append((x, fy + 6, z, (TILE_BLUE,) * 3, True))
    for a in range(25, 28):
        for r in range(5, 8):
            x, z = at(a, r)
            for dy in range(30):
                tile = TILE_MARKER if dy % 5 == 4 else TILE_STONE
                blocks.append((x, fy + dy, z, (tile,) * 3, False))
    return blocks


def demo_entities(bridge, fx, fy, fz, yaw=0.0):
    """WorldEntities: the selection outline and cracks on the pillar's front top block, a dropped item 2 ahead."""
    (ax, az), _ = facing_axes(yaw)
    sel = (fx + ax * 4, fy + 4, fz + az * 4)
    crack = struct.pack(WE_FMT, 5, 1, *sel, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, *tile_uv(TILE_CRACK), *(0.0,) * 8, 0)
    item = struct.pack(WE_FMT, 2, 2, fx + ax * 2 + 0.5, fy + 0.5, fz + az * 2 + 0.5, 30.0, 0.0, 0.5, 0.0, 0.0, 0.0, *tile_uv(TILE_MARKER),
                       *(0.0,) * 8, 0)
    head = struct.pack("<III3f3f", 0, 2, 1, sel[0], sel[1], sel[2], sel[0] + 1, sel[1] + 1, sel[2] + 1).ljust(0x40, b"\0")
    seq = struct.unpack_from("<I", bridge.m, OFF_WORLD_ENTITIES)[0]
    seq += seq & 1
    struct.pack_into("<I", bridge.m, OFF_WORLD_ENTITIES, seq + 1)
    bridge.m[OFF_WORLD_ENTITIES + 4:OFF_WORLD_ENTITIES + 0x40] = head[4:]
    bridge.m[OFF_WORLD_ENTITIES + 0x40:OFF_WORLD_ENTITIES + 0x40 + 192] = crack + item
    struct.pack_into("<I", bridge.m, OFF_WORLD_ENTITIES, seq + 2)


def demo_meshes(bridge, feet, avatar, yaw=0.0):
    """kRenScene: a small marker cube over the pillar (atlas texture); with avatar, kRenAvatar: a
    0.6 x 1.8 x 0.6 box at the feet in an entity texture (id 7, red/white)."""
    fx, fy, fz = (math.floor(c) for c in feet)
    (ax, az), _ = facing_axes(yaw)
    verts = cube_vertices(-0.25, 0.0, -0.25, size=0.5, normals=False)
    payload = struct.pack("<dddII", fx + ax * 5 + 0.5, fy + 8.0, fz + az * 5 + 0.5, 1, len(verts) // 32) + struct.pack("<IIII", 0, 0, len(verts) // 32, 0) + verts
    bridge.write_render(6, payload)
    if avatar:
        w = h = 16
        tex = b"".join(bytes((220, 40, 40, 255) if ((x // 4) + (y // 4)) % 2 else (240, 240, 240, 255)) for y in range(h) for x in range(w))
        if not getattr(bridge, "demo_tex_sent", False):
            bridge.write_render(4, struct.pack("<IIII", 7, w, h, 0) + tex)
            bridge.demo_tex_sent = True
        body = b""
        for ordinal, corners in CUBE_FACES:
            for k in (0, 1, 2, 0, 2, 3):
                x, y, z = corners[k]
                fu, fv = FACE_UV[k]
                body += struct.pack("<5fIII", (x - 0.5) * 0.6, y * 1.8, (z - 0.5) * 0.6, fu, fv, 0xFFFFFFFF, 15 | (15 << 8), 1 | ((ordinal + 1) << 4))
        bridge.write_render(5, struct.pack("<II", 1, len(body) // 32) + struct.pack("<IIII", 7, 0, len(body) // 32, 0) + body)


def demo_section(bridge, feet, yaw=0.0):
    w, h, pixels = demo_atlas()
    bridge.write_render(1, struct.pack("<II", w, h) + pixels)
    fx, fy, fz = (math.floor(c) for c in feet)
    blocks = demo_blocks(fx, fy, fz, yaw)
    sections = {}
    for bx, by, bz, tiles, translucent in blocks:
        s = (bx >> 4, by >> 4, bz >> 4)
        sections.setdefault(s, [b"", b""])
        sections[s][1 if translucent else 0] += cube_vertices(bx - s[0] * 16, by - s[1] * 16, bz - s[2] * 16, tiles, translucent=translucent)
    for (sx, sy, sz), (solid, blended) in sections.items():
        verts = solid + blended
        bridge.write_render(2, struct.pack("<iiiI", sx, sy, sz, len(verts) // 32) + verts)
    demo_entities(bridge, fx, fy, fz, yaw)
    print(f"demo: wrote a {w}x{h} atlas and {len(blocks)} blocks in {len(sections)} section(s) around MC block {fx},{fy},{fz} "
          f"(yaw {yaw:.0f}: pillar 4 ahead, tower 25 ahead 6 right), a selection outline + cracks and a dropped item")


def demo_load(bridge, feet, count):
    """A load test: count terrain-like sections (a 16x16 grass floor 2 blocks under the feet with a stone
    step every 4 blocks: ~330 quads each) on a square grid around the player, the same mesh in each."""
    fx, fy, fz = (math.floor(c) for c in feet)
    y = fy - 2
    sy, ly = y >> 4, y - (y >> 4) * 16
    mesh = b""
    for x in range(16):
        for z in range(16):
            grass = (TILE_GRASS_SIDE, TILE_GRASS_TOP, TILE_DIRT)
            if x % 4 == 0 and z % 4 == 0:
                mesh += cube_vertices(x, ly + 1, z, (TILE_STONE,) * 3) if ly < 15 else b""
            # only the top face of the floor (like a real surface section, hidden faces culled)
            top = cube_vertices(x, ly, z, grass)[36 * 32 // 6 * 1:36 * 32 // 6 * 2]
            mesh += top
    side = math.ceil(math.sqrt(count))
    sx0, sz0 = (fx >> 4) - side // 2, (fz >> 4) - side // 2
    n = 0
    for i in range(side):
        for k in range(side):
            if n >= count:
                break
            bridge.write_render(2, struct.pack("<iiiI", sx0 + i, sy, sz0 + k, len(mesh) // 32) + mesh)
            n += 1
    print(f"demo: load test wrote {n} sections ({len(mesh) // 32 // 6} quads, {len(mesh) >> 10} KiB each) around section "
          f"{fx >> 4},{sy},{fz >> 4}")


REN_SOLIDS = 10
ACTOR_VEHICLE = 1 << 4
HIT_PROJECTILE = 1 << 1


def wall_ring(bridge, feet, radius, height, drop=1):
    """Stream Q2's NPC test: a square ring of stone blocks (radius blocks out from the feet, height high,
    from drop blocks below the feet's block row: the fake doesn't fall, so after a warp its feet can hang
    a metre above the street) as section meshes (so it shows) and kRenSolids bitsets (so GTA IV's
    peds and vehicles collide with it). Any road through the middle crosses it twice."""
    w, h, pixels = demo_atlas()
    bridge.write_render(1, struct.pack("<II", w, h) + pixels)
    fx, fy, fz = (math.floor(c) for c in feet)
    fy -= drop
    cells = set()
    for i in range(-radius, radius + 1):
        for x, z in ((fx + i, fz - radius), (fx + i, fz + radius), (fx - radius, fz + i), (fx + radius, fz + i)):
            for dy in range(height):
                cells.add((x, fy + dy, z))
    sections = {}
    for bx, by, bz in cells:
        sections.setdefault((bx >> 4, by >> 4, bz >> 4), []).append((bx, by, bz))
    for (sx, sy, sz), blocks in sections.items():
        verts = b"".join(cube_vertices(bx - sx * 16, by - sy * 16, bz - sz * 16, (TILE_STONE,) * 3) for bx, by, bz in blocks)
        bridge.write_render(2, struct.pack("<iiiI", sx, sy, sz, len(verts) // 32) + verts)
        bits = bytearray(512)
        for bx, by, bz in blocks:
            bit = (bx - sx * 16) + 16 * (bz - sz * 16) + 256 * (by - sy * 16)
            bits[bit >> 3] |= 1 << (bit & 7)
        bridge.write_render(REN_SOLIDS, struct.pack("<iiiI", sx, sy, sz, len(blocks)) + bytes(bits))
    print(f"wall ring: {len(cells)} blocks ({2 * radius + 1}x{2 * radius + 1}, {height} high) in {len(sections)} sections around MC block "
          f"{fx},{fy},{fz}, sent as meshes + kRenSolids")


def combat_step(bridge, args, sky, t, t0, st):
    """The combat flags, once per loop. st holds next_hit / next_blast / next_actors / died_sent."""
    px, py, pz, yaw = sky[4], sky[5], sky[6], sky[7]
    want_actors = args.actors or args.hit_nearest_actor > 0
    actors = bridge.read_actors() if want_actors else None
    if args.actors and actors is not None and t >= st["next_actors"]:
        st["next_actors"] = t + 2.0
        near = sorted(actors, key=lambda r: math.dist((r[2], r[3], r[4]), (px, py, pz)))
        desc = ", ".join(f"{r[9]} {r[0]:08X} {math.dist((r[2], r[3], r[4]), (px, py, pz)):.1f}m hp{r[8]:.2f}{' DEAD' if r[1] & ACTOR_DEAD else ''}"
                         f"{f' {r[6]:.1f}x{r[7]:.1f}' if r[1] & ACTOR_VEHICLE else ''}" for r in near[:4])
        vehicles = len({r[0] & ~15 for r in actors if r[1] & ACTOR_VEHICLE})
        print(f"actors: {len(actors)} ({sum(1 for r in actors if r[1] & ACTOR_DEAD)} dead; {vehicles} vehicles in "
              f"{sum(1 for r in actors if r[1] & ACTOR_VEHICLE)} pieces); nearest: {desc}")
    if args.hit_nearest_actor > 0 and actors and t >= st["next_hit"]:
        st["next_hit"] = t + args.combat_interval
        alive = [r for r in actors if not r[1] & ACTOR_DEAD]
        if args.hit_kind != "any":
            alive = [r for r in alive if bool(r[1] & ACTOR_VEHICLE) == (args.hit_kind == "vehicle")]
        if alive:
            r = min(alive, key=lambda r: math.dist((r[2], r[3], r[4]), (px, py, pz)))
            dx, dz = r[2] - px, r[4] - pz
            n = math.hypot(dx, dz) or 1.0
            # The push is the way Minecraft's knockback moves the victim: away from the player.
            st["hits"] = st.get("hits", 0) + 1
            arrow = args.hit_projectile or (args.hit_projectile_every > 0 and st["hits"] % args.hit_projectile_every == 0)
            flags, weapon = (HIT_PROJECTILE, 5) if arrow else (0, 1)
            where = ""
            if r[1] & ACTOR_VEHICLE:
                # kEvHitPoint first: aimed from the eye at the piece's middle, at a fraction of its height
                # (--hit-heights, in turn), the point on that line at the piece's box.
                heights = [float(v) for v in args.hit_heights.split(",")]
                frac = heights[(st["hits"] - 1) % len(heights)]
                tx, ty, tz = r[2], r[3] + r[7] * frac, r[4]
                ex, ey, ez = px, py + 1.62, pz
                lx, ly, lz = tx - ex, ty - ey, tz - ez
                ln = math.sqrt(lx * lx + ly * ly + lz * lz) or 1.0
                lx, ly, lz = lx / ln, ly / ln, lz / ln
                back = r[6] * 0.5  # to about the box's face
                hx, hy, hz = tx - lx * back, ty - ly * back, tz - lz * back
                yaw_l, pitch_l = math.degrees(math.atan2(-lx, lz)), math.degrees(-math.asin(max(-1.0, min(1.0, ly))))
                bridge.push_event(EV_HIT_POINT, r[0], hx, hy, hz, yaw_l, struct.unpack("<I", struct.pack("<f", pitch_l))[0], 0)
                where = f", at {frac:.2f} of its height (yaw {yaw_l:.0f} pitch {pitch_l:.0f})"
            bridge.push_event(EV_HIT_ACTOR, r[0], args.hit_nearest_actor, dx / n, dz / n, 0.4, flags, weapon)
            print(f"combat: hit {r[9]} {r[0]:08X} at {math.dist((r[2], r[3], r[4]), (px, py, pz)):.1f} blocks (health {r[8]:.2f}) "
                  f"for {args.hit_nearest_actor}{' (projectile)' if arrow else ''}, push MC {dx / n:.2f} {dz / n:.2f}{where}")
    if args.explode_ahead is not None and t >= st["next_blast"] and st["next_blast"] >= 0:
        st["next_blast"] = t + args.explode_ahead if args.explode_ahead > 0 else -1.0
        r = math.radians(yaw)
        cx, cz = px - math.sin(r) * 6.0, pz + math.cos(r) * 6.0
        bridge.push_event(EV_EXPLOSION, 0, cx, py + 0.5, cz, 4.0)
        print(f"combat: explosion (radius 4) at MC {cx:.1f} {py + 0.5:.1f} {cz:.1f}, 6 blocks ahead (yaw {yaw:.0f})")
    if args.die_after > 0 and not st["died_sent"] and t >= t0 + args.die_after:
        st["died_sent"] = True
        bridge.push_event(EV_PLAYER_DIED, 0)
        print("combat: the Minecraft player died (kEvPlayerDied)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--link", default=BRIDGE, help="bridge file (default %(default)s)")
    ap.add_argument("--seconds", type=float, default=0, help="run time, 0 = until Ctrl+C")
    ap.add_argument("--circle", type=float, default=0.0, help="walk a circle of this radius (blocks) around the teleport point")
    ap.add_argument("--no-ack", action="store_true", help="never acknowledge teleports (the host must not start puppeting)")
    ap.add_argument("--walk-to", nargs=3, type=float, metavar=("X", "Y", "Z"), default=None,
                    help="walk in a straight line (no collision) from the first teleport point to this MC position")
    ap.add_argument("--walk-via", nargs=3, type=float, metavar=("X", "Y", "Z"), action="append", default=[],
                    help="--walk-to: pass through this MC position first (repeatable, in order)")
    ap.add_argument("--walk-pause", type=float, default=0.0, metavar="S", help="--walk-to: stand S s at every --walk-via point")
    ap.add_argument("--walk-speed", type=float, default=1.5, help="--walk-to speed in blocks/s (default %(default)s)")
    ap.add_argument("--walk-delay", type=float, default=5.0, help="--walk-to starts this many s after the first teleport ack")
    ap.add_argument("--walk-back", type=float, default=-1.0, metavar="S",
                    help="--walk-to: wait S s at the target, then walk back to the start (default: stay)")
    ap.add_argument("--screen", action="store_true", help="report a Minecraft screen open (cursor + text input)")
    ap.add_argument("--third-person", action="store_true", help="report F5 third-person camera, 4 blocks back")
    ap.add_argument("--demo-section", action="store_true",
                    help="write a block atlas, a pillar and a tower of blocks by the feet, a selection outline/cracks/item and a "
                         "scene entity (kRenScene, 1 Hz) into the render ring")
    ap.add_argument("--demo-on-teleport", action="store_true",
                    help="with --demo-section: move the demo (kRenClearAll + resend) when a teleport lands more than 8 blocks away")
    ap.add_argument("--demo-load", type=int, default=0, metavar="N",
                    help="with --demo-section: also N terrain sections (~330 quads each) around the player, a render load test")
    ap.add_argument("--demo-avatar", action="store_true", help="with --demo-section: also a box body (kRenTexture + kRenAvatar) at the feet (use with --third-person)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every collision message")
    ap.add_argument("--actors", action="store_true", help="print the host's actor table every 2 s")
    ap.add_argument("--hit-nearest-actor", type=float, default=0.0, metavar="DMG",
                    help="every --combat-interval s, hit the living actor nearest the player for DMG Minecraft damage")
    ap.add_argument("--explode-ahead", type=float, nargs="?", const=-1.0, default=None, metavar="EVERY",
                    help="a TNT explosion 6 blocks in front of the player (once, or every EVERY s)")
    ap.add_argument("--die-after", type=float, default=0.0, metavar="S", help="send kEvPlayerDied S s after combat starts")
    ap.add_argument("--combat-delay", type=float, default=10.0, metavar="S", help="combat flags start S s after the first teleport ack")
    ap.add_argument("--combat-interval", type=float, default=2.0, metavar="S", help="seconds between --hit-nearest-actor hits")
    ap.add_argument("--hit-kind", choices=("ped", "vehicle", "any"), default="ped",
                    help="--hit-nearest-actor hits the nearest ped (default), vehicle piece or either")
    ap.add_argument("--hit-projectile", action="store_true", help="--hit-nearest-actor hits like an arrow (kHitProjectile) instead of a sword")
    ap.add_argument("--hit-projectile-every", type=int, default=0, metavar="N", help="--hit-nearest-actor: every Nth hit is an arrow")
    ap.add_argument("--hit-heights", default="0.8,0.3", metavar="F,F",
                    help="--hit-kind vehicle: where the hits land (kEvHitPoint), as fractions of the piece's height, in turn "
                         "(default %(default)s: glass, then the door)")
    ap.add_argument("--wall-ring", type=int, default=0, metavar="R",
                    help="a square ring of stone blocks R blocks out from the first teleport point (meshes + kRenSolids): "
                         "GTA IV's peds and vehicles should not get through it")
    ap.add_argument("--wall-height", type=int, default=3, metavar="H", help="--wall-ring height in blocks (default %(default)s)")
    ap.add_argument("--wall-drop", type=int, default=1, metavar="D", help="--wall-ring starts D blocks below the feet's row (default %(default)s)")
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
    demo_feet, demo_yaw = None, 0.0
    next_demo_mesh = 0.0
    last_host_pid = host_pid
    stats = dict(clears=0, regions=0, blocks=0, tri_msgs=0, tris=0, pads=0, bytes=0, last="", last_tri="")
    combat_t0 = None  # when combat flags start (first teleport ack + --combat-delay)
    walk_from, walk_t0 = None, None  # --walk-to: start point and start time
    combat_state = dict(next_hit=0.0, next_blast=0.0, next_actors=0.0, died_sent=False)
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
                    if walk_from and cur and math.dist((x, y, z), cur) < 3.0:
                        pass  # walking: a resync (world change on the way) keeps us on the path
                    else:
                        origin = (x, y, z)
                        pos = cur = prev = origin
                    if args.walk_to and walk_from is None:
                        walk_from, walk_t0 = origin, time.monotonic() + args.walk_delay
                        print(f"walk to MC {args.walk_to[0]:.2f} {args.walk_to[1]:.2f} {args.walk_to[2]:.2f} in {args.walk_delay:.0f} s")
                    ack = tp
                    print(f"teleport #{tp} acknowledged: MC {x:.2f} {y:.2f} {z:.2f} (world {world}, epoch {epoch})")
                    if args.demo_on_teleport and demo_done and math.dist((x, y, z), demo_feet) > 8.0:
                        bridge.write_render(3, b"")  # kRenClearAll
                        demo_done = False  # rebuilt below, around the new position
                ring_at = getattr(bridge, "ring_at", None)
                if args.wall_ring > 0 and origin and (flags & 1) and (ring_at is None or math.dist(origin, ring_at) > 8.0):
                    if ring_at is not None:
                        bridge.write_render(3, b"")  # kRenClearAll: the player was moved (a warp); the ring follows
                    bridge.ring_at = origin
                    wall_ring(bridge, origin, args.wall_ring, args.wall_height, args.wall_drop)
                if args.demo_section and not demo_done and (flags & 1):
                    if args.demo_load > 0:
                        demo_load(bridge, (x, y, z), args.demo_load)
                    demo_section(bridge, (x, y, z), yaw)
                    demo_done = True
                    demo_feet, demo_yaw = (x, y, z), yaw
                if demo_done and t >= next_demo_mesh:
                    next_demo_mesh = t + 1.0
                    demo_meshes(bridge, demo_feet, args.demo_avatar, demo_yaw)
            else:
                flags = 0

            # 20 Hz ticks: the feet move along a circle (or stand still) on the tick rhythm.
            if origin and t >= next_tick:
                next_tick = t + 0.05 if t - next_tick > 0.1 else next_tick + 0.05
                prev = cur
                walk_o = walk
                if walk_from and t >= walk_t0:
                    path = [walk_from] + [tuple(v) for v in args.walk_via] + [tuple(args.walk_to)]
                    length = sum(math.dist(a, b) for a, b in zip(path, path[1:]))
                    raw = (t - walk_t0) * args.walk_speed  # blocks walked (standing time excluded below)
                    if args.walk_back >= 0 and raw > length + args.walk_back * args.walk_speed:
                        raw = max(0.0, 2 * length + args.walk_back * args.walk_speed - raw)
                    # --walk-pause: stand at each via point; that time doesn't count as travel
                    travel, left = 0.0, raw
                    for i, (a, b) in enumerate(zip(path, path[1:])):
                        step = min(left, math.dist(a, b))
                        travel += step
                        left -= step
                        if left <= 0 or i == len(path) - 2:
                            break
                        left = max(0.0, left - args.walk_pause * args.walk_speed)
                    travel = min(travel, length)
                    cur = path[-1]
                    for a, b in zip(path, path[1:]):
                        leg = math.dist(a, b)
                        if travel <= leg:
                            f = travel / leg if leg > 1e-6 else 1.0
                            cur = tuple(p + (q - p) * f for p, q in zip(a, b))
                            break
                        travel -= leg
                    walk += math.dist(prev, cur) * 0.6
                elif args.circle > 0:
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

            if origin and combat_t0 is None:
                combat_t0 = t + args.combat_delay
            if combat_t0 is not None and sky and t >= combat_t0:
                combat_step(bridge, args, sky, t, combat_t0, combat_state)

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
