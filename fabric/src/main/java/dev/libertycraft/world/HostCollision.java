package dev.libertycraft.world;

import static dev.libertycraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import it.unimi.dsi.fastutil.longs.Long2ObjectMap;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import java.lang.foreign.MemorySegment;
import java.util.HashMap;
import java.util.Iterator;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.shapes.BitSetDiscreteVoxelShape;
import net.minecraft.world.phys.shapes.CubeVoxelShape;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * Liberty City geometry as Minecraft sees it: an 8x8x8 sub-voxel collision shape per block
 * position, streamed from the ASI plugin. These are not blocks; they are merged into block
 * collision queries (see BlockCollisionsMixin) so vanilla movement code collides with them.
 *
 * <p>Memory: GTA IV streams the 8x8x8-block regions around the player and forgets columns of them
 * once the player is 7 regions away, telling us so (kColForget); it probes and sends a forgotten
 * column again before the player gets near. What it forgot is dropped here too once the player is
 * far enough from it: the triangles beyond {@link #KEEP_TRIS} regions (they serve what happens near
 * the player: the smooth collider, arrows, digging), the voxels beyond {@link #KEEP_VOXELS} (mobs
 * and items far out still stand on them while Minecraft ticks them). Without that a long drive kept
 * every region it passed (half a gigabyte every few minutes at speed, until Minecraft ran out of
 * memory). Voxels are kept per region (a mask and the occupied blocks' cells, one shared cell per
 * distinct shape) rather than per block.
 */
public final class HostCollision {
	/** GTA IV regions are streamed as cubes of this many blocks. Must match the ASI side. */
	public static final int REGION_SIZE = 8;
	/** Forgotten columns keep their triangles while this near the player (regions, either axis). */
	static final int KEEP_TRIS = 8;
	/** Forgotten columns keep their voxels while this near: Minecraft's simulation distance (8 chunks) and a chunk. */
	static final int KEEP_VOXELS = 18;
	private static final long EVICT_EVERY_NANOS = 1_000_000_000L;
	private static final long STATS_EVERY_NANOS = 60_000_000_000L;
	private static final int MAX_INTERNED = 16384;

	private static final int FILL_LOWER = 1 << 10;
	private static final int FILL_UPPER = 1 << 11;
	private static final int FILL_TOP_SHIFT = 12; // highest occupied of the 8 voxel layers (3 bits)

	/**
	 * One block's GTA IV geometry: its shape, and its fill word (sub-voxel count in bits 0 to 9, any in the
	 * lower half bit 10, any in the upper half bit 11, the highest occupied layer from bit 12). Blocks with
	 * the same sub-voxels share one.
	 */
	private record Cell(VoxelShape shape, int fill) {
	}

	/** One block's 8x8x8 sub-voxel mask, as the key that finds its shared Cell. */
	private record Mask(long l0, long l1, long l2, long l3, long l4, long l5, long l6, long l7) {
		// The record's own hash folds each long to (int) (l ^ l >>> 32): 0 for both an empty and a full
		// layer, so most ground and wall masks would share a handful of hashes.
		@Override
		public int hashCode() {
			long h = mix(mix(mix(mix(mix(mix(mix(mix(0, this.l0), this.l1), this.l2), this.l3), this.l4), this.l5), this.l6), this.l7);
			return (int) (h ^ h >>> 32);
		}

		private static long mix(long h, long l) {
			h = (h ^ l) * 0x9E3779B97F4A7C15L;
			return h ^ h >>> 29;
		}

		@Override
		public boolean equals(Object o) {
			return o instanceof Mask m && m.l0 == this.l0 && m.l1 == this.l1 && m.l2 == this.l2 && m.l3 == this.l3 && m.l4 == this.l4 && m.l5 == this.l5
				&& m.l6 == this.l6 && m.l7 == this.l7;
		}
	}

