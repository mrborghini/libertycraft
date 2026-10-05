package dev.libertycraft.world;

import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.level.material.Fluids;
import org.jspecify.annotations.Nullable;

/**
 * GTA IV's lakes, rivers and sea as Minecraft water: GTA IV sends the water surface over the block
 * columns around the player (see WaterGrid in the protocol), and wherever Minecraft has air below
 * that surface, entities treat it as water, so the player swims, floats, sinks slowly and drowns
 * there as in Minecraft water. Only entity physics sees it; no blocks change.
 *
 * <p>Besides the grid around the player, GTA IV sends tiles of a 5x5 lattice of 16-block tiles around
 * them in turn (Proto.WATER_GRID_TILE); they are kept for {@link #TILE_KEEP_NANOS}, so mobs up to about
 * 40 blocks from the player are in the water too. The grid around the player wins where both reach.
 */
public final class HostWater {
	private record Grid(int originX, int originZ, int size, float[] surface, long nanos) {
		double at(int x, int z) {
			int dx = x - this.originX, dz = z - this.originZ;
			if (dx < 0 || dz < 0 || dx >= this.size || dz >= this.size) {
				return Double.NaN;
			}
			float s = this.surface[dz * this.size + dx];
			return s < -1.0e20F ? Double.NaN : s;
		}
	}

	static final long TILE_KEEP_NANOS = 6_000_000_000L;
	private static volatile @Nullable Grid grid;
	private static final ConcurrentHashMap<Long, Grid> TILES = new ConcurrentHashMap<>();
	private static long lastPrune;

	private HostWater() {
	}

	/** Once a frame on the client: pick up GTA IV's latest grid (or tile). */
	public static void refresh() {
		Link.WaterGrid read = Link.readWaterGrid();
		if (read != null) {
			long now = System.nanoTime();
			Grid g = new Grid(read.originX, read.originZ, read.size, read.surface, now);
			if ((read.worldId & Proto.WATER_GRID_TILE) != 0) {
				TILES.put(tileKey(Math.floorDiv(read.originX, Proto.WATER_GRID_SIZE), Math.floorDiv(read.originZ, Proto.WATER_GRID_SIZE)), g);
			} else {
				grid = g;
			}
			dev.libertycraft.world.city.CityRecorder.water(read.originX, read.originZ, read.size, read.surface); // kept for the blocky city
			if (now - lastPrune > 1_000_000_000L) {
				lastPrune = now;
				TILES.values().removeIf(t -> now - t.nanos() > TILE_KEEP_NANOS);
			}
		}
	}

	private static long tileKey(int tx, int tz) {
		return (long) tx << 32 | tz & 0xFFFFFFFFL;
	}

	public static void clear() {
		grid = null;
		TILES.clear();
	}

	public static boolean active() {
		return grid != null;
	}

	/** Minecraft y of GTA IV's water surface over this column, or NaN where there is none. */
	public static double surfaceAt(int x, int z) {
		Grid g = grid;
		if (g == null) {
			return Double.NaN;
		}
		int dx = x - g.originX(), dz = z - g.originZ();
		if (dx >= 0 && dz >= 0 && dx < g.size() && dz < g.size()) {
			return g.at(x, z);
		}
		Grid t = TILES.isEmpty() ? null : TILES.get(tileKey(Math.floorDiv(x, Proto.WATER_GRID_SIZE), Math.floorDiv(z, Proto.WATER_GRID_SIZE)));
		return t == null ? Double.NaN : t.at(x, z);
	}

	/** How much of this block (0..1) is under GTA IV's water; 0 above the surface. */
	public static float depthIn(BlockPos pos) {
		double s = surfaceAt(pos.getX(), pos.getZ());
		if (Double.isNaN(s)) {
			return 0.0F;
		}
		double h = s - pos.getY();
		return h < 0.02 ? 0.0F : (float) Math.min(1.0, h);
	}

	/**
	 * GTA IV water in this cell that is open up to its surface: no GTA IV geometry from the cell up to
	 * the surface. GTA IV's water plane runs on under beaches and quays; a cell under their ground is
	 * below the surface but not in the water.
	 */
	public static boolean openAt(BlockPos pos) {
		double s = surfaceAt(pos.getX(), pos.getZ());
		if (Double.isNaN(s) || s - pos.getY() < 0.02) {
			return false;
		}
		BlockPos.MutableBlockPos p = pos.mutable();
		for (int y = pos.getY(), top = (int) Math.floor(s); y <= top; y++) {
			if (HostCollision.hasGeometry(p.setY(y))) {
				return false;
			}
		}
		return true;
	}

	/** True if GTA IV water reaches up into the box of block cells (inclusive). */
	public static boolean anyIn(int x0, int y0, int z0, int x1, int y1, int z1) {
		if (grid == null) {
			return false;
		}
		for (int x = x0; x <= x1; x++) {
			for (int z = z0; z <= z1; z++) {
				double s = surfaceAt(x, z);
				if (!Double.isNaN(s) && s > y0) {
					return true;
				}
			}
		}
		return false;
	}

	/** GTA IV water in an otherwise empty (air) Minecraft cell, as a Minecraft fluid; null if none. */
	public static @Nullable FluidState fluidAt(BlockGetter level, BlockPos pos) {
		if (depthIn(pos) <= 0.0F || !level.getBlockState(pos).isAir()) {
			return null;
		}
		return Fluids.WATER.getSource(false);
	}

	/** The exact water height in a cell only GTA IV fills (so floating matches its surface); -1 otherwise. */
	public static float substitutedHeight(BlockGetter level, BlockPos pos) {
		float depth = depthIn(pos);
		if (depth <= 0.0F || !level.getFluidState(pos).isEmpty() || !level.getBlockState(pos).isAir()) {
			return -1.0F;
		}
		return depth;
	}
}
