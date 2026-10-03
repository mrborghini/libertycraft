package dev.libertycraft.world;

import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * Makes Minecraft ray casts (arrows and other projectiles, the crosshair pick) hit GTA IV's exact
 * collision triangles. Vanilla still clips against real Minecraft blocks; whichever is nearer wins.
 */
public final class HostClip {
	private HostClip() {
	}

	public enum Use {
		/** A projectile: the hit cell is the one the surface is in (it sticks there). */
		PROJECTILE,
		/** The player's crosshair: the hit cell is where a block placed against the surface goes. */
		PICK
	}

	private static final ThreadLocal<List<HostTri>> SCRATCH = ThreadLocal.withInitial(ArrayList::new);

	public static BlockHitResult refine(Vec3 from, Vec3 to, BlockHitResult vanilla, Use use) {
		HostRay.Hit hit = cast(from, to);
		if (use == Use.PICK) {
			// The walls of a dug hole (GTA IV ground the crosshair meets from inside the hole).
			double limit = hit != null ? hit.t() : 1.0;
			if (vanilla.getType() != HitResult.Type.MISS) {
				limit = Math.min(limit, Math.sqrt(from.distanceToSqr(vanilla.getLocation()) / Math.max(from.distanceToSqr(to), 1e-9)));
			}
			HostRay.Hit wall = digWall(from, to, limit);
			if (wall != null) {
				hit = wall;
				vanilla = BlockHitResult.miss(to, Direction.UP, BlockPos.containing(to));
			}
		}
		if (hit == null) {
			return vanilla;
		}
		Vec3 location = new Vec3(hit.x(), hit.y(), hit.z());
		if (vanilla.getType() != HitResult.Type.MISS && from.distanceToSqr(vanilla.getLocation()) <= from.distanceToSqr(location)) {
			return vanilla;
		}
		Direction face = Direction.values()[HostRay.dominantFace(hit.nx(), hit.ny(), hit.nz())];
		int[] cell = use == Use.PICK ? HostRay.placementCell(hit) : HostRay.surfaceCell(hit);
		return new HostHitResult(location, face, new BlockPos(cell[0], cell[1], cell[2]), hit);
	}

	/**
	 * The crosshair's hit on GTA IV geometry (Use.PICK) names the cell a block placed there goes into,
	 * which is what block items need. Items that act on the cell in front of the face they're used on
	 * (flint and steel, fire charges: {@code clickedPos.relative(face)}) need the cell behind it
	 * instead, so their fire lands in that same cell, on the ground (BlockStateBaseMixin makes GTA IV
	 * ground hold it up). Other hits and items are returned as they are.
	 */
	public static BlockHitResult forItem(net.minecraft.world.item.ItemStack stack, BlockHitResult hit) {
		if (!(hit instanceof HostHitResult) || !(stack.getItem() instanceof net.minecraft.world.item.FlintAndSteelItem
			|| stack.getItem() instanceof net.minecraft.world.item.FireChargeItem)) {
			return hit;
		}
		return behindFace(hit, true);
	}

	/**
	 * A hit on GTA IV geometry moved back one cell from where a block placed against it would go, so
	 * that {@code getBlockPos().relative(getDirection())} is that cell (where buckets pour and fire
	 * starters light). With {@code clampLocation} the location is moved inside the new cell, for a
	 * hit that goes to the server in a "use item on" packet (it only accepts a location within a
	 * block of the cell it names); otherwise it stays the exact point on the surface (a boat is
	 * put there).
	 */
	public static BlockHitResult behindFace(BlockHitResult hit, boolean clampLocation) {
		BlockPos pos = hit.getBlockPos().relative(hit.getDirection().getOpposite());
		Vec3 at = hit.getLocation();
		if (!clampLocation) {
			return new BlockHitResult(at, hit.getDirection(), pos, false);
		}
		Vec3 inside = new Vec3(
			Math.clamp(at.x, pos.getX(), pos.getX() + 1.0), Math.clamp(at.y, pos.getY(), pos.getY() + 1.0), Math.clamp(at.z, pos.getZ(), pos.getZ() + 1.0)
		);
		return new BlockHitResult(inside, hit.getDirection(), pos, false);
	}

