package dev.libertycraft.combat;

import dev.libertycraft.link.Proto;
import java.util.List;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * GTA IV's peds and vehicles are solid for Minecraft's player and mobs. Their stand-ins
 * ({@link HostActorEntity}) block movement into them like any collidable entity, but GTA IV moves them
 * by teleporting, so a ped walking into the player or a car driving into a mob ends up overlapping it.
 * That body is then put back out (sideways, or up onto it if it's only a little in), and a moving
 * vehicle shoves it along. The player does this on its own client (ProxyPushClient), mobs on the
 * server ({@link #shoveMobs}).
 */
public final class ProxyPush {
	/** Bodies whose feet are this far (blocks) below the top of a stand-in step up onto it instead. */
	public static final double STEP_UP = 0.5;
	/** A ped moves a body at most this far a tick (blocks): a nudge, not a teleport. */
	public static final double PED_MAX_PUSH = 0.3;
	/** A vehicle moving this fast (blocks a tick) or faster shoves what it drives into. */
	public static final double SHOVE_SPEED = 0.05;

	private ProxyPush() {
	}

	/**
	 * The move {x, y, z} that takes box a out of box b, or null if they don't overlap: sideways along x
	 * or z (whichever is shorter), or up onto b if a's feet are less than {@code stepUp} below its top.
	 * If b is moving ({@code bvx}, {@code bvz} per tick), never back through it against its motion.
	 */
	public static double @Nullable [] separation(
		double aMinX, double aMinY, double aMinZ, double aMaxX, double aMaxY, double aMaxZ,
		double bMinX, double bMinY, double bMinZ, double bMaxX, double bMaxY, double bMaxZ,
		double bvx, double bvz, double stepUp
	) {
		final double eps = 1.0E-7;
		if (aMaxX <= bMinX + eps || aMinX >= bMaxX - eps || aMaxY <= bMinY + eps || aMinY >= bMaxY - eps || aMaxZ <= bMinZ + eps || aMinZ >= bMaxZ - eps) {
			return null;
		}
		// Candidate moves: +x, -x, +z, -z.
		double[][] moves = {
			{ bMaxX - aMinX, 0.0, 0.0 },
			{ -(aMaxX - bMinX), 0.0, 0.0 },
			{ 0.0, 0.0, bMaxZ - aMinZ },
			{ 0.0, 0.0, -(aMaxZ - bMinZ) },
		};
		boolean moving = bvx * bvx + bvz * bvz > 1.0E-6;
		double[] best = null;
		double bestLen = Double.POSITIVE_INFINITY;
		for (int pass = 0; pass < 2 && best == null; pass++) {
			for (double[] m : moves) {
				// First pass: only moves that don't go against the vehicle's motion.
				if (pass == 0 && moving && m[0] * bvx + m[2] * bvz < -1.0E-9) {
					continue;
				}
				double len = Math.abs(m[0]) + Math.abs(m[2]);
				if (len < bestLen) {
					bestLen = len;
					best = m;
				}
			}
		}
		double up = bMaxY - aMinY;
		if (up <= stepUp && up < bestLen) {
			return new double[] { 0.0, up, 0.0 };
		}
		return best == null ? null : best.clone();
	}

	/** {@link #separation} for two boxes. */
	public static double @Nullable [] separation(AABB a, AABB b, double bvx, double bvz, double stepUp) {
		return separation(a.minX, a.minY, a.minZ, a.maxX, a.maxY, a.maxZ, b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ, bvx, bvz, stepUp);
	}

	/**
	 * The velocity a body gets from a stand-in moving (bvx, bvz) a tick that pushed it out along
	 * {@code move}: the stand-in's speed along that direction (horizontal), and a little lift when it
	 * was fast. Zero if it wasn't moving that way.
	 */
	public static double[] shove(double[] move, double bvx, double bvz) {
		double len = Math.sqrt(move[0] * move[0] + move[2] * move[2]);
		if (len < 1.0E-9) {
			return new double[3];
		}
		double nx = move[0] / len, nz = move[2] / len;
		double along = bvx * nx + bvz * nz;
		if (along < SHOVE_SPEED) {
			return new double[3];
		}
		return new double[] { nx * along * 1.2, Math.min(0.4, along * 0.4), nz * along * 1.2 };
	}

	/**
	 * Server: mobs (and other loose bodies) a GTA IV stand-in moved into this tick go back out of it, and
	 * a moving vehicle shoves them. {@code bvx}/{@code bvz}: how far the stand-in moved this tick.
	 */
	public static void shoveMobs(ServerLevel level, HostActorEntity proxy, double bvx, double bvz) {
		AABB box = proxy.getBoundingBox();
		List<Entity> hit = level.getEntities(proxy, box, e -> e instanceof Mob && e.isAlive() && !e.noPhysics && !e.isPassenger());
		for (Entity e : hit) {
			double[] move = separation(e.getBoundingBox(), box, bvx, bvz, STEP_UP);
			if (move == null) {
				continue;
			}
			if (!proxy.isHostVehicle()) {
				clamp(move, PED_MAX_PUSH);
			}
			e.move(MoverType.SHULKER, new Vec3(move[0], move[1], move[2]));
			double[] v = proxy.isHostVehicle() ? shove(move, bvx, bvz) : new double[3];
			if (v[0] != 0.0 || v[2] != 0.0) {
				e.setDeltaMovement(e.getDeltaMovement().add(v[0], v[1], v[2]));
				e.needsSync = true;
			}
		}
	}

	/**
	 * The horizontal unit direction {x, z} from the player toward what hurt it, from a kInHurt's flags
	 * (Proto.HURT_HAS_DIRECTION: the MC yaw in bits 16 to 24), or null if they carry none.
	 */
	public static double @Nullable [] hurtDirection(int flags) {
		if ((flags & Proto.HURT_HAS_DIRECTION) == 0) {
			return null;
		}
		double yaw = Math.toRadians((flags >>> Proto.HURT_DIRECTION_SHIFT) & 0x1FF);
		return new double[] { -Math.sin(yaw), Math.cos(yaw) };
	}

	/** Shortens a horizontal move to at most {@code max}. */
	public static void clamp(double[] move, double max) {
		double len = Math.sqrt(move[0] * move[0] + move[2] * move[2]);
		if (len > max) {
			move[0] *= max / len;
			move[2] *= max / len;
		}
	}
}