	/**
	 * One region's voxels. Block (x, y, z) of the region is bit {@code x + 8 z} of {@code mask[y]}; the
	 * cells of the set bits follow in that order ({@code prefix[y]}: the set bits before word y). Immutable.
	 */
	private static final class Voxels {
		static final Voxels EMPTY = new Voxels(new long[8], new short[8], new Cell[0]);

		final long[] mask;
		final short[] prefix;
		final Cell[] cells;

		Voxels(long[] mask, short[] prefix, Cell[] cells) {
			this.mask = mask;
			this.prefix = prefix;
			this.cells = cells;
		}

		@Nullable Cell at(int lx, int ly, int lz) {
			long word = this.mask[ly];
			int bit = lx | lz << 3;
			if ((word >>> bit & 1L) == 0) {
				return null;
			}
			return this.cells[this.prefix[ly] + Long.bitCount(word & ((1L << bit) - 1))];
		}

		/** The cells into {@code out} (512, index x + 8 z + 64 y; null: nothing there). */
		void toDense(@Nullable Cell[] out) {
			int n = 0;
			for (int i = 0; i < 512; i++) {
				out[i] = (this.mask[i >> 6] >>> (i & 63) & 1L) != 0 ? this.cells[n++] : null;
			}
		}

		static Voxels of(@Nullable Cell[] dense) {
			int count = 0;
			for (Cell c : dense) {
				if (c != null) {
					count++;
				}
			}
			if (count == 0) {
				return EMPTY;
			}
			long[] mask = new long[8];
			short[] prefix = new short[8];
			Cell[] cells = new Cell[count];
			int n = 0;
			for (int y = 0; y < 8; y++) {
				prefix[y] = (short) n;
				for (int b = 0; b < 64; b++) {
					Cell c = dense[y << 6 | b];
					if (c != null) {
						mask[y] |= 1L << b;
						cells[n++] = c;
					}
				}
			}
			return new Voxels(mask, prefix, cells);
		}
	}

	// Written by the consumer thread only, read by any thread.
	private static final ConcurrentHashMap<Long, Voxels> VOXELS = new ConcurrentHashMap<>(); // every region GTA IV sent (EMPTY: none there)
	private static final ConcurrentHashMap<Long, HostTri[]> TRIS = new ConcurrentHashMap<>();
	// Diggable surfaces as they were before blocks were dug out of them (Proto.TRI_GHOST).
	private static final ConcurrentHashMap<Long, HostTri[]> GHOSTS = new ConcurrentHashMap<>();
	// A hash of each region's triangles as last received, and the regions whose triangles changed
	// since the client last looked (the walls of dug holes are drawn from them).
	private static final ConcurrentHashMap<Long, Long> TRI_HASH = new ConcurrentHashMap<>();
	private static final java.util.concurrent.ConcurrentLinkedQueue<Long> CHANGED = new java.util.concurrent.ConcurrentLinkedQueue<>();

	/** A column of regions we hold (consumer thread only): its rows, and whether GTA IV forgot it. */
	private static final class Column {
		int minRy = Integer.MAX_VALUE, maxRy = Integer.MIN_VALUE;
		boolean forgotten, trisDropped;
	}

	// Consumer thread only.
	private static final Long2ObjectOpenHashMap<Column> COLUMNS = new Long2ObjectOpenHashMap<>();
	private static final HashMap<Mask, Cell> INTERNED = new HashMap<>();
	private static final @Nullable Cell[] DENSE = new Cell[512];
	private static final Link.SkyState SKY = new Link.SkyState();
	private static long lastEvict, lastStats;
	private static long forgets, dropped, trisDropped;

	/** Regions (min corner, as BlockPos longs) whose triangles changed since the last call. */
	public static void takeChangedRegions(java.util.function.LongConsumer out) {
		Long key;
		while ((key = CHANGED.poll()) != null) {
			out.accept(key);
		}
	}
	private static volatile java.util.function.Predicate<net.minecraft.world.entity.Entity> smoothCollider = e -> false;
	private static volatile int epoch = -1;
	private static Thread consumer;

