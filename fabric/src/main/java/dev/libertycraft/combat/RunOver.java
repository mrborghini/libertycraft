package dev.libertycraft.combat;

/**
 * GTA IV's vehicles running Minecraft's mobs over: the maths, without Minecraft's classes (tested by
 * RunOverTest). {@link VehicleRunOver} applies it.
 *
 * <p>A vehicle reaches Minecraft as a row of upright boxes square to the world axes (its actor
 * records, {@link dev.libertycraft.link.Proto#ACTOR_VEHICLE}), all with the vehicle's yaw. Its pose is
 * rebuilt from them ({@link #pose}): the middle of the boxes (the host lays them out symmetrically
 * about the body's middle), the yaw, and how far the boxes reach along and across it. A mob touches it
 * when the mob's middle lies within that footprint plus the mob's half width ({@link #contact}).
 * What counts, as in GTA IV's own run-over of a ped (and RagdollOnVehicleHit for the player), is the
 * speed of the vehicle's point where the mob is, from this pose and the last one (its motion and its
 * turning), along the face of the footprint the mob came in through (where the mob was before, seen
 * from the vehicle as it was): a car driving into a mob hits it with its full speed, one brushing past
 * it only pushes it aside. 1 block = 1 metre.
 */
public final class RunOver {
	/** m/s into the mob: slower only pushes it aside (GTA IV's peds are knocked over from about 2 to 3 m/s). */
	public static final double MIN_SPEED = 2.5;
	/** Faster than this between two tables, GTA IV moved the vehicle (a teleport, a script), it didn't drive. */
	public static final double MAX_SPEED = 70.0;
	/** Blocks around the vehicle's boxes that still touch. */
	public static final double MARGIN = 0.1;
	/** Minecraft damage per m/s over {@link #DAMAGE_FROM}: 10 m/s deals 24 (a zombie, 20 health and 2 armour, dies). */
	public static final double DAMAGE_PER_MS = 3.0;
	public static final double DAMAGE_FROM = 2.0;
	/** The throw: horizontal speed off the hit as a share of the hit's speed, and the lift (blocks a tick). */
	public static final double THROW_SHARE = 1.0;
	public static final double LIFT_BASE = 0.2, LIFT_PER_MS = 0.025, LIFT_MIN = 0.25, LIFT_MAX = 0.8;
	/** A throw is at most this fast horizontally (blocks a tick: 30 m/s). */
	public static final double THROW_MAX = 1.5;

	private RunOver() {
	}

	/**
	 * A vehicle's pose: the middle of its boxes (x, z), their lowest bottom and highest top, its yaw (MC
	 * degrees: 0 south, 90 west) and how far its boxes reach from the middle along it and across it.
	 */
	public record Pose(double x, double z, double bottom, double top, double yaw, double halfLength, double halfWidth) {
		/** Forward (the way the yaw faces), x. */
		public double fx() {
			return -Math.sin(Math.toRadians(this.yaw));
		}

		public double fz() {
			return Math.cos(Math.toRadians(this.yaw));
		}

		/** Across (forward turned 90 degrees), x. */
		public double sx() {
			return Math.cos(Math.toRadians(this.yaw));
		}

		public double sz() {
			return Math.sin(Math.toRadians(this.yaw));
		}

		/** Along (a) and across (b) of the point (x, z) from the middle. */
		public double along(double x, double z) {
			return (x - this.x) * this.fx() + (z - this.z) * this.fz();
		}

		public double across(double x, double z) {
			return (x - this.x) * this.sx() + (z - this.z) * this.sz();
		}

		/** The world point at (along, across). */
		public double worldX(double a, double b) {
			return this.x + a * this.fx() + b * this.sx();
		}

		public double worldZ(double a, double b) {
			return this.z + a * this.fz() + b * this.sz();
		}
	}

	/**
	 * The pose of a vehicle from its {@code n} boxes: box i stands on (x[i], y[i], z[i]) (its bottom
	 * centre), w[i] wide on both axes and h[i] tall; yaw is the vehicle's.
	 */
	public static Pose pose(double[] x, double[] y, double[] z, double[] w, double[] h, int n, double yaw) {
		double minX = Double.POSITIVE_INFINITY, maxX = Double.NEGATIVE_INFINITY, minZ = Double.POSITIVE_INFINITY, maxZ = Double.NEGATIVE_INFINITY;
		double bottom = Double.POSITIVE_INFINITY, top = Double.NEGATIVE_INFINITY;
		for (int i = 0; i < n; i++) {
			double r = w[i] * 0.5;
			minX = Math.min(minX, x[i] - r);
			maxX = Math.max(maxX, x[i] + r);
			minZ = Math.min(minZ, z[i] - r);
			maxZ = Math.max(maxZ, z[i] + r);
			bottom = Math.min(bottom, y[i]);
			top = Math.max(top, y[i] + h[i]);
		}
		double cx = (minX + maxX) * 0.5, cz = (minZ + maxZ) * 0.5;
		Pose p = new Pose(cx, cz, bottom, top, yaw, 0.0, 0.0);
		double fx = p.fx(), fz = p.fz(), sx = p.sx(), sz = p.sz();
		// An axis-aligned box of side w reaches w/2 (|ux| + |uz|) along a unit u.
		double alongReach = Math.abs(fx) + Math.abs(fz), acrossReach = Math.abs(sx) + Math.abs(sz);
		double halfLength = 0.0, halfWidth = 0.0;
		for (int i = 0; i < n; i++) {
			double dx = x[i] - cx, dz = z[i] - cz, r = w[i] * 0.5;
			halfLength = Math.max(halfLength, Math.abs(dx * fx + dz * fz) + r * alongReach);
			halfWidth = Math.max(halfWidth, Math.abs(dx * sx + dz * sz) + r * acrossReach);
		}
		return new Pose(cx, cz, bottom, top, yaw, halfLength, halfWidth);
	}

