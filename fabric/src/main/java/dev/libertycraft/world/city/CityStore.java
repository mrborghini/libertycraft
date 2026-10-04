package dev.libertycraft.world.city;

import dev.libertycraft.LibertyCraft;
import it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.Deflater;
import java.util.zip.DeflaterOutputStream;
import java.util.zip.InflaterInputStream;
import org.jspecify.annotations.Nullable;

/**
 * Every bit of Liberty City GTA IV has described to Minecraft, kept in the world save
 * ({@code <world>/libertycraft_city/}) for the blocky copy of the city (CityChunkGenerator).
 *
 * <p>GTA IV streams its collision around the player as 8x8x8-block regions (kColRegion, with the
 * kColTris triangles of the same region just before); each one received is stored as the block
 * descriptors of CityCells, replacing what was stored for that region before (newer data wins). The
 * water surface (WaterGrid) is kept per block column. Regions are grouped into tiles of 16x16 region
 * columns (128x128 blocks), one deflate-compressed file each, written in the background a few seconds
 * after they change. A region costs a 64-byte mask plus 8 bytes per block that holds geometry
 * (typically 1 to 3 KiB before compression); only tiles near where the city is being built or
 * explored stay in memory.
 *
 * <p>Every stored region bumps a revision counter; each region column remembers the last one, and
 * each city chunk the one it was built from, so chunks built before newer data arrived are rebuilt.
 */
public final class CityStore {
	public static final int TILE_REGIONS = 16;               // region columns per tile edge
	public static final int TILE_BLOCKS = TILE_REGIONS * 8;  // 128
	private static final int FILE_MAGIC = 0x4C435431;        // "LCT1"
	private static final int INDEX_MAGIC = 0x4C434931;       // "LCI1"
	private static final int MAX_TILES_IN_MEMORY = 48;
	private static final short NO_WATER = Short.MIN_VALUE;

	private static volatile @Nullable CityStore current;

	private final Path dir;
	private final ConcurrentHashMap<Long, Tile> tiles = new ConcurrentHashMap<>();
	private final AtomicLong revision = new AtomicLong();
	private final Long2LongOpenHashMap chunkApplied = new Long2LongOpenHashMap(); // guarded by itself
	private volatile boolean indexDirty;
	private final Object flushLock = new Object();
	private long regionsStored, waterColumnsStored;

	private static final class Region {
		long rev;
		byte[] packed; // CityStore.pack
	}

	private static final class Tile {
		final int tx, tz;
		final Map<Integer, Region> regions = new HashMap<>(); // key: ry << 8 | lrz << 4 | lrx
		final long[] columnRev = new long[TILE_REGIONS * TILE_REGIONS];
		final int[] columnMinRy = new int[TILE_REGIONS * TILE_REGIONS];
		final int[] columnMaxRy = new int[TILE_REGIONS * TILE_REGIONS];
		short @Nullable [] water; // [lz * 128 + lx]: MC y * 16, NO_WATER: none
		boolean dirty;
		volatile long lastUsed;

		Tile(int tx, int tz) {
			this.tx = tx;
			this.tz = tz;
			java.util.Arrays.fill(this.columnMinRy, Integer.MAX_VALUE);
			java.util.Arrays.fill(this.columnMaxRy, Integer.MIN_VALUE);
		}
	}

	private CityStore(Path dir) {
		this.dir = dir;
	}

	/** The store of the world being played (null without an integrated server). */
	public static @Nullable CityStore current() {
		return current;
	}

	public static synchronized CityStore open(Path dir) {
		close();
		CityStore store = new CityStore(dir);
		try {
			Files.createDirectories(dir);
			store.readIndex();
		} catch (IOException e) {
			LibertyCraft.LOG.warn("[LibertyCraft] blocky city: couldn't open {}: {}", dir, e.toString());
		}
		current = store;
		LibertyCraft.LOG.info("[LibertyCraft] blocky city store at {} (revision {}, {} chunks built)", dir, store.revision.get(), store.chunkApplied.size());
		return store;
	}

	public static synchronized void close() {
		CityStore store = current;
		current = null;
		if (store != null) {
			store.flush(true);
		}
	}

	// ---- writes (the collision consumer thread) ---------------------------------------------------

