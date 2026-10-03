#!/usr/bin/env python3
"""Decode the collision ring in the live LibertyCraft bridge (read-only) and inspect it.

    python3 tools/dump_collision.py                       # summary: regions, triangles, voxels
    python3 tools/dump_collision.py --at X Y Z            # what's at an MC position: column profile, triangles
    python3 tools/dump_collision.py --at X Y Z --slice    # + ASCII slices around it (top-down and vertical)
    python3 tools/dump_collision.py --at X Y Z --map      # + a 1/4-block map of floors and walls around it

Coordinates are Minecraft's (GTA (x, y, z) = MC (x, -z, y)). The ring is read from its oldest
surviving byte; per region the newest kColTris / kColRegion since the last kColClear wins.
The bridge is LIBERTYCRAFT_LINK or /dev/shm/libertycraft-bridge. Ported from SkyCraft (MIT).
"""
import argparse
import math
import mmap
import os
import struct
import sys

PATH = os.environ.get("LIBERTYCRAFT_LINK", "/dev/shm/libertycraft-bridge")
OFF_COL = 0x20000
COL_BYTES = 32 << 20
DATA = COL_BYTES - 0x80
TRI_STAIR, TRI_DIG, TRI_GHOST, TRI_TERRAIN = 1, 2, 4, 8


