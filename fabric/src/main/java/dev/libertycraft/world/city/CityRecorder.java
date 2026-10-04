package dev.libertycraft.world.city;

import static dev.libertycraft.link.Proto.COL_BLOCK_BYTES;
import static dev.libertycraft.link.Proto.COL_REGION_HEADER_BYTES;
import static dev.libertycraft.link.Proto.COL_TRI_BYTES;
import static dev.libertycraft.link.Proto.TRI_GTA_MATERIAL;
import static dev.libertycraft.link.Proto.TRI_GTA_MATERIAL_SHIFT;
import static dev.libertycraft.link.Proto.TRI_TERRAIN;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import dev.libertycraft.LibertyCraft;
import java.lang.foreign.MemorySegment;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.jspecify.annotations.Nullable;

/**
 * Hands what GTA IV streams to Minecraft (HostCollision's consumer thread: kColTris, then kColRegion
 * of the same region; the water grid from the render thread) to the city store. The triangles bring
 * each block its materials (CityCells.Votes), the region its voxels.
 */
public final class CityRecorder {
	private static final ScheduledExecutorService WORKER = Executors.newSingleThreadScheduledExecutor(r -> {
		Thread t = new Thread(r, "LibertyCraft city store");
		t.setDaemon(true);
		return t;
	});

	// The last kColTris' region and its votes (consumer thread only).
	private static int votesX = Integer.MIN_VALUE, votesY, votesZ;
	private static CityCells.@Nullable Votes votes;
	private static float @Nullable [] lastWater;
	private static int lastWaterX, lastWaterZ;
	private static boolean loggedFirst;

	private CityRecorder() {
	}

	static ScheduledExecutorService worker() {
		return WORKER;
	}

	/** A kColTris message (payload at {@code p}): remember its triangles' materials for its region's kColRegion. */
	public static void tris(MemorySegment s, long p) {
		if (CityStore.current() == null) {
			return;
		}
		int minX = s.get(JAVA_INT, p), minY = s.get(JAVA_INT, p + 4), minZ = s.get(JAVA_INT, p + 8);
		int count = s.get(JAVA_INT, p + 28);
		CityCells.Votes v = new CityCells.Votes(minX, minY, minZ);
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_TRI_BYTES) {
			double ax = s.get(JAVA_FLOAT, e), ay = s.get(JAVA_FLOAT, e + 4), az = s.get(JAVA_FLOAT, e + 8);
			double bx = s.get(JAVA_FLOAT, e + 12), by = s.get(JAVA_FLOAT, e + 16), bz = s.get(JAVA_FLOAT, e + 20);
			double cx = s.get(JAVA_FLOAT, e + 24), cy = s.get(JAVA_FLOAT, e + 28), cz = s.get(JAVA_FLOAT, e + 32);
			int flags = s.get(JAVA_INT, e + 36);
			double ux = bx - ax, uy = by - ay, uz = bz - az, wx = cx - ax, wy = cy - ay, wz = cz - az;
			double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
			double len = Math.sqrt(nx * nx + ny * ny + nz * nz);
			if (!(len > 1e-9)) {
				continue;
			}
			double up = ny / len;
			v.triangle(ax, ay, az, bx, by, bz, cx, cy, cz, up, materialOf(flags, up));
		}
		votes = v;
		votesX = minX;
		votesY = minY;
		votesZ = minZ;
	}

	/** The material code (CityCells.MAT_*) a triangle brings: GTA IV's own when the host sent it. */
	static int materialOf(int flags, double up) {
		if ((flags & TRI_GTA_MATERIAL) != 0) {
			int mat = (flags >>> TRI_GTA_MATERIAL_SHIFT) & 0xFF;
			if (mat < CityCells.MAT_GTA_LIMIT) {
				return mat;
			}
		}
		if (up >= 0.7) {
			return (flags & TRI_TERRAIN) != 0 ? CityCells.MAT_TERRAIN : CityCells.MAT_FLOOR;
		}
		return up <= -0.7 ? CityCells.MAT_CEILING : CityCells.MAT_WALL;
	}

	/** A kColRegion message (payload at {@code p}): its blocks, with the materials of its triangles, into the store. */
	public static void region(MemorySegment s, long p) {
		CityStore store = CityStore.current();
		if (store == null) {
			return;
		}
		int minX = s.get(JAVA_INT, p), minY = s.get(JAVA_INT, p + 4), minZ = s.get(JAVA_INT, p + 8);
		int maxX = s.get(JAVA_INT, p + 12), maxY = s.get(JAVA_INT, p + 16), maxZ = s.get(JAVA_INT, p + 20);
		int count = s.get(JAVA_INT, p + 28);
		if (maxX - minX != 7 || maxY - minY != 7 || maxZ - minZ != 7 || (minX & 7) != 0 || (minY & 7) != 0 || (minZ & 7) != 0) {
			return; // not one 8x8x8 region (the host only sends those)
		}
		long[] cells = new long[512];
		long[] layers = new long[8];
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_BLOCK_BYTES) {
			int x = s.get(JAVA_INT, e) - minX, y = s.get(JAVA_INT, e + 4) - minY, z = s.get(JAVA_INT, e + 8) - minZ;
			if (x < 0 || x > 7 || y < 0 || y > 7 || z < 0 || z > 7) {
				continue;
			}
			for (int k = 0; k < 8; k++) {
				layers[k] = s.get(JAVA_LONG, e + 16 + k * 8L);
			}
			cells[x + z * 8 + y * 64] = CityCells.describe(layers);
		}
		CityCells.Votes v = votes != null && votesX == minX && votesY == minY && votesZ == minZ ? votes : null;
		votes = null;
		if (v != null) {
			for (int i = 0; i < 512; i++) {
				int floor = v.best(i, 0), wall = v.best(i, 1);
				if (floor != CityCells.MAT_NONE || wall != CityCells.MAT_NONE) {
					cells[i] = CityCells.withMaterials(cells[i], floor, wall);
				}
			}
		}
		store.putRegion(Math.floorDiv(minX, 8), Math.floorDiv(minY, 8), Math.floorDiv(minZ, 8), cells);
		if (!loggedFirst) {
			loggedFirst = true;
			LibertyCraft.LOG.info("[LibertyCraft] blocky city: storing GTA IV's regions as they arrive (first at {} {} {}, {} blocks, materials {})", minX, minY, minZ,
				count, v != null ? "from its triangles" : "none");
		}
	}

	/** GTA IV's water grid (render thread, every frame): stored when it changed, off the render thread. */
	public static void water(int x0, int z0, int size, float[] surface) {
		if (CityStore.current() == null) {
			return;
		}
		if (lastWater != null && x0 == lastWaterX && z0 == lastWaterZ && java.util.Arrays.equals(lastWater, surface)) {
			return;
		}
		float[] copy = surface.clone();
		lastWater = copy;
		lastWaterX = x0;
		lastWaterZ = z0;
		WORKER.execute(() -> {
			CityStore store = CityStore.current();
			if (store != null) {
				store.putWater(x0, z0, size, copy);
			}
		});
	}

	/** Writes the store's changes every few seconds (and drops idle tiles). */
	static void startFlushing() {
		WORKER.scheduleWithFixedDelay(() -> {
			try {
				CityStore store = CityStore.current();
				if (store != null) {
					store.flush(false);
				}
			} catch (RuntimeException e) {
				LibertyCraft.LOG.warn("[LibertyCraft] blocky city: flush failed", e);
			}
		}, 10, 10, TimeUnit.SECONDS);
	}
}