	/** One region's block descriptors (CityCells, 512 of them, x + 8z + 64y; 0 = nothing there). */
	public void putRegion(int rx, int ry, int rz, long[] cells) {
		byte[] packed = pack(cells);
		Tile tile = tile(Math.floorDiv(rx, TILE_REGIONS), Math.floorDiv(rz, TILE_REGIONS));
		long rev = this.revision.incrementAndGet();
		int lrx = Math.floorMod(rx, TILE_REGIONS), lrz = Math.floorMod(rz, TILE_REGIONS);
		int col = lrz * TILE_REGIONS + lrx;
		synchronized (tile) {
			Region region = tile.regions.get(regionKey(lrx, ry, lrz));
			if (region != null && java.util.Arrays.equals(region.packed, packed)) {
				return; // nothing new (a re-probe of an unchanged region)
			}
			if (region == null) {
				region = new Region();
				tile.regions.put(regionKey(lrx, ry, lrz), region);
			}
			region.rev = rev;
			region.packed = packed;
			tile.columnRev[col] = rev;
			tile.columnMinRy[col] = Math.min(tile.columnMinRy[col], ry);
			tile.columnMaxRy[col] = Math.max(tile.columnMaxRy[col], ry);
			tile.dirty = true;
			this.regionsStored++;
		}
		this.indexDirty = true;
	}

	/** GTA IV's water surface (MC y, NaN: none) over {@code size x size} columns from (x0, z0). */
	public void putWater(int x0, int z0, int size, float[] surface) {
		for (int dz = 0; dz < size; dz++) {
			for (int dx = 0; dx < size; dx++) {
				float s = surface[dz * size + dx];
				int x = x0 + dx, z = z0 + dz;
				Tile tile = tile(Math.floorDiv(x, TILE_BLOCKS), Math.floorDiv(z, TILE_BLOCKS));
				short value = Float.isNaN(s) || s < -2000.0F || s > 2000.0F ? NO_WATER : (short) Math.round(s * 16.0F);
				synchronized (tile) {
					int i = Math.floorMod(z, TILE_BLOCKS) * TILE_BLOCKS + Math.floorMod(x, TILE_BLOCKS);
					if (tile.water == null) {
						if (value == NO_WATER) {
							continue;
						}
						tile.water = new short[TILE_BLOCKS * TILE_BLOCKS];
						java.util.Arrays.fill(tile.water, NO_WATER);
					}
					if (Math.abs(tile.water[i] - value) <= 1 && (tile.water[i] == NO_WATER) == (value == NO_WATER)) {
						continue;
					}
					tile.water[i] = value;
					int col = Math.floorMod(Math.floorDiv(z, 8), TILE_REGIONS) * TILE_REGIONS + Math.floorMod(Math.floorDiv(x, 8), TILE_REGIONS);
					tile.columnRev[col] = this.revision.incrementAndGet();
					tile.dirty = true;
					this.waterColumnsStored++;
				}
				this.indexDirty = true;
			}
		}
	}

	// ---- reads (worldgen threads, the server thread) -----------------------------------------------

	/** The stored region's 512 descriptors into {@code out}; false (out untouched) if it was never described. */
	public boolean region(int rx, int ry, int rz, long[] out) {
		Tile tile = tile(Math.floorDiv(rx, TILE_REGIONS), Math.floorDiv(rz, TILE_REGIONS));
		byte[] packed;
		synchronized (tile) {
			Region region = tile.regions.get(regionKey(Math.floorMod(rx, TILE_REGIONS), ry, Math.floorMod(rz, TILE_REGIONS)));
			if (region == null) {
				return false;
			}
			packed = region.packed;
		}
		unpack(packed, out);
		return true;
	}

	/** The region rows (ry) stored for the region column (rx, rz): {min, max}, or null if none. */
	public int @Nullable [] columnRange(int rx, int rz) {
		Tile tile = tile(Math.floorDiv(rx, TILE_REGIONS), Math.floorDiv(rz, TILE_REGIONS));
		int col = Math.floorMod(rz, TILE_REGIONS) * TILE_REGIONS + Math.floorMod(rx, TILE_REGIONS);
		synchronized (tile) {
			return tile.columnMinRy[col] > tile.columnMaxRy[col] ? null : new int[] { tile.columnMinRy[col], tile.columnMaxRy[col] };
		}
	}

	/** GTA IV's water surface (MC y) over this block column, or NaN. */
	public double waterAt(int x, int z) {
		Tile tile = tile(Math.floorDiv(x, TILE_BLOCKS), Math.floorDiv(z, TILE_BLOCKS));
		synchronized (tile) {
			if (tile.water == null) {
				return Double.NaN;
			}
			short v = tile.water[Math.floorMod(z, TILE_BLOCKS) * TILE_BLOCKS + Math.floorMod(x, TILE_BLOCKS)];
			return v == NO_WATER ? Double.NaN : v / 16.0;
		}
	}

	/** The newest data a city chunk depends on: its region columns and their neighbours (edges, barriers). */
	public long chunkSourceRevision(int cx, int cz) {
		long rev = 0;
		for (int rx = cx * 2 - 1; rx <= cx * 2 + 2; rx++) {
			for (int rz = cz * 2 - 1; rz <= cz * 2 + 2; rz++) {
				Tile tile = tile(Math.floorDiv(rx, TILE_REGIONS), Math.floorDiv(rz, TILE_REGIONS));
				synchronized (tile) {
					rev = Math.max(rev, tile.columnRev[Math.floorMod(rz, TILE_REGIONS) * TILE_REGIONS + Math.floorMod(rx, TILE_REGIONS)]);
				}
			}
		}
		return rev;
	}

