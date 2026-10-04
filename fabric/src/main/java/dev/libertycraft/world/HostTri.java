package dev.libertycraft.world;

/**
 * One exact GTA IV collision triangle in Minecraft space, with precomputed plane and bounds. Kept as
 * floats (GTA IV sends floats; the normal is worked out in double and rounded once): a long drive holds
 * hundreds of thousands of these, so each one is 96 bytes instead of 168.
 */
public final class HostTri {
	/** Surfaces at most ~45 degrees from flat can be walked on. */
	public static final double WALKABLE_NY = 0.7;

	public final float ax, ay, az, bx, by, bz, cx, cy, cz;
	public final float nx, ny, nz; // unit normal (winding is not trusted; use |ny|)
	public final float minX, minY, minZ, maxX, maxY, maxZ;
	public final boolean stairHelper;
	public final boolean walkable;
	/** Ground, rock, trees...: can be dug into. Its normal then faces out of the solid side. */
	public final boolean diggable;
	/** What it digs into (Proto.DIG_*). */
	public final int material;
	/** GTA IV's land (terrain), not an object on it. */
	public final boolean terrain;
	/** GTA IV's material of the surface (its index in materials.dat: CityCells.materialName), -1 if not sent. */
	public final int gtaMaterial;

	public HostTri(float[] v, int o, boolean stairHelper) {
		this(v, o, stairHelper ? dev.libertycraft.link.Proto.TRI_STAIR_HELPER : 0);
	}

	/** {@code flags}: the triangle's Proto.TRI_* flags as GTA IV sent them. */
	public HostTri(float[] v, int o, int flags) {
		boolean stairHelper = (flags & dev.libertycraft.link.Proto.TRI_STAIR_HELPER) != 0;
		this.diggable = (flags & dev.libertycraft.link.Proto.TRI_DIGGABLE) != 0;
		this.material = (flags >>> dev.libertycraft.link.Proto.TRI_MATERIAL_SHIFT) & 0xFF;
		this.terrain = (flags & dev.libertycraft.link.Proto.TRI_TERRAIN) != 0;
		this.gtaMaterial = (flags & dev.libertycraft.link.Proto.TRI_GTA_MATERIAL) != 0 ? (flags >>> dev.libertycraft.link.Proto.TRI_GTA_MATERIAL_SHIFT) & 0xFF : -1;
		this.ax = v[o];
		this.ay = v[o + 1];
		this.az = v[o + 2];
		this.bx = v[o + 3];
		this.by = v[o + 4];
		this.bz = v[o + 5];
		this.cx = v[o + 6];
		this.cy = v[o + 7];
		this.cz = v[o + 8];
		double ux = (double) this.bx - this.ax, uy = (double) this.by - this.ay, uz = (double) this.bz - this.az;
		double wx = (double) this.cx - this.ax, wy = (double) this.cy - this.ay, wz = (double) this.cz - this.az;
		double qx = uy * wz - uz * wy, qy = uz * wx - ux * wz, qz = ux * wy - uy * wx;
		double len = Math.sqrt(qx * qx + qy * qy + qz * qz);
		if (len < 1e-12) {
			this.nx = 0;
			this.ny = 1;
			this.nz = 0;
		} else {
			this.nx = (float) (qx / len);
			this.ny = (float) (qy / len);
			this.nz = (float) (qz / len);
		}
		this.minX = Math.min(this.ax, Math.min(this.bx, this.cx));
		this.minY = Math.min(this.ay, Math.min(this.by, this.cy));
		this.minZ = Math.min(this.az, Math.min(this.bz, this.cz));
		this.maxX = Math.max(this.ax, Math.max(this.bx, this.cx));
		this.maxY = Math.max(this.ay, Math.max(this.by, this.cy));
		this.maxZ = Math.max(this.az, Math.max(this.bz, this.cz));
		this.stairHelper = stairHelper;
		this.walkable = stairHelper || Math.abs(this.ny) >= WALKABLE_NY;
	}

	public boolean degenerate() {
		return this.maxX - this.minX < 1e-9 && this.maxZ - this.minZ < 1e-9;
	}

	/**
	 * Height of the triangle's plane above (x, z) if that point lies inside the triangle's
	 * horizontal footprint, otherwise NaN. Near-vertical triangles have no meaningful height.
	 */
	public double heightAt(double x, double z) {
		if (x < this.minX - 1e-9 || x > this.maxX + 1e-9 || z < this.minZ - 1e-9 || z > this.maxZ + 1e-9 || Math.abs(this.ny) < 0.05) {
			return Double.NaN;
		}
		double d1 = edge(x, z, this.ax, this.az, this.bx, this.bz);
		double d2 = edge(x, z, this.bx, this.bz, this.cx, this.cz);
		double d3 = edge(x, z, this.cx, this.cz, this.ax, this.az);
		boolean hasNeg = d1 < -1e-12 || d2 < -1e-12 || d3 < -1e-12;
		boolean hasPos = d1 > 1e-12 || d2 > 1e-12 || d3 > 1e-12;
		if (hasNeg && hasPos) {
			return Double.NaN;
		}
		// Plane: n . (p - a) = 0  ->  y = ay - (nx (x - ax) + nz (z - az)) / ny
		return this.ay - (this.nx * (x - this.ax) + this.nz * (z - this.az)) / this.ny;
	}

	private static double edge(double px, double pz, double x0, double z0, double x1, double z1) {
		return (x1 - x0) * (pz - z0) - (z1 - z0) * (px - x0);
	}
}
