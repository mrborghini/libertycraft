package dev.libertycraft.link;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import org.jspecify.annotations.Nullable;

/**
 * Ships overlay frames into the triple buffer with only what changed (the protocol's dirty tiles,
 * kOverlayFlagTiles). Each frame is compared with the last one published, tile by tile; a frame that is
 * the same isn't published at all (GTA IV keeps showing the one it has). Each of the three slots is
 * brought up to date with just the tiles that changed since the frame it last held, and the frame's
 * header tells GTA IV which tiles changed since the previous one, or since the one before when GTA IV
 * never took the previous one (it was still unread when this one replaced it), so it uploads only those.
 * A 1080p overlay is 8 MB: copied whole every frame that was over 1 GB/s on each side. Not thread-safe
 * (Minecraft's render thread).
 */
public final class OverlayShipper {
	/** Where frames go: the triple buffer (Link), or a test's stand-in. */
	public interface Target {
		MemorySegment memory();

		int backSlot();

		long backSlotOffset();

		boolean middleUnread();

		/** Publishes the back slot; {@code tiles} null: it says nothing about earlier frames. */
		void publish(int w, int h, long frameId, long baseFrameId, int @Nullable [] tiles);
	}

	private @Nullable MemorySegment ref; // the last frame published
	private int refW, refH, refGeneration = Integer.MIN_VALUE;
	private long published, lastFrameId;
	private final long[] tileChanged = new long[OverlayTiles.GRID * OverlayTiles.GRID]; // publish number of each tile's last change
	private final long[] slotHolds = new long[Proto.OVERLAY_SLOTS]; // publish number each slot holds, 0: unknown
	private final int[] changed = new int[OverlayTiles.WORDS];
	private final int[] forSlot = new int[OverlayTiles.WORDS];
	private final int[] header = new int[OverlayTiles.WORDS];
	private boolean headerTiles;
	private long headerBase;
	public long shipped, skipped, tilesShipped;

	/**
	 * Frame {@code src} ({@code w x h} RGBA): into the back slot and published, unless it equals the last
	 * one. {@code generation}: a new value (a new GTA IV) forgets what the slots hold. Returns whether it
	 * was published.
	 */
	public boolean ship(MemorySegment src, int w, int h, long frameId, int generation, Target target) {
		long bytes = (long) w * h * 4L;
		if (src.byteSize() < bytes) {
			return false;
		}
		boolean whole = this.ref == null || this.refW != w || this.refH != h || this.refGeneration != generation;
		MemorySegment ref = this.ref;
		if (whole) {
			ref = this.ref = Arena.ofAuto().allocate(bytes, 64);
			this.refW = w;
			this.refH = h;
			this.refGeneration = generation;
			MemorySegment.copy(src, 0, ref, 0, bytes);
			OverlayTiles.fill(this.changed, true);
			java.util.Arrays.fill(this.slotHolds, 0L);
		} else if (!OverlayTiles.diffInto(src, ref, w, h, this.changed)) {
			this.skipped++;
			return false;
		}
		this.published++;
		for (int t = 0; t < this.tileChanged.length; t++) {
			if (OverlayTiles.get(this.changed, t)) {
				this.tileChanged[t] = this.published;
			}
		}
		MemorySegment mem = target.memory();
		int back = target.backSlot();
		long holds = this.slotHolds[back];
		if (holds == 0L) {
			MemorySegment.copy(ref, 0, mem, target.backSlotOffset(), bytes);
		} else {
			OverlayTiles.fill(this.forSlot, false);
			for (int t = 0; t < this.tileChanged.length; t++) {
				if (this.tileChanged[t] > holds) {
					OverlayTiles.set(this.forSlot, t);
				}
			}
			OverlayTiles.copyTiles(ref, 0, mem, target.backSlotOffset(), w, h, this.forSlot);
		}
		this.slotHolds[back] = this.published;
		if (whole) {
			this.headerTiles = false;
		} else if (target.middleUnread()) {
			// The previous frame is about to be replaced unread: these tiles add to its.
			for (int i = 0; i < this.header.length; i++) {
				this.header[i] |= this.changed[i];
			}
		} else {
			this.headerTiles = true;
			this.headerBase = this.lastFrameId;
			System.arraycopy(this.changed, 0, this.header, 0, this.header.length);
		}
		target.publish(w, h, frameId, this.headerBase, this.headerTiles ? this.header : null);
		this.lastFrameId = frameId;
		this.shipped++;
		for (int word : this.changed) {
			this.tilesShipped += Integer.bitCount(word);
		}
		return true;
	}
}