	/** The revision this city chunk was last built from (0: never). */
	public long chunkApplied(int cx, int cz) {
		synchronized (this.chunkApplied) {
			return this.chunkApplied.get(chunkKey(cx, cz));
		}
	}

	public void setChunkApplied(int cx, int cz, long rev) {
		synchronized (this.chunkApplied) {
			this.chunkApplied.put(chunkKey(cx, cz), rev);
		}
		this.indexDirty = true;
	}

	public long revision() {
		return this.revision.get();
	}

	public String stats() {
		return String.format(java.util.Locale.ROOT, "revision %d, %d tiles in memory, %d region updates and %d water columns stored this session", this.revision.get(),
			this.tiles.size(), this.regionsStored, this.waterColumnsStored);
	}

	// ---- tiles ------------------------------------------------------------------------------------------

	private Tile tile(int tx, int tz) {
		long key = (long) tx << 32 | (tz & 0xFFFFFFFFL);
		Tile tile = this.tiles.get(key);
		if (tile == null) {
			tile = this.tiles.computeIfAbsent(key, k -> readTile(tx, tz));
		}
		tile.lastUsed = System.nanoTime();
		return tile;
	}

	private static int regionKey(int lrx, int ry, int lrz) {
		return ry << 8 | lrz << 4 | lrx;
	}

	private static long chunkKey(int cx, int cz) {
		return (long) cx << 32 | (cz & 0xFFFFFFFFL);
	}

	private Path tilePath(int tx, int tz) {
		return this.dir.resolve("t." + tx + "." + tz + ".bin");
	}

	private Tile readTile(int tx, int tz) {
		Tile tile = new Tile(tx, tz);
		Path path = tilePath(tx, tz);
		if (!Files.isRegularFile(path)) {
			return tile;
		}
		try (InputStream raw = Files.newInputStream(path); DataInputStream in = new DataInputStream(new InflaterInputStream(raw))) {
			if (in.readInt() != FILE_MAGIC) {
				throw new IOException("not a city tile");
			}
			int count = in.readInt();
			for (int i = 0; i < count; i++) {
				int key = in.readInt();
				Region region = new Region();
				region.rev = in.readLong();
				region.packed = new byte[in.readInt()];
				in.readFully(region.packed);
				tile.regions.put(key, region);
				int ry = key >> 8, col = (key >> 4 & 0xF) * TILE_REGIONS + (key & 0xF);
				tile.columnRev[col] = Math.max(tile.columnRev[col], region.rev);
				tile.columnMinRy[col] = Math.min(tile.columnMinRy[col], ry);
				tile.columnMaxRy[col] = Math.max(tile.columnMaxRy[col], ry);
			}
			if (in.readBoolean()) {
				tile.water = new short[TILE_BLOCKS * TILE_BLOCKS];
				for (int i = 0; i < tile.water.length; i++) {
					tile.water[i] = in.readShort();
				}
				for (int col = 0; col < tile.columnRev.length; col++) {
					tile.columnRev[col] = Math.max(tile.columnRev[col], in.readLong());
				}
			}
		} catch (IOException e) {
			LibertyCraft.LOG.warn("[LibertyCraft] blocky city: couldn't read {} ({}); starting that tile over", path.getFileName(), e.toString());
			return new Tile(tx, tz);
		}
		return tile;
	}

	private void writeTile(Tile tile) throws IOException {
		ByteArrayOutputStream bytes = new ByteArrayOutputStream(64 << 10);
		synchronized (tile) {
			Deflater deflater = new Deflater(6);
			try (DataOutputStream out = new DataOutputStream(new DeflaterOutputStream(bytes, deflater, 1 << 15))) {
				out.writeInt(FILE_MAGIC);
				out.writeInt(tile.regions.size());
				for (Map.Entry<Integer, Region> e : tile.regions.entrySet()) {
					out.writeInt(e.getKey());
					out.writeLong(e.getValue().rev);
					out.writeInt(e.getValue().packed.length);
					out.write(e.getValue().packed);
				}
				out.writeBoolean(tile.water != null);
				if (tile.water != null) {
					for (short s : tile.water) {
						out.writeShort(s);
					}
					for (long rev : tile.columnRev) {
						out.writeLong(rev); // the water's revisions too (regions carry their own)
					}
				}
			} finally {
				deflater.end();
			}
			tile.dirty = false;
		}
		writeAtomically(tilePath(tile.tx, tile.tz), bytes.toByteArray());
	}

