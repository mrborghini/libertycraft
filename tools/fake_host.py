"""Stand-in for the GTA IV ASI plugin, for testing the Minecraft mod without GTA IV (Linux).

Creates the shared bridge file, streams a flat floor + a staircase of collision, scripts some input
(punch an NPC, build, shoot, drop and pick up an item), prints Minecraft's reported player state,
and saves the overlay frame to a PNG.

    python3 tools/fake_host.py [--link PATH] [--drive | --npc | --shield] [seconds] [out.png] [atlas.png]

--npc scripts Stream Q2's checks instead: a car (three vehicle pieces, kActorVehicle) and a ped stand
next to the player; it punches the car and shoots it with the bow (one kEvHitActor per hit, on a vehicle
id, the arrow flagged as a projectile), walks into the ped (its stand-in must stop the player), drives the
car through the player (it must shove the player along), then lights the floor with flint and steel
(the fire must land on the floor: kRenLights shows it). Prints a PASS/FAIL line per check.

--shield scripts GTA IV's hits (kInHurt) against a raised shield: from in front (blocked) and from behind
(hurts), with a direction only and from a ped's stand-in, and checks Minecraft's vitals in McState.

--drive scripts "GTA IV drives the player" instead (kSkyHostDrives / kSkyInVehicle): Niko mode on
foot (Minecraft follows a walking target), a vehicle (Minecraft rides its mount - boat, horse, ... per
vehicleMount in its config/libertycraft.properties - around a fast circle, with a teleport mid-way),
then the hand-back (teleport handshake, Minecraft lands on the floor). Prints how closely Minecraft
followed and a PASS/FAIL line per check; the mount's spawn/mount/removal are in Minecraft's log.

Ported from SkyCraft's tools/fake_skyrim.py (MIT). Stdlib only.
"""

import math
import mmap
import os
import struct
import sys
import time
import zlib

MAGIC = 0x5954424C  # "LBTY"
VERSION = 11
# The bridge is a tmpfs file both sides mmap (see protocol/libertycraft_protocol.h). Override with
# --link PATH or LIBERTYCRAFT_LINK, matching the client's -Dlibertycraft.link=PATH.
BRIDGE = os.environ.get("LIBERTYCRAFT_LINK", "/dev/shm/libertycraft-bridge")
OFF_SKY = 0x100
OFF_MC = 0x200
OFF_OVL = 0x300
OFF_OVL_HDR = 0x340
OFF_IN = 0x1000
OFF_ACTORS = 0x12000
OFF_EVENTS = 0x17000
OFF_ENTITIES = 0x1C000
OFF_COL = 0x20000
COL_BYTES = 32 << 20
OFF_PIX = OFF_COL + COL_BYTES
SLOT = 3840 * 2160 * 4
OFF_RENDER = OFF_PIX + SLOT * 3
RENDER_BYTES = 64 << 20
SIZE = OFF_RENDER + RENDER_BYTES
COL_DATA = COL_BYTES - 0x80

W, H = 1280, 720
FLOOR_Y = 100  # blocks
X0 = 100000    # test area origin (blocks); must be a multiple of 8


def tick():
    # GTA IV's GetTickCount64 under Wine: CLOCK_MONOTONIC_RAW in ms. Minecraft uses the same clock.
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW) // 1_000_000


def open_bridge(path, size):
    """Map the bridge file read-write, creating it or growing it to `size` if needed.

    Never shrink it: a Minecraft that already mapped it would crash (SIGBUS) on the lost pages.
    """
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        if os.fstat(fd).st_size < size:
            os.ftruncate(fd, size)  # sparse in tmpfs: pages cost memory once touched
        return mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    finally:
        os.close(fd)  # the mapping keeps the file alive