	/**
	 * A mob and a vehicle this tick. touching: the mob is at the vehicle; hit: it is being run over.
	 * speed: of the vehicle's point at the mob (m/s); closing: that speed into the mob, through the face
	 * it came in by; (vx, vz): that point's velocity (m/s); (nx, nz): the face's outward normal;
	 * (outA, outB): where the mob's middle goes to be clear of the vehicle, in the vehicle's frame.
	 */
	public record Contact(boolean touching, boolean hit, double speed, double closing, double vx, double vz, double nx, double nz, double outA, double outB) {
		static final Contact NONE = new Contact(false, false, 0, 0, 0, 0, 0, 0, 0, 0);
	}

	/**
	 * The mob (middle (mx, mz), half width r, from minY to maxY; its middle was at (px, pz) a tick ago)
	 * and the vehicle (pose {@code now}, and {@code before}, {@code dt} seconds earlier).
	 */
	public static Contact contact(Pose now, Pose before, double dt, double mx, double mz, double px, double pz, double r, double minY, double maxY) {
		if (minY >= now.top() - 0.05 || maxY <= now.bottom() + 0.05) {
			return Contact.NONE; // on its roof, or under it
		}
		double a = now.along(mx, mz), b = now.across(mx, mz);
		double reachA = now.halfLength() + r, reachB = now.halfWidth() + r;
		if (Math.abs(a) > reachA + MARGIN || Math.abs(b) > reachB + MARGIN) {
			return Contact.NONE;
		}
		if (!(dt > 1.0E-3)) {
			return new Contact(true, false, 0, 0, 0, 0, 0, 0, a, b);
		}
		// The vehicle's point where the mob is now, where it was a moment ago.
		double wasX = before.worldX(a, b), wasZ = before.worldZ(a, b);
		double vx = (mx - wasX) / dt, vz = (mz - wasZ) / dt;
		double speed = Math.sqrt(vx * vx + vz * vz);
		// The face it came in by: the side of the vehicle (as it was) the mob was furthest out of.
		double a0 = before.along(px, pz), b0 = before.across(px, pz);
		double outLength = Math.abs(a0) - (before.halfLength() + r), outWidth = Math.abs(b0) - (before.halfWidth() + r);
		double nx, nz, outA = a, outB = b;
		if (outLength >= outWidth) {
			double sign = a0 >= 0.0 ? 1.0 : -1.0;
			nx = now.fx() * sign;
			nz = now.fz() * sign;
			outA = sign * (reachA + 0.05);
		} else {
			double sign = b0 >= 0.0 ? 1.0 : -1.0;
			nx = now.sx() * sign;
			nz = now.sz() * sign;
			outB = sign * (reachB + 0.05);
		}
		double closing = vx * nx + vz * nz;
		boolean hit = Double.isFinite(speed) && speed <= MAX_SPEED && closing >= MIN_SPEED;
		return new Contact(true, hit, speed, closing, vx, vz, nx, nz, outA, outB);
	}

	/** Minecraft damage of a hit at {@code closing} m/s. */
	public static float damage(double closing) {
		return closing < MIN_SPEED ? 0.0F : (float) (DAMAGE_PER_MS * (closing - DAMAGE_FROM));
	}

	/**
	 * The mob's velocity off a hit at {@code closing} m/s (blocks a tick {x, y, z}): along the vehicle's
	 * motion (vx, vz), a little out through the face it was hit by (nx, nz), with some lift; less for
	 * mobs that resist knockback (0 to 1).
	 */
	public static double[] throwVelocity(double closing, double vx, double vz, double nx, double nz, double knockbackResistance) {
		double len = Math.sqrt(vx * vx + vz * vz);
		double dx = len > 1.0E-6 ? vx / len : nx, dz = len > 1.0E-6 ? vz / len : nz;
		dx += nx * 0.3;
		dz += nz * 0.3;
		double dl = Math.sqrt(dx * dx + dz * dz);
		if (dl > 1.0E-6) {
			dx /= dl;
			dz /= dl;
		}
		double keep = 1.0 - 0.6 * Math.clamp(knockbackResistance, 0.0, 1.0);
		double horizontal = Math.min(THROW_MAX, THROW_SHARE * closing / 20.0) * keep;
		double lift = Math.clamp(LIFT_BASE + LIFT_PER_MS * closing, LIFT_MIN, LIFT_MAX) * keep;
		return new double[] { dx * horizontal, lift, dz * horizontal };
	}
}