	private void readIndex() throws IOException {
		Path path = this.dir.resolve("index.bin");
		if (!Files.isRegularFile(path)) {
			return;
		}
		try (DataInputStream in = new DataInputStream(new InflaterInputStream(Files.newInputStream(path)))) {
			if (in.readInt() != INDEX_MAGIC) {
				throw new IOException("not a city index");
			}
			this.revision.set(in.readLong());
			int chunks = in.readInt();
			for (int i = 0; i < chunks; i++) {
				this.chunkApplied.put(in.readLong(), in.readLong());
			}
		}
	}

	private void writeIndex() throws IOException {
		ByteArrayOutputStream bytes = new ByteArrayOutputStream();
		try (DataOutputStream out = new DataOutputStream(new DeflaterOutputStream(bytes))) {
			out.writeInt(INDEX_MAGIC);
			out.writeLong(this.revision.get());
			synchronized (this.chunkApplied) {
				out.writeInt(this.chunkApplied.size());
				for (var e : this.chunkApplied.long2LongEntrySet()) {
					out.writeLong(e.getLongKey());
					out.writeLong(e.getLongValue());
				}
			}
		}
		writeAtomically(this.dir.resolve("index.bin"), bytes.toByteArray());
	}

	private static void writeAtomically(Path path, byte[] data) throws IOException {
		Path tmp = path.resolveSibling(path.getFileName() + ".tmp");
		Files.write(tmp, data);
		try {
			Files.move(tmp, path, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
		} catch (AtomicMoveNotSupportedException e) {
			Files.move(tmp, path, StandardCopyOption.REPLACE_EXISTING);
		}
	}

	/**
	 * Writes the changed tiles and the index; drops tiles not used for a while beyond the in-memory
	 * budget. {@code all}: also called on shutdown (everything written, the log says so).
	 */
	public void flush(boolean all) {
		synchronized (this.flushLock) {
			int written = 0;
			long bytes = 0;
			List<Tile> dirty = new ArrayList<>();
			for (Tile tile : this.tiles.values()) {
				if (tile.dirty) {
					dirty.add(tile);
				}
			}
			for (Tile tile : dirty) {
				try {
					writeTile(tile);
					written++;
					bytes += Files.size(tilePath(tile.tx, tile.tz));
				} catch (IOException e) {
					LibertyCraft.LOG.warn("[LibertyCraft] blocky city: couldn't write tile {} {}: {}", tile.tx, tile.tz, e.toString());
				}
			}
			if (this.indexDirty || all) {
				this.indexDirty = false;
				try {
					writeIndex();
				} catch (IOException e) {
					LibertyCraft.LOG.warn("[LibertyCraft] blocky city: couldn't write the index: {}", e.toString());
				}
			}
			if (this.tiles.size() > MAX_TILES_IN_MEMORY) {
				List<Tile> byAge = new ArrayList<>(this.tiles.values());
				byAge.sort(java.util.Comparator.comparingLong(t -> t.lastUsed));
				long idle = System.nanoTime() - 60_000_000_000L; // only tiles nobody touched for a minute (no writer holds them)
				for (int i = 0; i < byAge.size() - MAX_TILES_IN_MEMORY; i++) {
					Tile t = byAge.get(i);
					synchronized (t) {
						if (!t.dirty && t.lastUsed - idle < 0) {
							this.tiles.remove((long) t.tx << 32 | (t.tz & 0xFFFFFFFFL));
						}
					}
				}
			}
			if (written > 0 || all) {
				LibertyCraft.LOG.info("[LibertyCraft] blocky city: {} tile(s) written ({} KiB){}; {}", written, bytes >> 10, all ? " on close" : "", stats());
			}
		}
	}

	// ---- packing: a 64-byte mask of the blocks that hold something, then their descriptors ---------------

	static byte[] pack(long[] cells) {
		int count = 0;
		for (long c : cells) {
			if (c != 0) {
				count++;
			}
		}
		byte[] out = new byte[64 + count * 8];
		int p = 64;
		for (int i = 0; i < 512; i++) {
			long c = cells[i];
			if (c == 0) {
				continue;
			}
			out[i >> 3] |= (byte) (1 << (i & 7));
			for (int b = 0; b < 8; b++) {
				out[p++] = (byte) (c >>> (b * 8));
			}
		}
		return out;
	}

	static void unpack(byte[] packed, long[] out) {
		int p = 64;
		for (int i = 0; i < 512; i++) {
			if ((packed[i >> 3] & (1 << (i & 7))) == 0) {
				out[i] = 0;
				continue;
			}
			long c = 0;
			for (int b = 0; b < 8; b++) {
				c |= (packed[p++] & 0xFFL) << (b * 8);
			}
			out[i] = c;
		}
	}
}