class Link:
    def __init__(self, path=BRIDGE):
        self.m = open_bridge(path, SIZE)
        # Like the real host on (re)start: zero our side's headers (this also clears mcPid, which is
        # how a running Minecraft notices the restart even if the host pid repeats) and the rings.
        self.m[0:0x100] = bytes(0x100)
        self.m[OFF_SKY:OFF_SKY + 0x40] = bytes(0x40)
        self.m[OFF_OVL:OFF_OVL + 0x100] = bytes(0x100)
        self.m[OFF_IN:OFF_IN + 0x80] = bytes(0x80)
        self.m[OFF_COL:OFF_COL + 0x80] = bytes(0x80)
        self.m[OFF_ACTORS:OFF_ACTORS + 0x40] = bytes(0x40)
        self.m[OFF_EVENTS:OFF_EVENTS + 0x80] = bytes(0x80)
        self.m[OFF_ENTITIES:OFF_ENTITIES + 0x40] = bytes(0x40)
        self.m[OFF_RENDER:OFF_RENDER + 0x80] = bytes(0x80)
        struct.pack_into("<IIII", self.m, 0, MAGIC, VERSION, os.getpid(), 0)
        self.front = 2
        self.sky_seq = 0
        self.col_head = 0
        self.actor_seq = 0
        self.events = []
        self.sections = 0
        self.atlas = None
        self.lights = []  # (x, y, z, level, when) from kRenLights
        self.hits = []    # (form, damage, push x, push z, flags) of kEvHitActor
        self.points = []  # (form, x, y, z, yaw, pitch) of kEvHitPoint

    def heartbeat(self):
        struct.pack_into("<Q", self.m, 0x10, tick())
        self.drain_events()
        self.drain_render()

    def drain_events(self):
        head, tail = struct.unpack_from("<Q", self.m, OFF_EVENTS)[0], struct.unpack_from("<Q", self.m, OFF_EVENTS + 0x40)[0]
        while tail < head:
            typ, form, a, b, c, d, flags = struct.unpack_from("<IIffffI", self.m, OFF_EVENTS + 0x80 + (tail % 512) * 32)
            print(f"  event from Minecraft: type={typ} form={form:08X} damage={a:.2f} push=({b:.2f},{c:.2f})x{d:.2f} flags={flags:#x}")
            self.events.append(typ)
            if typ == 1:
                self.hits.append((form, a, b, c, flags))
            elif typ == 6:
                self.points.append((form, a, b, c, d, struct.unpack("<f", struct.pack("<I", flags))[0]))
            tail += 1
        struct.pack_into("<Q", self.m, OFF_EVENTS + 0x40, tail)

    def drain_render(self):
        base = OFF_RENDER
        size = RENDER_BYTES - 0x80
        head, tail = struct.unpack_from("<Q", self.m, base)[0], struct.unpack_from("<Q", self.m, base + 0x40)[0]
        while tail < head:
            pos = tail % size
            typ, n = struct.unpack_from("<II", self.m, base + 0x80 + pos)
            if typ == 0:
                tail += size - pos
                continue
            if typ == 1:
                w, h = struct.unpack_from("<II", self.m, base + 0x88 + pos)
                print(f"  render: atlas {w}x{h}")
                start = base + 0x90 + pos
                self.atlas = (w, h, bytes(self.m[start:start + w * h * 4]))
            elif typ == 2:
                sx, sy, sz, verts = struct.unpack_from("<iiiI", self.m, base + 0x88 + pos)
                self.sections += 1
                if self.sections <= 5:
                    print(f"  render: section ({sx},{sy},{sz}) {verts} vertices")
                    vbase = base + 0x98 + pos
                    for k in range(min(verts, 6)):
                        x, y, z, u, v, r, g, b, a, light, flags = struct.unpack_from("<5f4BII", self.m, vbase + k * 32)
                        texel = ""
                        if self.atlas:
                            aw, ah, px = self.atlas
                            tx, ty = min(int(u * aw), aw - 1), min(int(v * ah), ah - 1)
                            o = (ty * aw + tx) * 4
                            texel = f" atlas texel ({tx},{ty}) = rgba{tuple(px[o:o + 4])}"
                        print(f"    v{k}: pos ({x:.2f},{y:.2f},{z:.2f}) uv ({u:.4f},{v:.4f}) rgba ({r},{g},{b},{a}) light {light:#x} flags {flags}{texel}")
            elif typ == 3:
                print("  render: clear all")
            elif typ == 8:
                sx, sy, sz, count = struct.unpack_from("<iiiI", self.m, base + 0x88 + pos)
                for k in range(count):
                    lx, ly, lz, level = struct.unpack_from("<4B", self.m, base + 0x98 + pos + k * 8)
                    self.lights.append((sx * 16 + lx, sy * 16 + ly, sz * 16 + lz, level, time.time()))
            tail += (8 + n + 7) & ~7
        struct.pack_into("<Q", self.m, base + 0x40, tail)

    def write_actor(self, form, x, y, z):
        struct.pack_into("<I", self.m, OFF_ACTORS, self.actor_seq * 2 + 1)
        rec = struct.pack("<IIfffffffHH24s", form, 1, x, y, z, 90.0, 0.6, 1.8, 1.0, 10, 0, b"Test Bandit")
        struct.pack_into("<I", self.m, OFF_ACTORS + 4, 1)
        self.m[OFF_ACTORS + 0x40:OFF_ACTORS + 0x40 + 64] = rec
        self.actor_seq += 1
        struct.pack_into("<I", self.m, OFF_ACTORS, self.actor_seq * 2)

    def mc_alive(self):
        (beat,) = struct.unpack_from("<Q", self.m, 0x18)
        return beat != 0 and tick() - beat < 3000

    def write_sky(self, flags, pos, yaw, pitch, teleport_seq, epoch):
        self.sky_seq += 1
        struct.pack_into("<I", self.m, OFF_SKY, self.sky_seq * 2 - 1)
        struct.pack_into("<IIIdddffIIIf", self.m, OFF_SKY + 4, flags, 0x3C, epoch, pos[0], pos[1], pos[2],
                         yaw, pitch, teleport_seq, W, H, 12.0)
        struct.pack_into("<I", self.m, OFF_SKY, self.sky_seq * 2)

    def read_mc(self):
        raw = self.m[OFF_MC:OFF_MC + 0x68]
        seq, flags, x, y, z, yaw, pitch, eye, sens, ack, gui, frame, fov, bph, bam, vh, ex, ey, ez = struct.unpack("<IIdddffffIIQfffIddd", raw)
        (va,) = struct.unpack_from("<I", self.m, OFF_MC + 0xBC)
        # Minecraft's vitals (kMcVitalsValid): health * 100 | max * 100 << 16; armour | absorption << 8 | valid.
        vitals = dict(health=(vh & 0xFFFF) / 100, max=(vh >> 16) / 100, armour=va & 0xFF, absorption=(va >> 8) & 0xFF) if va >> 31 else None
        return dict(flags=flags, pos=(x, y, z), yaw=yaw, pitch=pitch, eye=eye, ack=ack, frame=frame, vitals=vitals)

    def push_input(self, typ, code, a=0, b=0, c=0):
        a, b, c = (v - (1 << 32) if v >= 1 << 31 else v for v in (a, b, c))
        head, = struct.unpack_from("<Q", self.m, OFF_IN)
        struct.pack_into("<HHiii", self.m, OFF_IN + 0x80 + (head % 4096) * 16, typ, code, a, b, c)
        struct.pack_into("<Q", self.m, OFF_IN, head + 1)

    def write_col(self, typ, payload):
        msg = 8 + len(payload)
        msg = (msg + 7) & ~7
        pos = self.col_head % COL_DATA
        if pos + msg > COL_DATA:
            struct.pack_into("<II", self.m, OFF_COL + 0x80 + pos, 0, 0)
            self.col_head += COL_DATA - pos
            pos = 0
        base = OFF_COL + 0x80 + pos
        struct.pack_into("<II", self.m, base, typ, len(payload))
        self.m[base + 8:base + 8 + len(payload)] = payload
        self.col_head += msg
        struct.pack_into("<Q", self.m, OFF_COL, self.col_head)

    def acquire_overlay(self):
        (state,) = struct.unpack_from("<I", self.m, OFF_OVL)
        if not state & 4:
            return None
        struct.pack_into("<I", self.m, OFF_OVL, self.front)
        self.front = state & 3
        w, h, flags = struct.unpack_from("<III", self.m, OFF_OVL_HDR + self.front * 0x40)
        start = OFF_PIX + self.front * SLOT
        return w, h, flags, bytes(self.m[start:start + w * h * 4])


