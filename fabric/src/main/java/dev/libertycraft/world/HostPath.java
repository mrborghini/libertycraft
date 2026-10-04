package dev.libertycraft.world;

import dev.libertycraft.world.city.BlockyCity;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import org.jspecify.annotations.Nullable;

/**
 * GTA IV's ground for Minecraft's pathfinder. The mirror world is a void: its streets, floors and
 * walls are GTA IV's collision (HostCollision), which mobs walk on and bump into, but which the
 * pathfinder (block states only) didn't see, so mobs found no path anywhere and only attacked what
 * stood within reach. The pathfinding mixins (PathfindingContextMixin, WalkNodeEvaluatorMixin,
 * GroundPathNavigationMixin) read the collision voxels (1/8 of a block) here as blocks: a cell GTA
 * IV's geometry reaches into 5/8 of the way up or more is solid (BLOCKED: walls, the ground under a
 * surface that sits high in its cell), a cell with ground lower in it is one to stand in (WALKABLE:
 * what vanilla makes of the cell above a slab). The 5/8 keeps a mob from hopping on slopes and
 * stairs: its next node is never more than its step height (0.6) above its feet unless the ground
 * really rises that much (GTA IV's steps arrive as a ramp of 1/8 voxel steps; the Schottler Medical
 * Center's front steps rise 1 block over 2). Only in the mirror world while GTA IV describes it; the
 * blocky city is real blocks.
 */
public final class HostPath {
	/** A cell whose geometry reaches this high (of the cell) or higher is solid. */
	public static final float SOLID_FROM = 0.625F;

	private HostPath() {
	}

	/** Mobs in this level path over GTA IV's ground. */
	public static boolean applies(@Nullable Level level) {
		return level != null && !level.isClientSide() && BlockyCity.isMirror(level) && HostCollision.active();
	}

	/** GTA IV's geometry fills this cell above its middle: the pathfinder's BLOCKED. */
	public static boolean solid(BlockPos pos) {
		return HostCollision.hasGeometry(pos) && HostCollision.groundTop(pos) >= SOLID_FROM;
	}

	/** A body can stand in this cell on GTA IV's ground (low in it, or the top of the cell below). */
	public static boolean floor(BlockPos pos) {
		return !solid(pos) && HostCollision.supportsFromBelow(pos);
	}

	/**
	 * The cell to path to for a target in air cell {@code pos}: the nearest one at or below it (up to
	 * {@code down} cells) or above it (up to {@code up}) that stands on GTA IV's ground; null if none.
	 */
	public static @Nullable BlockPos standable(BlockPos pos, int up, int down) {
		BlockPos.MutableBlockPos p = pos.mutable();
		for (int dy = 0; dy <= down; dy++) {
			p.set(pos.getX(), pos.getY() - dy, pos.getZ());
			if (standsOn(p)) {
				return p.immutable();
			}
		}
		for (int dy = 1; dy <= up; dy++) {
			p.set(pos.getX(), pos.getY() + dy, pos.getZ());
			if (standsOn(p)) {
				return p.immutable();
			}
		}
		return null;
	}

	private static boolean standsOn(BlockPos.MutableBlockPos p) {
		if (solid(p)) {
			return false;
		}
		if (floor(p)) {
			return true;
		}
		p.setY(p.getY() - 1);
		boolean below = solid(p);
		p.setY(p.getY() + 1);
		return below;
	}

	/** Implemented by PathfindingContext (PathfindingContextMixin): this search is over GTA IV's ground. */
	public interface Context {
		boolean libertycraft$gta();
	}
}