	private static final HostTri STONE_WALL = new HostTri(new float[9], 0, dev.libertycraft.link.Proto.TRI_DIGGABLE | (dev.libertycraft.link.Proto.DIG_STONE << dev.libertycraft.link.Proto.TRI_MATERIAL_SHIFT));

	/**
	 * Where the segment, having gone through dug cells, first enters an undug cell inside GTA IV's
	 * geometry (the wall of a hole, drawn by the client's DigWalls), before segment parameter
	 * {@code limit}; or null.
	 */
	static HostRay.Hit digWall(Vec3 from, Vec3 to, double limit) {
		HostDig.DugLookup dug = HostDig.clientDug;
		if (dug == null) {
			return null;
		}
		double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
		int x = (int) Math.floor(from.x), y = (int) Math.floor(from.y), z = (int) Math.floor(from.z);
		int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1, stepZ = dz > 0 ? 1 : -1;
		double tDeltaX = dx == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dx);
		double tDeltaY = dy == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dy);
		double tDeltaZ = dz == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dz);
		double tMaxX = dx == 0 ? Double.POSITIVE_INFINITY : ((dx > 0 ? x + 1 - from.x : from.x - x) * tDeltaX);
		double tMaxY = dy == 0 ? Double.POSITIVE_INFINITY : ((dy > 0 ? y + 1 - from.y : from.y - y) * tDeltaY);
		double tMaxZ = dz == 0 ? Double.POSITIVE_INFINITY : ((dz > 0 ? z + 1 - from.z : from.z - z) * tDeltaZ);
		boolean wasDug = dug.isDug(x, y, z);
		HostDig.Probe probe = null;
		for (int i = 0; i < 64; i++) {
			double t;
			double nx = 0, ny = 0, nz = 0;
			if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
				t = tMaxX;
				tMaxX += tDeltaX;
				x += stepX;
				nx = -stepX;
			} else if (tMaxY <= tMaxZ) {
				t = tMaxY;
				tMaxY += tDeltaY;
				y += stepY;
				ny = -stepY;
			} else {
				t = tMaxZ;
				tMaxZ += tDeltaZ;
				z += stepZ;
				nz = -stepZ;
			}
			if (t > limit) {
				return null;
			}
			boolean isDug = dug.isDug(x, y, z);
			if (wasDug && !isDug) {
				double px = from.x + dx * t, py = from.y + dy * t, pz = from.z + dz * t;
				if (probe == null) {
					probe = new HostDig.Probe().around(Math.min(from.x, to.x), Math.min(from.y, to.y), Math.min(from.z, to.z), Math.max(from.x, to.x), Math.max(from.y, to.y),
						Math.max(from.z, to.z));
				}
				if (probe.test(px - nx * 0.02, py - ny * 0.02, pz - nz * 0.02) > HostDig.AIR) {
					return new HostRay.Hit(t, px, py, pz, nx, ny, nz, probe.surface != null ? probe.surface : STONE_WALL);
				}
			}
			wasDug = isDug;
		}
		return null;
	}

	/** Nearest GTA IV triangle hit on the segment, or null. */
	public static HostRay.Hit cast(Vec3 from, Vec3 to) {
		List<HostTri> tris = SCRATCH.get();
		tris.clear();
		HostCollision.trianglesNear(new AABB(from, to).inflate(0.01), tris);
		if (tris.isEmpty()) {
			return null;
		}
		HostRay.Hit hit = HostRay.cast(tris, from.x, from.y, from.z, to.x, to.y, to.z);
		tris.clear();
		return hit;
	}

	/** A hit on GTA IV geometry (not a Minecraft block). Keeps the exact surface normal and triangle. */
	public static final class HostHitResult extends BlockHitResult {
		public final double nx, ny, nz;
		public final HostRay.Hit hit;

		public HostHitResult(Vec3 location, Direction direction, BlockPos pos, HostRay.Hit hit) {
			super(location, direction, pos, false);
			this.nx = hit.nx();
			this.ny = hit.ny();
			this.nz = hit.nz();
			this.hit = hit;
		}
	}
}
