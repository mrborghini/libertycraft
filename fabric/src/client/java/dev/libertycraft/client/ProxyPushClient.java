package dev.libertycraft.client;

import dev.libertycraft.combat.HostActorEntity;
import dev.libertycraft.combat.ProxyPush;
import java.util.HashMap;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * The local player's side of {@link ProxyPush}: once a tick, if a GTA IV ped or vehicle moved into the
 * player, the player goes back out of it (a ped nudges, a car shoves). Walking into them is already
 * blocked by Minecraft's own entity collision (HostActorEntity.canBeCollidedWith).
 */
public final class ProxyPushClient {
	private static final Map<Integer, Vec3> LAST = new HashMap<>();
	private static final Map<Integer, Vec3> SEEN = new HashMap<>();

	private ProxyPushClient() {
	}

	public static void tick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (player == null || minecraft.level == null || !HostClient.linked() || player.noPhysics || player.isSpectator() || player.isPassenger()
			|| HostDriveClient.driving()) {
			LAST.clear();
			return;
		}
		AABB box = player.getBoundingBox();
		SEEN.clear();
		for (Entity e : minecraft.level.getEntities(player, box.inflate(8.0), x -> x instanceof HostActorEntity)) {
			HostActorEntity proxy = (HostActorEntity) e;
			Vec3 now = proxy.position();
			Vec3 before = LAST.get(proxy.getId());
			SEEN.put(proxy.getId(), now);
			if (!proxy.isAlive()) {
				continue;
			}
			double vx = before == null ? 0.0 : now.x - before.x, vz = before == null ? 0.0 : now.z - before.z;
			if (vx * vx + vz * vz > 16.0) {
				vx = vz = 0.0; // a jump (the actor was teleported), not motion
			}
			double[] move = ProxyPush.separation(box, proxy.getBoundingBox(), vx, vz, ProxyPush.STEP_UP);
			if (move == null) {
				continue;
			}
			if (!proxy.isHostVehicle()) {
				ProxyPush.clamp(move, ProxyPush.PED_MAX_PUSH);
			}
			player.move(MoverType.SHULKER, new Vec3(move[0], move[1], move[2]));
			box = player.getBoundingBox();
			double[] v = proxy.isHostVehicle() ? ProxyPush.shove(move, vx, vz) : new double[3];
			if (v[0] != 0.0 || v[2] != 0.0) {
				player.setDeltaMovement(player.getDeltaMovement().add(v[0], v[1], v[2]));
			}
		}
		LAST.clear();
		LAST.putAll(SEEN);
	}
}
