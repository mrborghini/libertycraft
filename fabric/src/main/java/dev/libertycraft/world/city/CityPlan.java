package dev.libertycraft.world.city;

/**
 * One column of the blocky city: the block descriptors stored for it (CityCells) and GTA IV's water
 * surface over it, turned into Minecraft block ids bottom to top. Pure Java (unit tested); the chunk
 * generator and the refresher (CityChunkGenerator) map the ids to block states.
 */
public final class CityPlan {
	/** Water deeper than this many blocks under the surface without ground below gets a gravel bed there. */
	public static final int MAX_WATER_DEPTH = 24;
	public static final String WATER = "minecraft:water";
	public static final String SEABED = "minecraft:gravel";

	private CityPlan() {
	}

	/**
	 * Blocks for the column {@code cells[0..n)} (bottom up, cell i at y0 + i; 0 = nothing known there).
	 * {@code water}: the MC y of GTA IV's water surface over the column, or NaN. Returns block ids, null
	 * for air. The bottom cell should be one below the lowest data (a low floor sheet rounds into it).
	 */
	public static String[] column(long[] cells, int y0, double water) {
		int n = cells.length;
		boolean[] solid = new boolean[n];
		int[] floorMat = new int[n];
		int[] wallMat = new int[n];
		for (int i = 0; i < n; i++) {
			floorMat[i] = cells[i] == 0 ? CityCells.MAT_NONE : CityCells.floorMaterial(cells[i]);
			wallMat[i] = cells[i] == 0 ? CityCells.MAT_NONE : CityCells.wallMaterial(cells[i]);
		}
		for (int i = 0; i < n; i++) {
			int cls = cells[i] == 0 ? CityCells.EMPTY : CityCells.classify(cells[i]);
			if (cls == CityCells.SOLID) {
				solid[i] = true;
			} else if (cls == CityCells.SOLID_BELOW) {
				int below = i > 0 ? i - 1 : i;
				solid[below] = true;
				if (below != i) {
					if (floorMat[below] == CityCells.MAT_NONE) {
						floorMat[below] = floorMat[i];
					}
					if (wallMat[below] == CityCells.MAT_NONE) {
						wallMat[below] = wallMat[i];
					}
				}
			}
		}
		String[] out = new String[n];
		int runTopFloor = CityCells.MAT_NONE;
		int depth = 0;
		for (int i = n - 1; i >= 0; i--) {
			if (!solid[i]) {
				continue;
			}
			boolean top = i + 1 >= n || !solid[i + 1];
			if (top) {
				runTopFloor = floorMat[i] != CityCells.MAT_NONE ? floorMat[i] : wallMat[i] != CityCells.MAT_NONE ? CityCells.MAT_NONE : CityCells.MAT_TERRAIN;
				depth = 0;
				out[i] = CityCells.blockFor(floorMat[i], wallMat[i], CityCells.Role.TOP, 0);
			} else {
				depth++;
				out[i] = CityCells.blockFor(runTopFloor, wallMat[i], CityCells.Role.INSIDE, depth);
			}
		}
		if (!Double.isNaN(water)) {
			// From the surface down to the ground (or a gravel bed MAX_WATER_DEPTH down).
			int top = (int) Math.floor(water - 0.5) - y0; // the highest block whose middle is under the surface
			int filled = 0;
			for (int i = Math.min(top, n - 1); i >= 0; i--) {
				if (solid[i]) {
					break;
				}
				if (filled++ >= MAX_WATER_DEPTH) {
					out[i] = SEABED;
					break;
				}
				out[i] = WATER;
			}
		}
		return out;
	}
}
