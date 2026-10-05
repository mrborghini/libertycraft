package dev.libertycraft.link;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.HashMap;
import java.util.Map;
import java.util.Random;
import org.junit.jupiter.api.Test;

/**
 * The overlay's dirty tiles end to end: OverlayShipper writing into a simulated triple buffer, and a reader
 * that keeps a copy the way the plugin does (render/OverlayTiles.h: pending tiles since the frame it holds,
 * a whole copy when the header's base isn't the frame it took last). Whatever the reader skips, its copy
 * must always be the frame it took.
 */
class OverlayShipperTest {
	@Test
	void diffMarksExactlyTheChangedTiles() {
		int w = 100, h = 70; // tiles 7 x 5, the last ones short
		MemorySegment a = frame(w, h), b = frame(w, h);
		int[] mask = new int[OverlayTiles.WORDS];
		assertFalse(OverlayTiles.diffInto(a, b, w, h, mask));
		poke(a, w, 99, 69, 7); // the last pixel: tile (14, 13)
		poke(a, w, 8, 6, 9);   // tile (1, 1)
		poke(a, w, 13, 6, 9);  // tile (1, 1) again
		assertTrue(OverlayTiles.diffInto(a, b, w, h, mask));
		int n = 0;
		for (int t = 0; t < 256; t++) {
			if (OverlayTiles.get(mask, t)) {
				n++;
			}
		}
		assertEquals(2, n);
		assertTrue(OverlayTiles.get(mask, 13 * 16 + 14));
		assertTrue(OverlayTiles.get(mask, 16 + 1));
		assertEquals(-1, MemorySegment.mismatch(a, 0, a.byteSize(), b, 0, b.byteSize())); // the reference caught up
	}

	@Test
	void theReadersCopyIsAlwaysTheFrameItTook() {
		for (long seed = 1; seed <= 6; seed++) {
			run(new Random(seed), seed % 2 == 0 ? 100 : 37, seed % 2 == 0 ? 70 : 23, 600);
		}
	}

	private static void run(Random rnd, int w, int h, int steps) {
		long bytes = (long) w * h * 4;
		Sim sim = new Sim(w, h, rnd);
		OverlayShipper shipper = new OverlayShipper();
		MemorySegment cur = frame(w, h);
		long frameId = 0;
		int wholeUploads = 0, partialUploads = 0;
		for (int step = 0; step < steps; step++) {
			// the next frame: a few small changes, now and then none or a big one
			int changes = rnd.nextInt(10) < 2 ? 0 : 1 + rnd.nextInt(rnd.nextInt(20) == 0 ? 40 : 3);
			for (int c = 0; c < changes; c++) {
				int x0 = rnd.nextInt(w), y0 = rnd.nextInt(h), x1 = Math.min(w, x0 + 1 + rnd.nextInt(12)), y1 = Math.min(h, y0 + 1 + rnd.nextInt(12));
				for (int y = y0; y < y1; y++) {
					for (int x = x0; x < x1; x++) {
						poke(cur, w, x, y, rnd.nextInt());
					}
				}
			}
			frameId += 1 + rnd.nextInt(2); // captures skipped now and then
			int generation = step == steps / 2 ? 1 : 0; // a new GTA IV halfway
			if (shipper.ship(cur, w, h, frameId, generation, sim)) {
				sim.frames.put(frameId, copy(cur, bytes));
			}
			// the reader takes frames and uploads them when it likes
			if (rnd.nextInt(10) < 7) {
				sim.acquire();
			}
			if (sim.acquiredAny && rnd.nextInt(10) < 8) {
				if (sim.upload()) {
					wholeUploads++;
				} else {
					partialUploads++;
				}
				MemorySegment expected = sim.frames.get(sim.coveredTo);
				assertEquals(-1, MemorySegment.mismatch(sim.copy, 0, bytes, expected, 0, bytes), "the reader's copy is frame " + sim.coveredTo);
			}
		}
		assertTrue(partialUploads > wholeUploads, partialUploads + " partial uploads, " + wholeUploads + " whole ones");
	}