def region_payload(rx, ry, rz, epoch, block_fn):
    blocks = []
    for by in range(8):
        for bz in range(8):
            for bx in range(8):
                x, y, z = rx * 8 + bx, ry * 8 + by, rz * 8 + bz
                bits = block_fn(x, y, z)
                if bits and any(bits):
                    blocks.append(struct.pack("<iiiI8Q", x, y, z, 0, *bits))
    head = struct.pack("<iiiiiiII", rx * 8, ry * 8, rz * 8, rx * 8 + 7, ry * 8 + 7, rz * 8 + 7, epoch, len(blocks))
    return head + b"".join(blocks)


def floor_tris_payload(rx, ry, rz, epoch):
    """Exact triangles for the flat floor top (y = FLOOR_Y) inside one region (the player collides with these)."""
    y = float(FLOOR_Y)
    tris = []
    if ry * 8 <= FLOOR_Y - 1 <= ry * 8 + 7:
        x0, z0, x1, z1 = rx * 8.0, rz * 8.0, rx * 8.0 + 8.0, rz * 8.0 + 8.0
        tris.append(struct.pack("<9fI", x0, y, z0, x1, y, z0, x1, y, z1, 0))
        tris.append(struct.pack("<9fI", x0, y, z0, x1, y, z1, x0, y, z1, 0))
    head = struct.pack("<iiiiiiII", rx * 8, ry * 8, rz * 8, rx * 8 + 7, ry * 8 + 7, rz * 8 + 7, epoch, len(tris))
    return head + b"".join(tris)


FULL = [0xFFFFFFFFFFFFFFFF] * 8


def world(x, y, z):
    """A flat floor at FLOOR_Y-1, plus a staircase of half-block steps towards +X, and a 1/8 slope towards -X."""
    x -= X0
    if y == FLOOR_Y - 1:
        return FULL
    if 3 <= x < 11 and -2 <= z <= 2 and y == FLOOR_Y + (x - 3) // 2:
        return FULL if (x - 3) % 2 == 1 else [0xFFFFFFFFFFFFFFFF] * 4 + [0] * 4
    if -12 <= x <= -4 and -2 <= z <= 2:
        # gentle 1/8-step ramp going up towards -X: height rises one sub-voxel per sub-voxel... per block: 1 voxel/8 voxels
        rise = (-4 - x)  # sub-voxels of height at this block
        base = FLOOR_Y + rise // 8
        if y == base:
            layers = rise % 8 + 1
            return [0xFFFFFFFFFFFFFFFF] * layers + [0] * (8 - layers)
    return None


def save_png(path, w, h, rgba, bottom_up):
    rows = [rgba[r * w * 4:(r + 1) * w * 4] for r in range(h)]
    if bottom_up:
        rows.reverse()
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def check_arrow_texture(link):
    # An arrow's two atlas rects (side view, back plate) should cover the arrow entity texture.
    count, _ = struct.unpack_from("<II", link.m, OFF_ENTITIES + 4)
    for i in range(min(count, 16)):
        base = OFF_ENTITIES + 0x40 + i * 96
        (kind,) = struct.unpack_from("<I", link.m, base)
        if kind != 1 or not link.atlas:
            continue
        aw, ah, px = link.atlas
        for name, k in (("side", 0), ("back", 1)):
            u0, v0, u1, v1 = struct.unpack_from("<4f", link.m, base + 44 + k * 16)
            x0, y0, x1, y1 = round(u0 * aw), round(v0 * ah), round(u1 * aw), round(v1 * ah)
            rows = []
            for y in range(y0, y1):
                rows.append("".join("#" if px[(y * aw + x) * 4 + 3] > 127 else "." for x in range(x0, x1)))
            print(f"  arrow {name} rect ({x0},{y0})-({x1},{y1}):")
            for r in rows:
                print("    " + r)
        return


