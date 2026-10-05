package dev.libertycraft.world;

import dev.libertycraft.world.city.BlockyCity;
import java.util.HashMap;
import java.util.Map;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.Vec3;

/**
 * GTA IV's roofs, ceilings, balconies, bridges and el-train tracks keep the sun and the rain off in the
 * mirror world. Minecraft decides both by whether a spot can see the sky, and the mirror world is a void:
 * GTA IV's geometry is no blocks, so everything saw it (undead burned in Roman's apartment). A spot is
 * covered here when the player is in one of GTA IV's interiors (SkyState's worldId), or when a ray from
 * near the top of its block straight up meets GTA IV's collision within {@link #REACH} blocks (the triangles GTA IV has sent for
 * that column; walls beside it don't count). Used for the undead's sun burn and their search for shade
 * (Mob, FleeSunGoal, GroundPathNavigation's sun avoidance: SunCoverMixin) and for where it rains
 * (LevelRainMixin: rain doesn't reach a covered spot, so it neither wets a mob nor puts out a fire there).
 * Answers are kept per block for a few seconds, and only asked for where Minecraft asks.
 */
public final class HostCover {
	/** How far above a spot GTA IV's geometry still covers it (blocks). */
	public static final double REACH = 48.0;
	private static final long KEEP_TICKS = 100;
	private static final Map<Long, Cover> CACHE = new HashMap<>();
	private static long cleared;

	private HostCover() {
	}

	/** Covered spots are only in the mirror world, while GTA IV describes it (server side). */
	public static boolean applies(Level level) {
		return !level.isClientSide() && BlockyCity.isMirror(level) && HostCollision.active();
	}

	/** Minecraft's own answer ({@code sees}: the spot sees the sky) with GTA IV's cover taken into account. */
	public static boolean seesSky(Level level, BlockPos pos, boolean sees) {
		return sees && !(applies(level) && covered(level, pos));
	}

	/** The same for a spot to hide in: it must be shade one can stand in ({@link #shade}). */
	public static boolean seesSkyForShade(Level level, BlockPos pos, boolean sees) {
		return sees && !(applies(level) && shade(level, pos));
	}

	/** GTA IV covers {@code pos} (see the class comment). Server thread. */
	public static boolean covered(Level level, BlockPos pos) {
		return HostSky.indoors() || coverAbove(level, pos) < Double.POSITIVE_INFINITY;
	}

	/**
	 * Shade a mob can go to (FleeSunGoal's hiding spot): covered, but not inside GTA IV's geometry or under
	 * the ground (where the ray up meets the ground itself), with room to stand under what covers it.
	 */
	public static boolean shade(Level level, BlockPos pos) {
		if (HostPath.solid(pos)) {
			return false;
		}
		if (HostSky.indoors()) {
			return true;
		}
		return coverAbove(level, pos) - pos.getY() >= 2.0;
	}

	/** The height of the first of GTA IV's surfaces straight above {@code pos} (within REACH), or +infinity. */
	static double coverAbove(Level level, BlockPos pos) {
		long now = level.getGameTime();
		if (now - cleared > 600 || CACHE.size() > 8192) {
			CACHE.clear();
			cleared = now;
		}
		long key = pos.asLong();
		Cover cached = CACHE.get(key);
		if (cached != null && now - cached.time() < KEEP_TICKS) {
			return cached.above();
		}
		// From near the top of the cell: the ground a spot's own cell holds (a street surface at 14.5 in cell 14,
		// where a mob's feet are) is no cover.
		Vec3 from = new Vec3(pos.getX() + 0.5, pos.getY() + 0.9, pos.getZ() + 0.5);
		HostRay.Hit hit = HostClip.cast(from, from.add(0.0, REACH, 0.0));
		double above = hit != null ? hit.y() : Double.POSITIVE_INFINITY;
		CACHE.put(key, new Cover(now, above));
		return above;
	}

	private record Cover(long time, double above) {
	}
}
