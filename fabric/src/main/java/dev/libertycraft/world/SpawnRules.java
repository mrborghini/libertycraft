package dev.libertycraft.world;

import java.util.List;
import java.util.OptionalDouble;

/**
 * Where and what LibertyCraft's mob spawner ({@link MobSpawner}) spawns: the rules, without Minecraft's
 * classes (tested by SpawnRulesTest).
 *
 * <p>The mirror world is a void over GTA IV's collision, so Minecraft's own spawning finds no ground. The
 * spawner picks a spot 24 to 64 blocks from the player (vanilla's band) and stands a mob there on GTA
 * IV's ground ({@link #surface}): the lowest ground within 4 blocks of the player's height (the street
 * they are on, not a roof above it), with room for the mob and nothing of GTA IV's above it for 12
 * blocks (outdoors: not inside a building, under a bridge or in an interior). Hostiles come at night by GTA
 * IV's clock with vanilla's overworld weights; animals now and then by day, on grass.
 */
public final class SpawnRules {
	/** Night by GTA IV's clock: from 20:00 to 05:00, when its streets are dark. */
	public static final float NIGHT_FROM = 20.0F, NIGHT_TO = 5.0F;
	public static final double MIN_DISTANCE = 24.0, MAX_DISTANCE = 64.0;
	/** Hostiles around the player (within 128 blocks) before the spawner waits; animals likewise. */
	public static final int HOSTILE_CAP = 24, PASSIVE_CAP = 8;
	/** A spot is at most this far above or below the player's feet. */
	public static final int HEIGHT_BAND = 4;
	/** Nothing of GTA IV's this many blocks above a spot: it's outdoors. */
	public static final int OPEN_SKY = 12;

	private SpawnRules() {
	}

	/** A kind of mob: its entity id, vanilla's spawn weight, how many come together. */
	public record Kind(String id, int weight, int minPack, int maxPack) {
	}

	/** Vanilla's overworld monsters (zombie villagers, slimes and the like left out). */
	public static final List<Kind> HOSTILES = List.of(
		new Kind("minecraft:zombie", 95, 1, 3),
		new Kind("minecraft:skeleton", 100, 1, 3),
		new Kind("minecraft:creeper", 100, 1, 2),
		new Kind("minecraft:spider", 100, 1, 2),
		new Kind("minecraft:enderman", 10, 1, 2),
		new Kind("minecraft:witch", 5, 1, 1));

	/** Vanilla's farm animals. */
	public static final List<Kind> ANIMALS = List.of(
		new Kind("minecraft:sheep", 12, 2, 3),
		new Kind("minecraft:pig", 10, 2, 3),
		new Kind("minecraft:chicken", 10, 2, 3),
		new Kind("minecraft:cow", 8, 2, 3));

	/** GTA IV's hour (0 to 24) is night. */
	public static boolean isNight(float hour) {
		float h = ((hour % 24.0F) + 24.0F) % 24.0F;
		return h >= NIGHT_FROM || h < NIGHT_TO;
	}

	/** Minecraft's time of day (0 to 23999; 0 is 06:00) for GTA IV's hour. */
	public static long dayTimeOf(float hour) {
		double h = ((hour % 24.0) + 24.0) % 24.0;
		return Math.floorMod(Math.round((h - 6.0) * 1000.0), 24000L);
	}

	/**
	 * Ticks to add to Minecraft's clock ({@code ticks} in all) so its time of day matches GTA IV's hour:
	 * the shorter way round (it may go back a little, never a day), 0 within 100 ticks (Minecraft's day
	 * runs faster than GTA IV's, so it is corrected every few seconds).
	 */
	public static long clockCorrection(long ticks, float hour) {
		long d = Math.floorMod(dayTimeOf(hour) - Math.floorMod(ticks, 24000L) + 12000L, 24000L) - 12000L;
		return Math.abs(d) <= 100 ? 0 : d;
	}

	public static int totalWeight(List<Kind> kinds) {
		int total = 0;
		for (Kind k : kinds) {
			total += k.weight();
		}
		return total;
	}

	/** The kind a roll in [0, totalWeight) picks. */
	public static Kind pick(List<Kind> kinds, int roll) {
		int r = Math.floorMod(roll, totalWeight(kinds));
		for (Kind k : kinds) {
			if (r < k.weight()) {
				return k;
			}
			r -= k.weight();
		}
		return kinds.getLast();
	}

	/** How many of a kind come, for a roll (any int). */
	public static int packSize(Kind kind, int roll) {
		return kind.minPack() + Math.floorMod(roll, kind.maxPack() - kind.minPack() + 1);
	}

	/** (dx, dz) from the player is in the spawn band. */
	public static boolean inBand(double dx, double dz) {
		double d2 = dx * dx + dz * dz;
		return d2 >= MIN_DISTANCE * MIN_DISTANCE && d2 <= MAX_DISTANCE * MAX_DISTANCE;
	}

	/** What the spawner knows of GTA IV's collision (HostCollision, or a test's). */
	public interface Ground {
		/** GTA IV has described this cell's region. */
		boolean known(int x, int y, int z);

		/** Any of GTA IV's geometry in this cell. */
		boolean geometry(int x, int y, int z);

		/** How high (0 to 1 of the cell) its geometry reaches; 0 without any. */
		float top(int x, int y, int z);
	}

	/**
	 * The height a mob {@code height} blocks tall stands at in column (x, z): the lowest ground there
	 * from {@code playerY} - HEIGHT_BAND up to playerY + HEIGHT_BAND (geometry low in a cell, or the top
	 * of a cell it fills: HostPath's rule), if there is room above it for the mob and nothing above that
	 * for OPEN_SKY blocks; a column whose ground is covered (indoors, under a bridge or an awning) is
	 * no place at all, and roofs above it don't count. Empty if GTA IV hasn't described the column.
	 */
	public static OptionalDouble surface(Ground g, int x, int z, double playerY, double height) {
		int top = (int) Math.floor(playerY) + HEIGHT_BAND, bottom = (int) Math.floor(playerY) - HEIGHT_BAND;
		int room = (int) Math.ceil(height + 0.25);
		for (int y = bottom; y <= top; y++) {
			if (!g.known(x, y, z) || !g.known(x, y - 1, z)) {
				return OptionalDouble.empty();
			}
			double stand;
			if (g.geometry(x, y, z)) {
				float t = g.top(x, y, z);
				if (t >= HostPath.SOLID_FROM) {
					continue; // solid: the cell above may be the one
				}
				stand = y + t;
			} else if (g.geometry(x, y - 1, z) && g.top(x, y - 1, z) >= HostPath.SOLID_FROM) {
				stand = y - 1 + g.top(x, y - 1, z);
			} else {
				continue;
			}
			for (int k = 1; k <= room + OPEN_SKY; k++) {
				if (!g.known(x, y + k, z) || g.geometry(x, y + k, z)) {
					return OptionalDouble.empty(); // a roof over it, or no room: indoors, under something
				}
			}
			return OptionalDouble.of(stand);
		}
		return OptionalDouble.empty();
	}

	/** GTA IV's material name (materials.dat) is grass for animals. */
	public static boolean isGrass(String material) {
		return material.startsWith("GRASS") || material.equals("SHORT_GRASS") || material.equals("FLOWERS");
	}
}