	/** The triple buffer and the plugin's reader. */
	private static final class Sim implements OverlayShipper.Target {
		final int w, h;
		final long bytes;
		final Random rnd;
		final MemorySegment mem;
		final long[] hdrFrame = new long[3], hdrBase = new long[3];
		final int[][] hdrTiles = new int[3][];
		final int[] hdrW = new int[3], hdrH = new int[3];
		int middle = 0, back = 1, front = 2;
		boolean dirty;
		final Map<Long, MemorySegment> frames = new HashMap<>();
		// reader
		final MemorySegment copy;
		boolean acquiredAny, covered, all = true;
		long coveredTo;
		int coveredW, coveredH;
		final int[] pending = new int[OverlayTiles.WORDS];

		Sim(int w, int h, Random rnd) {
			this.w = w;
			this.h = h;
			this.bytes = (long) w * h * 4;
			this.rnd = rnd;
			this.mem = Arena.ofAuto().allocate(this.bytes * 3);
			this.copy = Arena.ofAuto().allocate(this.bytes);
		}

		@Override
		public MemorySegment memory() {
			return this.mem;
		}

		@Override
		public int backSlot() {
			return this.back;
		}

		@Override
		public long backSlotOffset() {
			return this.back * this.bytes;
		}

		@Override
		public boolean middleUnread() {
			boolean unread = this.dirty;
			if (this.rnd.nextInt(8) == 0) {
				acquire(); // the reader takes it right after the writer looked
			}
			return unread;
		}

		@Override
		public void publish(int pw, int ph, long frameId, long baseFrameId, int[] tiles) {
			this.hdrFrame[this.back] = frameId;
			this.hdrBase[this.back] = baseFrameId;
			this.hdrTiles[this.back] = tiles != null ? tiles.clone() : null;
			this.hdrW[this.back] = pw;
			this.hdrH[this.back] = ph;
			int old = this.middle;
			this.middle = this.back;
			this.dirty = true;
			this.back = old;
		}

		void acquire() {
			if (!this.dirty) {
				return;
			}
			int old = this.middle;
			this.middle = this.front;
			this.front = old;
			this.dirty = false;
			this.acquiredAny = true;
			int[] tiles = this.hdrTiles[this.front];
			if (tiles != null && this.covered && this.hdrBase[this.front] == this.coveredTo && this.hdrW[this.front] == this.coveredW && this.hdrH[this.front] == this.coveredH) {
				for (int i = 0; i < tiles.length; i++) {
					this.pending[i] |= tiles[i];
				}
			} else {
				this.all = true;
			}
			this.covered = true;
			this.coveredTo = this.hdrFrame[this.front];
			this.coveredW = this.hdrW[this.front];
			this.coveredH = this.hdrH[this.front];
		}

		/** Returns true for a whole copy. */
		boolean upload() {
			boolean whole = this.all;
			if (whole) {
				MemorySegment.copy(this.mem, this.front * this.bytes, this.copy, 0, this.bytes);
			} else {
				OverlayTiles.copyTiles(this.mem, this.front * this.bytes, this.copy, 0, this.w, this.h, this.pending);
			}
			this.all = false;
			OverlayTiles.fill(this.pending, false);
			return whole;
		}
	}

	private static MemorySegment frame(int w, int h) {
		return Arena.ofAuto().allocate((long) w * h * 4);
	}

	private static MemorySegment copy(MemorySegment s, long bytes) {
		MemorySegment c = Arena.ofAuto().allocate(bytes);
		MemorySegment.copy(s, 0, c, 0, bytes);
		return c;
	}

	private static void poke(MemorySegment s, int w, int x, int y, int v) {
		s.set(ValueLayout.JAVA_INT_UNALIGNED, ((long) y * w + x) * 4, v);
	}
}
