package dev.libertycraft.client;

import dev.libertycraft.world.HostCollision;
import dev.libertycraft.world.HostTri;
import dev.libertycraft.world.TriCollider;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/** Feeds the local player's movement through {@link TriCollider} against nearby GTA IV triangles. */
public final class HostCollider {
	private HostCollider() {
	}

	public static Vec3 collide(LocalPlayer player, Vec3 move) {
		AABB box = player.getBoundingBox();
		double step = player.maxUpStep();
		List<HostTri> tris = new ArrayList<>();
		HostCollision.trianglesNear(box.expandTowards(move).inflate(1.0, 1.0 + step, 1.0), tris);
		if (tris.isEmpty()) {
			return move;
		}
		double[] r = TriCollider.resolve(
			tris, (box.minX + box.maxX) * 0.5, box.minY, (box.minZ + box.maxZ) * 0.5, box.getXsize() * 0.5, box.getYsize(), step, player.onGround(),
			move.x, move.y, move.z
		);
		if (r[0] == move.x && r[1] == move.y && r[2] == move.z) {
			return move;
		}
		// The triangle pass (snapping down a slope, pushing out of a wall) can move the player into a
		// Minecraft block placed on the terrain, or a GTA IV ped's or car's stand-in; collide that
		// result with those again.
		Vec3 smooth = new Vec3(r[0], r[1], r[2]);
		return Entity.collideBoundingBox(player, smooth, box, player.level(), player.level().getEntityCollisions(player, box.expandTowards(smooth)));
	}

	/** Highest GTA IV surface at or below {@code maxAbove} over the feet at (x, y, z), or NaN. */
	public static double groundAt(double x, double y, double z, double maxAbove) {
		List<HostTri> tris = new ArrayList<>();
		HostCollision.trianglesNear(new AABB(x - 1, y - 4, z - 1, x + 1, y + maxAbove + 1, z + 1), tris);
		return TriCollider.groundAt(tris, x, y, z, maxAbove);
	}

	public static double walkableGroundAt(double x, double y, double z, double maxAbove) {
		List<HostTri> tris = new ArrayList<>();
		HostCollision.trianglesNear(new AABB(x - 1, y - 4, z - 1, x + 1, y + maxAbove + 1, z + 1), tris);
		return TriCollider.walkableGroundAt(tris, x, y, z, maxAbove);
	}
}