def load():
    try:
        with open(PATH, "rb") as f:
            m = mmap.mmap(f.fileno(), 0x20000 + COL_BYTES, access=mmap.ACCESS_READ)
    except (OSError, ValueError) as e:
        sys.exit(f"can't map {PATH} ({e}); is the host or Minecraft running?")
    head, = struct.unpack_from("<Q", m, OFF_COL)
    tail, = struct.unpack_from("<Q", m, OFF_COL + 0x40)
    pos = 0 if head <= DATA else head - DATA
    # Starting mid-ring after a wrap would land mid-message; skip to the first plausible header.
    tris, vox, clears, msgs = {}, {}, 0, 0
    epoch = None
    while pos < head:
        p = pos % DATA
        if DATA - p < 8:
            pos += DATA - p
            continue
        typ, n = struct.unpack_from("<II", m, OFF_COL + 0x80 + p)
        if typ == 0:
            pos += DATA - p
            continue
        if typ > 3 or n > DATA:
            pos += 8
            continue
        body = OFF_COL + 0x80 + p + 8
        msgs += 1
        if typ == 1:
            epoch = struct.unpack_from("<I", m, body)[0]
            tris.clear()
            vox.clear()
            clears += 1
        else:
            mnx, mny, mnz, mxx, mxy, mxz, ep, cnt = struct.unpack_from("<iiiiiiII", m, body)
            key = (mnx // 8, mny // 8, mnz // 8)
            if epoch is None:
                epoch = ep
            if ep == epoch:
                if typ == 3:
                    lst = []
                    for i in range(cnt):
                        v = struct.unpack_from("<9fI", m, body + 32 + i * 40)
                        lst.append((v[:9], v[9]))
                    tris[key] = lst
                else:
                    blocks = {}
                    for i in range(cnt):
                        b = body + 32 + i * 80
                        x, y, z = struct.unpack_from("<iii", m, b)
                        blocks[(x, y, z)] = struct.unpack_from("<8Q", m, b + 16)
                    vox[key] = blocks
        pos += (8 + n + 7) & ~7
    return dict(head=head, tail=tail, tris=tris, vox=vox, clears=clears, msgs=msgs, epoch=epoch)


def solid(vox, x, y, z):
    """Is the 1/8-block voxel containing MC point (x, y, z) solid? None if its region wasn't sent."""
    bx, by, bz = math.floor(x), math.floor(y), math.floor(z)
    region = vox.get((bx // 8, by // 8, bz // 8))
    if region is None:
        return None
    bits = region.get((bx, by, bz))
    if bits is None:
        return False
    sx, sy, sz = int((x - bx) * 8), int((y - by) * 8), int((z - bz) * 8)
    return bool((bits[sy] >> (sz * 8 + sx)) & 1)


def normal(v):
    ux, uy, uz = v[3] - v[0], v[4] - v[1], v[5] - v[2]
    wx, wy, wz = v[6] - v[0], v[7] - v[1], v[8] - v[2]
    n = (uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx)
    l = math.sqrt(sum(c * c for c in n)) or 1.0
    return tuple(c / l for c in n)


def kind(v):
    ny = normal(v)[1]
    return "floor" if ny >= 0.7 else "ceil" if ny <= -0.7 else "slope" if ny > 0.05 else "under" if ny < -0.05 else "wall"


def height_at(v, x, z):
    """Height of the triangle's plane at (x, z) if (x, z) is inside its XZ footprint, else None."""
    (ax, ay, az, bx, by, bz, cx, cy, cz) = v
    den = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
    if abs(den) < 1e-9:
        return None
    l0 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / den
    l1 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / den
    l2 = 1 - l0 - l1
    if min(l0, l1, l2) < -1e-4:
        return None
    return l0 * ay + l1 * by + l2 * cy


def all_tris(d):
    seen = set()
    for lst in d["tris"].values():
        for v, f in lst:
            if (v, f) not in seen:
                seen.add((v, f))
                yield v, f


def profile(d, x, y, z, span):
    print(f"\ncolumn at MC x {x:.2f} z {z:.2f} (GTA {x:.2f} {-z:.2f}), y {y - span:.1f}..{y + span:.1f}:")
    surfaces = []
    for v, f in all_tris(d):
        h = height_at(v, x, z)
        if h is not None and y - span <= h <= y + span:
            surfaces.append((h, kind(v), f))
    for h, k, f in sorted(set(surfaces), reverse=True):
        flags = "".join(n for b, n in ((TRI_TERRAIN, "T"), (TRI_DIG, "D"), (TRI_STAIR, "S"), (TRI_GHOST, "G")) if f & b)
        print(f"  triangle {k:5s} at y {h:8.3f}  flags {flags or '-'} material {(f >> 8) & 0xFF}")
    runs, cur = [], None
    yy = math.floor((y - span) * 8) / 8
    while yy < y + span:
        s = solid(d["vox"], x, yy + 1 / 16, z)
        state = "?" if s is None else "#" if s else "."
        if cur and cur[0] == state:
            cur[2] = yy + 0.125
        else:
            cur = [state, yy, yy + 0.125]
            runs.append(cur)
        yy += 0.125
    print("  voxels (bottom-up; # solid, . free, ? region not sent):")
    for state, lo, hi in runs:
        print(f"    {state} {lo:8.3f} .. {hi:8.3f}")


def walls_near(d, x, y, z, r):
    out = []
    for v, f in all_tris(d):
        if kind(v) != "wall":
            continue
        xs, ys, zs = v[0::3], v[1::3], v[2::3]
        if max(ys) < y - 0.5 or min(ys) > y + 2.0:
            continue
        cx, cz = sum(xs) / 3, sum(zs) / 3
        if abs(cx - x) <= r and abs(cz - z) <= r:
            n = normal(v)
            out.append((round(cx, 2), round(cz, 2), round(min(ys), 2), round(max(ys), 2), round(n[0], 2), round(n[2], 2)))
    out = sorted(set(out))
    print(f"\nwall triangles within {r} blocks horizontally (centre x, z, y range, normal x, z): {len(out)}")
    for w in out[:40]:
        print("  ", w)


def slices(d, x, y, z, r):
    # top-down at heights y+0.25 and y+1.0: '#' solid voxel at the cell centre, '.' free, '?' unknown,
    # 'W' a wall triangle crosses the cell at that height, '@' the given position. 2 chars per block.
    for h in (y + 0.25, y + 1.0):
        print(f"\ntop-down slice at y {h:.2f} (x left->right {x - r:.1f}..{x + r:.1f}, z top->bottom {z - r:.1f}..{z + r:.1f}; 2 cells/block)")
        walls = []
        for v, f in all_tris(d):
            if kind(v) == "wall" and min(v[1::3]) <= h <= max(v[1::3]):
                walls.append((min(v[0::3]), max(v[0::3]), min(v[2::3]), max(v[2::3])))
        n = int(r * 2)
        for j in range(-n, n):
            zz = z + (j + 0.5) / 2
            row = ""
            for i in range(-n, n):
                xx = x + (i + 0.5) / 2
                if abs(xx - x) <= 0.25 and abs(zz - z) <= 0.25:
                    row += "@"
                    continue
                if any(a - 0.25 <= xx <= b + 0.25 and c - 0.25 <= zz <= e + 0.25 for a, b, c, e in walls):
                    row += "W"
                    continue
                s = solid(d["vox"], xx, h, zz)
                row += "?" if s is None else "#" if s else "."
            print("  " + row)
    print(f"\nvertical slice along x at z {z:.2f} (y top->bottom {y + 6:.1f}..{y - 3:.1f}; 2 cells/block; voxels)")
    for k in range(18, -6, -1):
        yy = y + (k + 0.5) / 2
        row = ""
        for i in range(-int(r * 2), int(r * 2)):
            xx = x + (i + 0.5) / 2
            s = solid(d["vox"], xx, yy, z)
            row += "?" if s is None else "#" if s else "."
        print(f"  {yy:7.2f} {row}")


def floor_map(d, x, y, z, r, res=0.25):
    """Top-down map from the triangles: W a wall crossing y+1 within the cell, 0 a floor at the feet
    (within 0.15), + / - a floor above / below them (within +1 / -3), blank none; @ the position."""
    near = [(v, f) for v, f in all_tris(d)
            if min(v[0::3]) <= x + r + 1 and max(v[0::3]) >= x - r - 1 and min(v[2::3]) <= z + r + 1 and max(v[2::3]) >= z - r - 1
            and min(v[1::3]) <= y + 3 and max(v[1::3]) >= y - 4]
    walls = [v for v, f in near if kind(v) == "wall" and min(v[1::3]) <= y + 1.0 <= max(v[1::3])]
    floors = [v for v, f in near if kind(v) in ("floor", "slope")]

    def wall_cell(cx, cz):
        for v in walls:
            if not (min(v[0::3]) - res / 2 <= cx <= max(v[0::3]) + res / 2 and min(v[2::3]) - res / 2 <= cz <= max(v[2::3]) + res / 2):
                continue
            n = normal(v)
            if abs((cx - v[0]) * n[0] + (cz - v[2]) * n[2]) <= res / 2:
                return True
        return False

    print(f"\nmap around MC {x:.2f} {y:.2f} {z:.2f} ({res} blocks a cell, x left->right, z top->bottom): "
          "W wall at y+1, 0 floor at the feet, +/- floor above/below, blank none, @ here")
    n = int(r / res)
    for j in range(-n, n):
        cz = z + (j + 0.5) * res
        row = ""
        for i in range(-n, n):
            cx = x + (i + 0.5) * res
            if abs(cx - x) <= res / 2 and abs(cz - z) <= res / 2:
                row += "@"
                continue
            if wall_cell(cx, cz):
                row += "W"
                continue
            best = None
            for v in floors:
                h = height_at(v, cx, cz)
                if h is not None and y - 3 <= h <= y + 1.0 and (best is None or h > best):
                    best = h
            row += " " if best is None else "0" if abs(best - y) < 0.15 else "+" if best > y else "-"
        print("  " + row)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--at", nargs=3, type=float, metavar=("X", "Y", "Z"), help="MC position to inspect")
    ap.add_argument("--span", type=float, default=6.0, help="vertical range around Y for the column profile")
    ap.add_argument("--slice", action="store_true", help="print ASCII slices around --at")
    ap.add_argument("--map", action="store_true", help="print a floor/wall map around --at")
    ap.add_argument("--radius", type=float, default=6.0, help="slice / map / wall search radius (blocks)")
    args = ap.parse_args()
    d = load()
    ntris = sum(len(v) for v in d["tris"].values())
    nblocks = sum(len(v) for v in d["vox"].values())
    print(f"ring head {d['head']} tail {d['tail']}: {d['msgs']} messages, {d['clears']} clears, epoch {d['epoch']}; "
          f"{len(d['vox'])} voxel regions ({nblocks} blocks), {len(d['tris'])} triangle regions ({ntris} triangles)")
    kinds = {}
    for lst in d["tris"].values():
        for v, f in lst:
            kinds[kind(v)] = kinds.get(kind(v), 0) + 1
    print("triangles by kind:", dict(sorted(kinds.items())))
    if args.at:
        x, y, z = args.at
        print(f"region at MC {x:.2f} {y:.2f} {z:.2f}: {(math.floor(x) // 8, math.floor(y) // 8, math.floor(z) // 8)}")
        profile(d, x, y, z, args.span)
        walls_near(d, x, y, z, args.radius)
        if args.slice:
            slices(d, x, y, z, args.radius)
        if args.map:
            floor_map(d, x, y, z, args.radius)


if __name__ == "__main__":
    main()