def read_world_entities(link):
    count, has_sel = struct.unpack_from("<II", link.m, OFF_ENTITIES + 4)
    kinds = []
    for i in range(min(count, 16)):
        kind, eid, x, y, z = struct.unpack_from("<IIfff", link.m, OFF_ENTITIES + 0x40 + i * 96)
        kinds.append((kind, round(x, 2), round(y, 2), round(z, 2)))
    sel = struct.unpack_from("<6f", link.m, OFF_ENTITIES + 0x0C) if has_sel else None
    return kinds, sel


SKY_IN_GAME = 1
SKY_HOST_DRIVES = 1 << 3
SKY_IN_VEHICLE = 1 << 4


class DriveScenario:
    """--drive: GTA IV drives the player on foot, then in a vehicle, then hands it back."""

    WALK = (1.0, 6.0)      # host drives on foot: target walks +X at 3 m/s
    VEHICLE = (6.0, 18.0)  # in a vehicle: a 12 m circle at 15 m/s, teleport bumped at 12 s
    RADIUS, SPEED = 12.0, 15.0
    SEAT = 0.4             # the rider's feet above the floor
    BACK = 18.0            # hand-back: flags clear, teleport to the floor

    def __init__(self, spawn, tseq):
        self.spawn = spawn
        self.tseq = tseq
        self.devs = {"walk": [], "vehicle": []}
        self.acks_while_driving = []
        self.bumped = False
        self.back_seq = None
        self.results = []

    def target(self, t):
        """(flags, pos, yaw) GTA IV reports at script time t."""
        sx, sy, sz = self.spawn
        if t < self.WALK[0]:
            return SKY_IN_GAME, self.spawn, -90.0
        if t < self.WALK[1]:
            d = 3.0 * (t - self.WALK[0])
            return SKY_IN_GAME | SKY_HOST_DRIVES, (sx + d, sy, sz), -90.0
        if t < self.VEHICLE[1]:
            u = t - self.VEHICLE[0]
            ang = u * self.SPEED / self.RADIUS
            cx, cz = sx + 15.0, sz  # circle centre
            x, z = cx - self.RADIUS * math.cos(ang), cz - self.RADIUS * math.sin(ang)
            # Moving direction (dx, dz) = (sin, -cos) * R * w: Minecraft yaw faces (-sin(yaw), cos(yaw)).
            yaw = math.degrees(math.atan2(-math.sin(ang), -math.cos(ang)))
            return SKY_IN_GAME | SKY_HOST_DRIVES | SKY_IN_VEHICLE, (x, sy + self.SEAT, z), yaw
        return SKY_IN_GAME, (sx + 2.0, sy, sz + 2.0), 0.0

    def step(self, link, t, mc):
        flags, pos, yaw = self.target(t)
        if self.VEHICLE[0] + 6.0 <= t and not self.bumped:
            self.bumped = True
            self.tseq += 1  # a teleport while GTA IV drives (world change): acknowledged, no hold
            print(f"  t={t:.1f}: teleport #{self.tseq} while driving")
        if t >= self.BACK and self.back_seq is None:
            self.tseq += 1
            self.back_seq = self.tseq
            print(f"  t={t:.1f}: GTA IV lets go; teleport #{self.tseq} to the floor at {pos}")
        dev = math.dist(mc["pos"], pos)
        if self.WALK[0] + 0.5 <= t < self.WALK[1]:
            self.devs["walk"].append(dev)
        if self.VEHICLE[0] + 1.0 <= t < self.VEHICLE[1]:
            self.devs["vehicle"].append(dev)
        if self.bumped and t < self.VEHICLE[1]:
            self.acks_while_driving.append(mc["ack"] == self.tseq)
        return flags, pos, yaw

    def report(self, mc):
        def check(name, ok, detail):
            self.results.append(ok)
            print(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")

        for name, limit in (("walk", 0.35), ("vehicle", 1.2)):
            d = sorted(self.devs[name])
            if not d:
                check(f"follow ({name})", False, "no samples")
                continue
            p95 = d[int(len(d) * 0.95)]
            check(f"follow ({name})", p95 < limit, f"{len(d)} frames, median {d[len(d) // 2]:.3f} m, p95 {p95:.3f} m (limit {limit})")
        acked = sum(self.acks_while_driving)
        check("teleport while driving acknowledged", acked > 0, f"{acked}/{len(self.acks_while_driving)} frames acked")
        _, pos, _ = self.target(self.BACK + 1.0)
        landed = mc["ack"] == self.back_seq and math.dist(mc["pos"], pos) < 0.3
        check("hand-back", landed, f"ack {mc['ack']} (want {self.back_seq}), MC at {tuple(round(v, 3) for v in mc['pos'])}, want {pos}")
        on_floor = abs(mc["pos"][1] - FLOOR_Y) < 0.05
        check("standing on the floor after", on_floor, f"y {mc['pos'][1]:.3f} (floor {FLOOR_Y})")
        print(f"drive summary: {'PASS' if all(self.results) else 'FAIL'} ({sum(self.results)}/{len(self.results)})")


ACTOR_VEHICLE = 1 << 4
VEHICLE_TAG = 0x56000000
HIT_PROJECTILE = 1 << 1


class NpcScenario:
    """Stream Q2: GTA IV's vehicles and peds as solid, hittable stand-ins; fire on GTA IV ground."""
    CAR = 0x1234  # vehicle handle
    PED = 0x4C000777

    def __init__(self, spawn):
        self.spawn = spawn
        self.car = (spawn[0] + 3.0, spawn[2], "z")  # centre x, z and the axis it lies along
        self.car_speed = 0.0
        self.results = []
        self.marks = {}
        self.yaw, self.pitch = -90.0, 20.0  # at the car's side, below its roof

    def car_records(self):
        cx, cz, axis = self.car
        recs = []
        for piece, off in enumerate((1.6, 0.0, -1.6)):
            x, z = (cx, cz + off) if axis == "z" else (cx + off, cz)
            recs.append(struct.pack("<IIfffffffHH24s", VEHICLE_TAG | (self.CAR << 4) | piece, ACTOR_VEHICLE, x, FLOOR_Y, z, 0.0, 1.9, 1.5, 1.0, 0, 0,
                                    b"ADMIRAL"))
        return recs

    def write_actors(self, link):
        recs = self.car_records()
        recs.append(struct.pack("<IIfffffffHH24s", self.PED, 0, self.spawn[0], FLOOR_Y, self.spawn[2] + 4.5, 0.0, 0.6, 1.8, 1.0, 0, 0, b"Civilian"))
        struct.pack_into("<I", link.m, OFF_ACTORS, link.actor_seq * 2 + 1)
        struct.pack_into("<I", link.m, OFF_ACTORS + 4, len(recs))
        link.m[OFF_ACTORS + 0x40:OFF_ACTORS + 0x40 + 64 * len(recs)] = b"".join(recs)
        link.actor_seq += 1
        struct.pack_into("<I", link.m, OFF_ACTORS, link.actor_seq * 2)

    def once(self, t, at, name):
        if t >= at and name not in self.marks:
            self.marks[name] = t
            return True
        return False

    def check(self, name, ok, detail):
        self.results.append(ok)
        print(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")

    def command(self, link, t, at, text):
        """Types a chat command: T, the text, Enter."""
        if self.once(t, at, f"T{at}"):
            link.push_input(1, 23, 1)
        if self.once(t, at + 0.1, f"Tup{at}"):
            link.push_input(1, 23, 0)
        if self.once(t, at + 0.5, f"text{at}"):
            for ch in text:
                link.push_input(5, 0, ord(ch))
        if self.once(t, at + 0.9, f"enter{at}"):
            link.push_input(1, 40, 1)
        if self.once(t, at + 1.0, f"enterup{at}"):
            link.push_input(1, 40, 0)

    def key(self, link, t, at, code):
        if self.once(t, at, f"k{code}@{at}"):
            link.push_input(1, code, 1)
        if self.once(t, at + 0.1, f"k{code}up@{at}"):
            link.push_input(1, code, 0)

    def click(self, link, t, at, button, hold=0.1):
        if self.once(t, at, f"b{button}@{at}"):
            link.push_input(2, button, 1)
        if self.once(t, at + hold, f"b{button}up@{at}"):
            link.push_input(2, button, 0)

    def step(self, link, t, mc, dt):
        """True once every check ran."""
        sx, sy, sz = self.spawn
        # Kit: a bow in slot 3, arrows, flint and steel in slot 9 (the tests' own copy of the dev world).
        self.command(link, t, 0.3, "/item replace entity @s hotbar.2 with minecraft:bow")
        self.command(link, t, 1.5, "/give @s minecraft:arrow 16")
        self.command(link, t, 2.7, "/item replace entity @s hotbar.8 with minecraft:flint_and_steel")
        self.key(link, t, 3.9, 30)  # '1': whatever is in slot 1 (a fist will do)
        # Melee: two clicks at the car's middle piece straight ahead (the first may only grab the mouse).
        self.click(link, t, 4.5, 1)
        self.click(link, t, 5.2, 1)
        if self.once(t, 6.1, "melee"):
            hits = [h for h in link.hits if (h[0] & 0xFF000000) == VEHICLE_TAG]
            ok = len(hits) >= 1 and all((h[0] >> 4) & 0xFFFFF == self.CAR and h[2] > 0.9 and not h[4] & HIT_PROJECTILE for h in hits)
            self.check("melee on a vehicle", ok, f"{len(hits)} vehicle hit event(s): {[(f'{h[0]:08X}', round(h[1], 2), round(h[2], 2), round(h[3], 2), h[4]) for h in hits]}"
                       " (want one per hit, on the car, pushed +x away from the player)")
            # Where they landed: on the car's near face (x = car - 0.95 - slack), along the look (yaw -90, pitch 20).
            pts = [p for p in link.points if (p[0] & 0xFF000000) == VEHICLE_TAG]
            face_x = self.spawn[0] + 3.0 - 1.9 / 2
            ok = len(pts) == len(hits) and all(abs(p[1] - face_x) < 0.15 and abs(p[4] + 90.0) < 1.0 and abs(p[5] - 20.0) < 1.0 for p in pts)
            self.check("melee hit points", ok, f"{[tuple(round(v, 2) for v in p[1:]) for p in pts]} (want one per hit, x near {face_x:.2f}, yaw -90, pitch 20)")
            self.marks["hits_before_bow"] = len(link.hits)
            self.marks["points_before_bow"] = len(link.points)
        # Bow (slot 3) into the car.
        self.key(link, t, 6.2, 32)
        self.click(link, t, 6.7, 3, hold=1.3)
        if self.once(t, 9.0, "arrow"):
            hits = [h for h in link.hits[self.marks["hits_before_bow"]:] if (h[0] & 0xFF000000) == VEHICLE_TAG]
            ok = len(hits) >= 1 and all(h[4] & HIT_PROJECTILE for h in hits)
            self.check("arrow into a vehicle", ok, f"{len(hits)} vehicle hit event(s), flags {[h[4] for h in hits]} (want the projectile flag)")
            pts = [p for p in link.points[self.marks["points_before_bow"]:] if (p[0] & 0xFF000000) == VEHICLE_TAG]
            ok = len(pts) == len(hits) and all(abs(p[4] + 90.0) < 10.0 for p in pts)
            self.check("arrow hit point", ok, f"{[tuple(round(v, 2) for v in p[1:]) for p in pts]} (want one per arrow, flying east: yaw about -90)")
        # Walk south into the ped standing 4.5 blocks away.
        if t >= 9.1:
            self.yaw, self.pitch = 0.0, 0.0
        if self.once(t, 9.3, "w"):
            link.push_input(1, 26, 1)
        if self.once(t, 11.5, "wup"):
            link.push_input(1, 26, 0)
        if self.once(t, 12.0, "walk"):
            z = mc["pos"][2]
            want = sz + 4.5 - 0.6
            self.check("a ped's stand-in is solid", sz + 1.0 < z <= want + 0.05, f"walked from z {sz:.2f} to {z:.3f} (the ped's box starts at {want + 0.3:.2f};"
                       f" the player stops at {want:.2f})")
            self.marks["pos_before_car"] = mc["pos"]
        # Drive the car (now lying along x) through the player toward -x at 8 blocks/s.
        if self.once(t, 12.2, "car_place"):
            p = mc["pos"]
            self.car = (p[0] + 6.0, p[2], "x")
            self.car_speed = 8.0
        if 12.2 <= t < 13.7 and self.car_speed:
            cx, cz, axis = self.car
            self.car = (max(cx - self.car_speed * dt, sx - 6.0), cz, axis)
        if self.once(t, 14.2, "shove"):
            before, x = self.marks["pos_before_car"], mc["pos"][0]
            cx = self.car[0]
            ok = x < before[0] - 2.0 and x <= cx - 2.4 - 0.3 + 0.05
            self.check("a moving car shoves the player", ok, f"player x {before[0]:.2f} -> {x:.2f}, the car's front end stopped at {cx - 2.4:.2f}"
                       " (want the player pushed ahead of it, out of its box)")
            self.car = (sx + 30.0, sz, "z")  # out of the way
        # Flint and steel (slot 9) on the floor in front.
        self.key(link, t, 14.4, 38)
        if t >= 14.6:
            self.pitch = 60.0
        if self.once(t, 15.1, "lights_mark"):
            self.marks["lights_before"] = len(link.lights)
        self.click(link, t, 15.2, 3)
        if self.once(t, 17.0, "fire"):
            p = mc["pos"]
            fires = [l for l in link.lights[self.marks["lights_before"]:] if l[3] >= 15 and abs(l[0] + 0.5 - p[0]) < 3 and abs(l[2] + 0.5 - p[2]) < 3]
            ok = any(l[1] == FLOOR_Y for l in fires)
            self.check("flint and steel lights GTA IV ground", ok, f"light-15 blocks near the player since the click: {[l[:4] for l in fires]}"
                       f" (want one at y {FLOOR_Y}, on the floor)")
        if t >= 17.5:
            print(f"npc summary: {'PASS' if all(self.results) else 'FAIL'} ({sum(self.results)}/{len(self.results)})")
            return True
        return False


IN_HURT = 7
HURT_MELEE, HURT_PROJECTILE, HURT_OTHER = 0, 1, 3
HURT_HAS_DIRECTION, HURT_DIRECTION_SHIFT = 1 << 2, 16
MC_BLOCKING = 1 << 8  # McState flags: the shield is up (kMcBlocking)


class ShieldScenario(NpcScenario):
    """Stream Q2: GTA IV's hits (kInHurt) against a raised shield. The player faces +X (yaw -90) with a
    shield in the off hand, held up; hits come from in front and from behind, with a direction only
    (kHurtHasDirection) and from a ped's stand-in. In front must be blocked, behind must hurt;
    Minecraft's vitals (McState padding) must show up, and McState's kMcBlocking only while the
    shield is up."""
    PED = 0x4C000999

    def __init__(self, spawn):
        super().__init__(spawn)
        self.pitch = 0.0
        self.ped_x = None  # the attacking ped's x (None: no ped)

    def write_actors(self, link):
        recs = []
        if self.ped_x is not None:
            recs.append(struct.pack("<IIfffffffHH24s", self.PED, 1, self.ped_x, FLOOR_Y, self.spawn[2], 90.0, 0.6, 1.8, 1.0, 0, 0, b"Gangster"))
        struct.pack_into("<I", link.m, OFF_ACTORS, link.actor_seq * 2 + 1)
        struct.pack_into("<I", link.m, OFF_ACTORS + 4, len(recs))
        if recs:
            link.m[OFF_ACTORS + 0x40:OFF_ACTORS + 0x40 + 64 * len(recs)] = b"".join(recs)
        link.actor_seq += 1
        struct.pack_into("<I", link.m, OFF_ACTORS, link.actor_seq * 2)

    def hurt(self, link, t, at, name, kind, yaw=None, attacker=0):
        if self.once(t, at, name):
            flags = (HURT_HAS_DIRECTION | ((int(yaw) % 360) << HURT_DIRECTION_SHIFT)) if yaw is not None else 0
            self.marks[name + "_h"] = None
            link.push_input(IN_HURT, kind, 2000, attacker, flags)  # 20 host damage = 4 Minecraft damage

    def step(self, link, t, mc, dt):
        v = mc.get("vitals")
        health = v["health"] if v else None
        self.command(link, t, 0.3, "/item replace entity @s weapon.offhand with minecraft:shield")
        if self.once(t, 3.0, "vitals"):
            ok = v is not None and v["max"] == 20.0 and 0 < v["health"] <= 20.0
            self.check("Minecraft's vitals in McState", ok, f"{v} (want health and max 20)")
        if self.once(t, 3.0, "down_flag"):
            self.check("McState: no kMcBlocking before the shield goes up", not mc["flags"] & MC_BLOCKING, f"flags {mc['flags']:#x}")
        if self.once(t, 3.2, "raise"):
            link.push_input(2, 3, 1)  # hold the right button: the off-hand shield goes up
        if self.once(t, 4.2, "up_flag"):
            self.check("McState: kMcBlocking while the shield is up", bool(mc["flags"] & MC_BLOCKING), f"flags {mc['flags']:#x}")
        # (The marks below remember the health just before each hit.)
        for name, at, kind, yaw, attacker, ped_x, want_blocked in (
            ("front", 4.5, HURT_PROJECTILE, -90.0, 0, None, True),
            ("behind", 6.0, HURT_PROJECTILE, 90.0, 0, None, False),
            ("ped_front", 7.5, HURT_MELEE, None, self.PED, self.spawn[0] + 1.5, True),
            ("ped_behind", 9.0, HURT_MELEE, None, self.PED, self.spawn[0] - 1.5, False),
            ("no_direction", 10.5, HURT_OTHER, None, 0, None, False),
        ):
            if at - 0.6 <= t < at:
                self.ped_x = ped_x
            if self.once(t, at - 0.1, name + "_before"):
                self.marks[name + "_hp"] = health
            self.hurt(link, t, at, name, kind, yaw, attacker)
            if self.once(t, at + 0.8, name + "_after"):
                before, after = self.marks.get(name + "_hp"), health
                blocked = before is not None and after is not None and after >= before - 0.01
                hurt_ = before is not None and after is not None and after <= before - 1.0
                self.check(f"hit {name.replace('_', ' ')}: {'blocked' if want_blocked else 'hurts'}", blocked if want_blocked else hurt_,
                           f"health {before} -> {after}")
        if self.once(t, 11.5, "lower"):
            link.push_input(2, 3, 0)
        if self.once(t, 12.3, "lowered_flag"):
            self.check("McState: no kMcBlocking once the shield is down", not mc["flags"] & MC_BLOCKING, f"flags {mc['flags']:#x}")
        if t >= 12.5:
            print(f"shield summary: {'PASS' if all(self.results) else 'FAIL'} ({sum(self.results)}/{len(self.results)})")
            return True
        return False


def main():
    args = sys.argv[1:]
    path = BRIDGE
    if "--link" in args:
        i = args.index("--link")
        path = args[i + 1]
        del args[i:i + 2]
    drive = None
    if "--drive" in args:
        args.remove("--drive")
        drive = True
    npc = None
    if "--npc" in args:
        args.remove("--npc")
        npc = True
    shield = "--shield" in args
    if shield:
        args.remove("--shield")
        npc = True
    seconds = float(args[0]) if len(args) > 0 else 90
    out = args[1] if len(args) > 1 else "overlay.png"
    atlas_out = args[2] if len(args) > 2 else None
    link = Link(path)
    print(f"fake host up on {path}; waiting for Minecraft...")
    epoch = 1
    sent = False
    start = time.time()
    # Far from anywhere Liberty City maps to, so test blocks never show up in the game.
    spawn = (X0 + 0.5, FLOOR_Y, 0.5)
    yaw = -90.0  # facing +X
    pitch = 10.0
    script_t0 = None
    last_print = 0
    saved = False
    checked = False
    tseq = int(time.time()) % 100000 + 2  # new teleport every run
    if drive:
        drive = DriveScenario(spawn, tseq)
    if npc:
        npc = ShieldScenario(spawn) if shield else NpcScenario(spawn)
    mc = link.read_mc()
    last_t = None
    while time.time() - start < seconds:
        link.heartbeat()
        sky_flags, sky_pos, sky_yaw, sky_pitch = 1, spawn, yaw, pitch
        if drive and script_t0 is not None:
            t = time.time() - script_t0
            sky_flags, sky_pos, sky_yaw = drive.step(link, t, mc)
            sky_pitch = 0.0
            tseq = drive.tseq
            if t > drive.BACK + 4.0:
                drive.report(mc)
                break
        if npc and script_t0 is not None:
            t = time.time() - script_t0
            npc.write_actors(link)
            if npc.step(link, t, mc, t - last_t if last_t is not None else 0.0):
                break
            last_t = t
            sky_yaw, sky_pitch = npc.yaw, npc.pitch
        link.write_sky(sky_flags, sky_pos, sky_yaw, sky_pitch, tseq, epoch)
        if link.mc_alive() and not sent:
            link.write_col(1, struct.pack("<I", epoch))
            for rx in range(X0 // 8 - 3, X0 // 8 + 3):
                for rz in range(-3, 3):
                    for ry in range(11, 15):
                        link.write_col(3, floor_tris_payload(rx, ry, rz, epoch))
                        link.write_col(2, region_payload(rx, ry, rz, epoch, world))
            sent = True
            print("collision sent")
        mc = link.read_mc()
        in_world = mc["flags"] & 1 and mc["ack"] == tseq and sent and abs(mc["pos"][1] - FLOOR_Y) < 0.05 and abs(mc["pos"][0] - spawn[0]) < 1
        if in_world and script_t0 is None:
            script_t0 = time.time()
            print("MC in world at", mc["pos"])
        if script_t0 is not None and not drive and not npc:
            t = time.time() - script_t0
            # A fake NPC two blocks ahead: punch it, then have it hit back.
            link.write_actor(0xFF00ABCD, X0 + 2.5, FLOOR_Y, 0.5)
            for click in (0.8, 1.5):  # the first click only grabs the mouse, like focusing a window
                if click <= t < click + 0.05:
                    link.push_input(2, 1, 1)  # left mouse down
                if click + 0.1 <= t < click + 0.15:
                    link.push_input(2, 1, 0)
            if 2.5 <= t < 2.55:
                link.push_input(7, 0, 2000, 0xFF00ABCD, 0)  # 20 host damage, melee, from the NPC
            # Build: cobblestone (hotbar 8) onto the host's floor, looking down and to the side.
            if 3.0 <= t < 3.05:
                yaw, pitch = 0.0, 55.0  # facing +Z, away from the NPC
                link.push_input(1, 37, 1)  # '8'
            if 3.1 <= t < 3.15:
                link.push_input(1, 37, 0)
            if 3.6 <= t < 3.65:
                link.push_input(2, 3, 1)  # right mouse: place
            if 3.7 <= t < 3.75:
                link.push_input(2, 3, 0)
            # Bow (hotbar 3): draw and loose an arrow into the floor further out.
            if 4.2 <= t < 4.25:
                yaw, pitch = 180.0, 35.0  # facing -Z: open host floor, no Minecraft blocks
                link.push_input(1, 32, 1)  # '3'
            if 4.3 <= t < 4.35:
                link.push_input(1, 32, 0)
            if 4.6 <= t < 4.65:
                link.push_input(2, 3, 1)
            if 5.8 <= t < 5.85:
                link.push_input(2, 3, 0)
            if 8.0 <= t and checked is False:
                checked = 1
                kinds, sel = read_world_entities(link)
                print(f"world entities at t=8: {kinds} (kind 1 = arrow), block outline {sel}")
            if 12.0 <= t and checked == 1:
                checked = 2
                kinds, sel = read_world_entities(link)
                print(f"world entities at t=12: {kinds} (a stuck arrow hasn't moved)")
                check_arrow_texture(link)
            # Pickup: drop one cobblestone (Q) ahead, walk over it, see whether it's gone.
            if 12.5 <= t < 12.55:
                yaw, pitch = 90.0, 10.0  # facing -X
                link.push_input(1, 37, 1)  # '8'
            if 12.6 <= t < 12.65:
                link.push_input(1, 37, 0)
            if 13.0 <= t < 13.05:
                link.push_input(1, 20, 1)  # Q (drop)
            if 13.1 <= t < 13.15:
                link.push_input(1, 20, 0)
            if 15.0 <= t and checked == 2:
                checked = 3
                kinds, sel = read_world_entities(link)
                print(f"world entities after dropping: {kinds} (kind 2 = dropped item)")
                link.push_input(1, 26, 1)  # W: walk over it
            if 16.5 <= t and checked == 3:
                checked = 4
                link.push_input(1, 26, 0)
            if 18.0 <= t and checked == 4:
                checked = 5
                kinds, sel = read_world_entities(link)
                print(f"world entities after walking over it: {kinds}")
            if 10 <= t and not saved:
                frame = link.acquire_overlay()
                if frame:
                    w, h, flags, px = frame
                    save_png(out, w, h, px, flags & 1)
                    nonzero = sum(1 for i in range(3, len(px), 4 * 97) if px[i])
                    print(f"saved overlay {w}x{h} -> {out} (sampled non-transparent pixels: {nonzero})")
                    saved = True
        if time.time() - last_print > 1.0:
            last_print = time.time()
            print(f"t={time.time()-start:5.1f} mc flags={mc['flags']:#x} ack={mc['ack']} pos=({mc['pos'][0]:.3f}, {mc['pos'][1]:.3f}, {mc['pos'][2]:.3f}) yaw={mc['yaw']:.1f} frame={mc['frame']}")
        time.sleep(1 / 60)
    print(f"summary: {link.sections} section meshes, events {link.events}")
    if link.atlas and atlas_out:
        w, h, px = link.atlas
        save_png(atlas_out, w, h, px, False)
        print(f"atlas saved to {atlas_out}")


if __name__ == "__main__":
    main()