	private HostCollision() {
	}

	private static @Nullable Cell cellAt(int x, int y, int z) {
		Voxels v = VOXELS.get(regionKey(x >> 3, y >> 3, z >> 3));
		return v == null ? null : v.at(x & 7, y & 7, z & 7);
	}

	public static @Nullable VoxelShape shapeAt(BlockPos pos) {
		if (VOXELS.isEmpty()) {
			return null;
		}
		Cell c = cellAt(pos.getX(), pos.getY(), pos.getZ());
		return c == null ? null : c.shape();
	}

	/** Entities (the local player) that collide with GTA IV's exact triangles instead of its voxels. */
	public static void setSmoothCollider(java.util.function.Predicate<net.minecraft.world.entity.Entity> predicate) {
		smoothCollider = predicate;
	}

	public static boolean usesSmoothCollider(net.minecraft.world.entity.@Nullable Entity entity) {
		return entity != null && smoothCollider.test(entity);
	}

	/** Adds every GTA IV triangle whose bounds overlap {@code box}. */
	public static void trianglesNear(net.minecraft.world.phys.AABB box, java.util.List<HostTri> out) {
		near(TRIS, box, out);
	}

	/**
	 * Every GTA IV surface whose bounds overlap {@code box} as it was before anything was dug out
	 * of it: what's behind these is inside GTA IV's geometry (HostDig).
	 */
	public static void originalSurfacesNear(net.minecraft.world.phys.AABB box, java.util.List<HostTri> out) {
		trianglesNear(box, out);
		near(GHOSTS, box, out);
	}

	private static void near(ConcurrentHashMap<Long, HostTri[]> store, net.minecraft.world.phys.AABB box, java.util.List<HostTri> out) {
		if (store.isEmpty()) {
			return;
		}
		int rx0 = Math.floorDiv((int) Math.floor(box.minX), REGION_SIZE), rx1 = Math.floorDiv((int) Math.floor(box.maxX), REGION_SIZE);
		int ry0 = Math.floorDiv((int) Math.floor(box.minY), REGION_SIZE), ry1 = Math.floorDiv((int) Math.floor(box.maxY), REGION_SIZE);
		int rz0 = Math.floorDiv((int) Math.floor(box.minZ), REGION_SIZE), rz1 = Math.floorDiv((int) Math.floor(box.maxZ), REGION_SIZE);
		for (int rx = rx0; rx <= rx1; rx++) {
			for (int ry = ry0; ry <= ry1; ry++) {
				for (int rz = rz0; rz <= rz1; rz++) {
					HostTri[] tris = store.get(regionKey(rx, ry, rz));
					if (tris == null) {
						continue;
					}
					for (HostTri t : tris) {
						if (t.maxX >= box.minX && t.minX <= box.maxX && t.maxY >= box.minY && t.minY <= box.maxY && t.maxZ >= box.minZ && t.minZ <= box.maxZ) {
							out.add(t);
						}
					}
				}
			}
		}
	}

	/** The collision epoch the regions we hold belong to (-1 before the first message). */
	public static int epoch() {
		return epoch;
	}

	/** True once GTA IV has sent the region containing this block (even if it was empty). */
	public static boolean isKnown(int x, int y, int z) {
		return VOXELS.containsKey(regionKey(x >> 3, y >> 3, z >> 3));
	}

