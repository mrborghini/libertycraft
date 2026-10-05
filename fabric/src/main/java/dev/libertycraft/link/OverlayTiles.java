package dev.libertycraft.link;

import java.lang.foreign.MemorySegment;

/**
 * The overlay's dirty tiles (the protocol's kOverlayFlagTiles): a frame cut into a 16 x 16 grid of
 * tiles {@code ceil(w / 16) x ceil(h / 16)} pixels, the last row and column cut short. A tile mask is
 * 8 ints, tile (tx, ty) is bit {@code (ty * 16 + tx) % 32} of word {@code (ty * 16 + tx) / 32}. Frames are
 * RGBA, 4 bytes a pixel, rows tightly packed.
 */
public final class OverlayTiles {
	public static final int GRID = Proto.OVERLAY_TILE_GRID;
	public static final int WORDS = GRID * GRID / 32;

	private OverlayTiles() {
	}

	public static int tileW(int w) {
		return (w + GRID - 1) / GRID;
	}

	public static int tileH(int h) {
		return (h + GRID - 1) / GRID;
	}

	public static boolean get(int[] mask, int tile) {
		return (mask[tile >>> 5] >>> (tile & 31) & 1) != 0;
	}

	public static void set(int[] mask, int tile) {
		mask[tile >>> 5] |= 1 << (tile & 31);
	}

	public static boolean isEmpty(int[] mask) {
		for (int m : mask) {
			if (m != 0) {
				return false;
			}
		}
		return true;
	}

	public static void fill(int[] mask, boolean all) {
		java.util.Arrays.fill(mask, all ? -1 : 0);
	}

	/** Tile row ty's 16 bits of the mask. */
	private static int band(int[] mask, int ty) {
		return mask[ty >>> 1] >>> ((ty & 1) << 4) & 0xFFFF;
	}

	/**
	 * Marks in {@code out} (cleared first) the tiles where frame {@code src} differs from {@code ref} and
	 * copies them into {@code ref}, so it holds {@code src} again. Compares whole rows first (most of an
	 * overlay frame is the same transparent nothing as before), then the tiles of the rows that differ.
	 * Returns true if any tile differs.
	 */
	public static boolean diffInto(MemorySegment src, MemorySegment ref, int w, int h, int[] out) {
		fill(out, false);
		long rowBytes = w * 4L;
		int tw = tileW(w), th = tileH(h);
		boolean any = false;
		for (int ty = 0; ty < GRID; ty++) {
			int y0 = ty * th, y1 = Math.min(h, y0 + th);
			if (y0 >= h) {
				break;
			}
			int bandMask = 0;
			int full = (1 << Math.min(GRID, (w + tw - 1) / tw)) - 1; // tiles that exist in a row
			for (int y = y0; y < y1 && bandMask != full; y++) {
				long off = y * rowBytes;
				long m = MemorySegment.mismatch(src, off, off + rowBytes, ref, off, off + rowBytes);
				if (m < 0) {
					continue;
				}
				int first = (int) (m / 4) / tw;
				bandMask |= 1 << first;
				for (int tx = first + 1; tx < GRID; tx++) {
					int x0 = tx * tw;
					if (x0 >= w) {
						break;
					}
					if ((bandMask >>> tx & 1) != 0) {
						continue;
					}
					long a = off + x0 * 4L, b = off + Math.min(w, x0 + tw) * 4L;
					if (MemorySegment.mismatch(src, a, b, ref, a, b) >= 0) {
						bandMask |= 1 << tx;
					}
				}
			}
			if (bandMask != 0) {
				any = true;
				out[ty >>> 1] |= bandMask << ((ty & 1) << 4);
			}
		}
		if (any) {
			copyTiles(src, 0, ref, 0, w, h, out);
		}
		return any;
	}

	/**
	 * Copies the tiles set in {@code mask} of a frame at {@code fromOff} in {@code from} to the same place of
	 * the frame at {@code toOff} in {@code to} (neighbouring tiles of a row as one span).
	 */
	public static void copyTiles(MemorySegment from, long fromOff, MemorySegment to, long toOff, int w, int h, int[] mask) {
		long rowBytes = w * 4L;
		int tw = tileW(w), th = tileH(h);
		for (int ty = 0; ty < GRID; ty++) {
			int y0 = ty * th, y1 = Math.min(h, y0 + th);
			if (y0 >= h) {
				break;
			}
			int bits = band(mask, ty);
			int tx = 0;
			while (bits >>> tx != 0) {
				if ((bits >>> tx & 1) == 0) {
					tx++;
					continue;
				}
				int end = tx;
				while (end + 1 < GRID && (bits >>> (end + 1) & 1) != 0) {
					end++;
				}
				int x0 = tx * tw, x1 = Math.min(w, (end + 1) * tw);
				if (x0 < w) {
					long len = (x1 - x0) * 4L;
					for (int y = y0; y < y1; y++) {
						long at = y * rowBytes + x0 * 4L;
						MemorySegment.copy(from, fromOff + at, to, toOff + at, len);
					}
				}
				tx = end + 1;
			}
		}
	}
}