	/** True if any GTA IV geometry exists in the 3x3 column below (x, y, z), down to {@code depth} blocks. */
	public static boolean hasSolidBelow(int x, int y, int z, int depth) {
		for (int dy = 0; dy <= depth; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				for (int dz = -1; dz <= 1; dz++) {
					if (cellAt(x + dx, y - dy, z + dz) != null) {
						return true;
					}
				}
			}
		}
		return false;
	}

	/** Fraction (0..1) of this block's volume that is GTA IV geometry. */
	public static float solidFraction(BlockPos pos) {
		Cell c = VOXELS.isEmpty() ? null : cellAt(pos.getX(), pos.getY(), pos.getZ());
		return c == null ? 0.0F : (c.fill() & 0x3FF) / 512.0F;
	}

	/** True if any GTA IV geometry is in this cell. */
	public static boolean hasGeometry(BlockPos pos) {
		return !VOXELS.isEmpty() && cellAt(pos.getX(), pos.getY(), pos.getZ()) != null;
	}

	/**
	 * How high (0..1) GTA IV geometry reaches in this cell: the top of its highest part. Terrain
	 * arrives as a thin surface, so what lies below that surface counts as ground too.
	 */
	public static float groundTop(BlockPos pos) {
		Cell c = VOXELS.isEmpty() ? null : cellAt(pos.getX(), pos.getY(), pos.getZ());
		return c == null ? 0.0F : (((c.fill() >> FILL_TOP_SHIFT) & 7) + 1) / 8.0F;
	}

	/** True if GTA IV ground holds up whatever is in this cell (terrain in its lower half or the top of the cell below). */
	public static boolean supportsFromBelow(BlockPos pos) {
		if (VOXELS.isEmpty()) {
			return false;
		}
		Cell here = cellAt(pos.getX(), pos.getY(), pos.getZ());
		if (here != null && (here.fill() & FILL_LOWER) != 0) {
			return true;
		}
		Cell below = cellAt(pos.getX(), pos.getY() - 1, pos.getZ());
		return below != null && (below.fill() & FILL_UPPER) != 0;
	}

	/** Blocks holding GTA IV geometry (walks every region). */
	public static int blockCount() {
		int n = 0;
		for (Voxels v : VOXELS.values()) {
			n += v.cells.length;
		}
		return n;
	}

	public static int regionCount() {
		return VOXELS.size();
	}

	/** GTA IV is describing its world around the player (false in a plain Minecraft world). */
	public static boolean active() {
		return !VOXELS.isEmpty();
	}

	private static long regionKey(int rx, int ry, int rz) {
		return BlockPos.asLong(rx, ry, rz);
	}

	private static long columnKey(int rx, int rz) {
		return (long) rx << 32 | (rz & 0xFFFFFFFFL);
	}

	public static synchronized void startConsumer() {
		if (consumer != null) {
			return;
		}
		consumer = new Thread(HostCollision::consumeLoop, "LibertyCraft collision");
		consumer.setDaemon(true);
		consumer.start();
	}

	private static void consumeLoop() {
		while (true) {
			try {
				boolean busy = drainOnce();
				long now = System.nanoTime();
				if (now - lastEvict >= EVICT_EVERY_NANOS) {
					lastEvict = now;
					evictFar(now);
				}
				if (!busy) {
					Thread.sleep(2);
				}
			} catch (InterruptedException e) {
				return;
			} catch (Throwable t) {
				LibertyCraft.LOG.error("[LibertyCraft] collision consumer error", t);
				try {
					Thread.sleep(500);
				} catch (InterruptedException e) {
					return;
				}
			}
		}
	}

	/** Processes all pending collision messages. Returns true if anything was consumed. */
	private static boolean drainOnce() {
		MemorySegment s = Link.segment();
		if (s == null) {
			return false;
		}
		long head = Link.collisionHead();
		long tail = Link.collisionTail();
		if (tail >= head) {
			return false;
		}
		long data = OFF_COLLISION_RING + CR_DATA;
		while (tail < head) {
			long pos = tail % CR_DATA_BYTES;
			int type = s.get(JAVA_INT, data + pos);
			int payloadBytes = s.get(JAVA_INT, data + pos + 4);
			if (type == COL_PAD) {
				tail += CR_DATA_BYTES - pos;
				continue;
			}
			handle(s, type, data + pos + 8);
			tail += align8(8 + payloadBytes);
		}
		Link.setCollisionTail(tail);
		return true;
	}

	/** One collision message (its payload at {@code payload}). Consumer thread (and tests). */
	static void handle(MemorySegment s, int type, long payload) {
		switch (type) {
			case COL_CLEAR -> clear(s.get(JAVA_INT, payload));
			case COL_REGION -> readRegion(s, payload);
			case COL_TRIS -> readTris(s, payload);
			case COL_FORGET -> forget(s, payload);
			default -> LibertyCraft.LOG.warn("[LibertyCraft] unknown collision message {}", type);
		}
	}

	private static long align8(long v) {
		return (v + 7) & ~7L;
	}

	/** A freshly started client joins whatever collision epoch GTA IV is already on. */
	private static void adoptEpochIfFresh(int msgEpoch) {
		if (epoch == -1) {
			epoch = msgEpoch;
			LibertyCraft.LOG.info("[LibertyCraft] joined collision epoch {} already in progress", msgEpoch);
		}
	}

	private static void clear(int newEpoch) {
		VOXELS.clear();
		TRIS.clear();
		GHOSTS.clear();
		TRI_HASH.clear();
		COLUMNS.clear();
		INTERNED.clear();
		epoch = newEpoch;
		LibertyCraft.LOG.info("[LibertyCraft] collision cleared (epoch {})", newEpoch);
	}

	/** The column of region (rx, ry, rz) just got data from GTA IV: it holds row ry, and GTA IV tracks it again. */
	private static Column noteRow(int rx, int ry, int rz) {
		Column c = COLUMNS.get(columnKey(rx, rz));
		if (c == null) {
			c = new Column();
			COLUMNS.put(columnKey(rx, rz), c);
		}
		c.minRy = Math.min(c.minRy, ry);
		c.maxRy = Math.max(c.maxRy, ry);
		c.forgotten = false;
		return c;
	}

	private static void readRegion(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minY = s.get(JAVA_INT, p + 4);
		int minZ = s.get(JAVA_INT, p + 8);
		int maxX = s.get(JAVA_INT, p + 12);
		int maxY = s.get(JAVA_INT, p + 16);
		int maxZ = s.get(JAVA_INT, p + 20);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);
		adoptEpochIfFresh(msgEpoch);
		if (msgEpoch != epoch) {
			return; // stale region from before a world change
		}
		dev.libertycraft.world.city.CityRecorder.region(s, p); // kept for the blocky city

		// Every region the box touches is rebuilt whole and swapped in, so readers never see a half-done one.
		for (int rx = minX >> 3; rx <= maxX >> 3; rx++) {
			for (int ry = minY >> 3; ry <= maxY >> 3; ry++) {
				for (int rz = minZ >> 3; rz <= maxZ >> 3; rz++) {
					int x0 = rx << 3, y0 = ry << 3, z0 = rz << 3;
					boolean whole = minX <= x0 && maxX >= x0 + 7 && minY <= y0 && maxY >= y0 + 7 && minZ <= z0 && maxZ >= z0 + 7;
					long key = regionKey(rx, ry, rz);
					Voxels old = VOXELS.get(key);
					if (whole || old == null) {
						java.util.Arrays.fill(DENSE, null);
					} else {
						old.toDense(DENSE);
						// what the box covers is replaced
						for (int x = Math.max(minX, x0); x <= Math.min(maxX, x0 + 7); x++) {
							for (int y = Math.max(minY, y0); y <= Math.min(maxY, y0 + 7); y++) {
								for (int z = Math.max(minZ, z0); z <= Math.min(maxZ, z0 + 7); z++) {
									DENSE[(x & 7) | (z & 7) << 3 | (y & 7) << 6] = null;
								}
							}
						}
					}
					long e = p + COL_REGION_HEADER_BYTES;
					for (int i = 0; i < count; i++, e += COL_BLOCK_BYTES) {
						int x = s.get(JAVA_INT, e);
						int y = s.get(JAVA_INT, e + 4);
						int z = s.get(JAVA_INT, e + 8);
						if (x >> 3 != rx || y >> 3 != ry || z >> 3 != rz || x < minX || x > maxX || y < minY || y > maxY || z < minZ || z > maxZ) {
							continue;
						}
						DENSE[(x & 7) | (z & 7) << 3 | (y & 7) << 6] = cell(s, e + 16);
					}
					VOXELS.put(key, Voxels.of(DENSE));
					noteRow(rx, ry, rz);
				}
			}
		}
	}

	private static void readTris(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minY = s.get(JAVA_INT, p + 4);
		int minZ = s.get(JAVA_INT, p + 8);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);
		adoptEpochIfFresh(msgEpoch);
		if (msgEpoch != epoch) {
			return;
		}
		dev.libertycraft.world.city.CityRecorder.tris(s, p); // the materials, for the blocky city
		HostTri[] tris = new HostTri[count];
		java.util.List<HostTri> ghosts = new java.util.ArrayList<>();
		float[] v = new float[9];
		int kept = 0;
		long hash = count;
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_TRI_BYTES) {
			for (int k = 0; k < 9; k++) {
				v[k] = s.get(JAVA_FLOAT, e + k * 4L);
				hash = hash * 31 + Float.floatToRawIntBits(v[k]);
			}
			int flags = s.get(JAVA_INT, e + 36);
			hash = hash * 31 + flags;
			HostTri t = new HostTri(v, 0, flags);
			if (t.degenerate()) {
				continue;
			}
			if ((flags & TRI_GHOST) != 0) {
				ghosts.add(t);
			} else {
				tris[kept++] = t;
			}
		}
		int rx = Math.floorDiv(minX, REGION_SIZE), ry = Math.floorDiv(minY, REGION_SIZE), rz = Math.floorDiv(minZ, REGION_SIZE);
		long region = regionKey(rx, ry, rz);
		if (ghosts.isEmpty()) {
			GHOSTS.remove(region);
		} else {
			GHOSTS.put(region, ghosts.toArray(new HostTri[0]));
		}
		TRIS.put(region, java.util.Arrays.copyOf(tris, kept));
		Long before = TRI_HASH.put(region, hash);
		if (before == null || before != hash) {
			CHANGED.add(BlockPos.asLong(minX, minY, minZ));
		}
		noteRow(rx, ry, rz).trisDropped = false;
	}

	/** kColForget: GTA IV forgot this column of regions; evictFar drops it once the player is far enough from it. */
	private static void forget(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minZ = s.get(JAVA_INT, p + 8);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		if (msgEpoch != epoch) {
			return;
		}
		Column c = COLUMNS.get(columnKey(minX >> 3, minZ >> 3));
		if (c != null) {
			c.forgotten = true;
			forgets++;
		}
	}

	/** Drops what GTA IV forgot and the player is far from (see the class comment). Consumer thread. */
	private static void evictFar(long now) {
		if (COLUMNS.isEmpty() || !Link.readSkyState(SKY)) {
			return;
		}
		evictAround((int) Math.floor(SKY.x) >> 3, (int) Math.floor(SKY.z) >> 3, now);
	}

	/** evictFar with the player in region column (px, pz). Consumer thread (and tests). */
	static void evictAround(int px, int pz, long now) {
		for (Iterator<Long2ObjectMap.Entry<Column>> it = COLUMNS.long2ObjectEntrySet().fastIterator(); it.hasNext(); ) {
			Long2ObjectMap.Entry<Column> entry = it.next();
			Column c = entry.getValue();
			if (!c.forgotten) {
				continue;
			}
			int rx = (int) (entry.getLongKey() >> 32), rz = (int) entry.getLongKey();
			int d = Math.max(Math.abs(rx - px), Math.abs(rz - pz));
			if (d > KEEP_VOXELS) {
				for (int ry = c.minRy; ry <= c.maxRy; ry++) {
					long key = regionKey(rx, ry, rz);
					VOXELS.remove(key);
					dropTris(key);
					dropped++;
				}
				it.remove();
			} else if (d > KEEP_TRIS && !c.trisDropped) {
				for (int ry = c.minRy; ry <= c.maxRy; ry++) {
					dropTris(regionKey(rx, ry, rz));
					trisDropped++;
				}
				c.trisDropped = true;
			}
		}
		if (now - lastStats >= STATS_EVERY_NANOS && (dropped > 0 || trisDropped > 0)) {
			lastStats = now;
			LibertyCraft.LOG.info("[LibertyCraft] collision: {} regions held ({} with triangles) in {} columns; dropped far from the player so far: {} regions, "
				+ "the triangles of {} more ({} columns forgotten by GTA IV)", VOXELS.size(), TRIS.size(), COLUMNS.size(), dropped, trisDropped, forgets);
		}
	}

	private static void dropTris(long key) {
		TRIS.remove(key);
		GHOSTS.remove(key);
		TRI_HASH.remove(key);
	}

	public static int triangleCount() {
		int n = 0;
		for (HostTri[] t : TRIS.values()) {
			n += t.length;
		}
		return n;
	}

	/** The shared cell of a block's sub-voxel mask at {@code bitsOff}, or null if it is empty. Consumer thread. */
	private static @Nullable Cell cell(MemorySegment s, long bitsOff) {
		long l0 = s.get(JAVA_LONG, bitsOff), l1 = s.get(JAVA_LONG, bitsOff + 8), l2 = s.get(JAVA_LONG, bitsOff + 16), l3 = s.get(JAVA_LONG, bitsOff + 24);
		long l4 = s.get(JAVA_LONG, bitsOff + 32), l5 = s.get(JAVA_LONG, bitsOff + 40), l6 = s.get(JAVA_LONG, bitsOff + 48), l7 = s.get(JAVA_LONG, bitsOff + 56);
		if ((l0 | l1 | l2 | l3 | l4 | l5 | l6 | l7) == 0) {
			return null;
		}
		Mask mask = new Mask(l0, l1, l2, l3, l4, l5, l6, l7);
		Cell c = INTERNED.get(mask);
		if (c == null) {
			if (INTERNED.size() >= MAX_INTERNED) {
				INTERNED.clear(); // what's held keeps its cells; new blocks just stop sharing with them
			}
			long[] layers = { l0, l1, l2, l3, l4, l5, l6, l7 };
			c = new Cell(buildShape(layers), fillInfo(layers));
			INTERNED.put(mask, c);
		}
		return c;
	}

	private static int fillInfo(long[] layers) {
		int count = 0;
		int info = 0;
		int top = 0;
		for (int y = 0; y < 8; y++) {
			long layer = layers[y];
			count += Long.bitCount(layer);
			if (layer != 0) {
				info |= y < 4 ? FILL_LOWER : FILL_UPPER;
				top = y;
			}
		}
		return info | count | top << FILL_TOP_SHIFT;
	}

	/** The shape of a non-empty mask. */
	private static VoxelShape buildShape(long[] layers) {
		boolean full = true;
		for (int y = 0; y < 8; y++) {
			full &= layers[y] == -1L;
		}
		if (full) {
			return Shapes.block();
		}
		BitSetDiscreteVoxelShape discrete = new BitSetDiscreteVoxelShape(8, 8, 8);
		for (int y = 0; y < 8; y++) {
			long layer = layers[y];
			while (layer != 0) {
				int bit = Long.numberOfTrailingZeros(layer);
				layer &= layer - 1;
				discrete.fill(bit & 7, y, bit >>> 3);
			}
		}
		return new CubeVoxelShape(discrete);
	}
}
