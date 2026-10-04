package dev.libertycraft.client;

import dev.libertycraft.combat.HostActorEntity;
import dev.libertycraft.combat.ProxyPush;
import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * The local player's side of {@link ProxyPush}, once a tick:
 * <ul>
 * <li>a GTA IV vehicle that moved into the player puts the player back out of it (and shoves it along
 * when it drives into the player). Walking into one is blocked by Minecraft's own entity collision
 * (HostActorEntity.canBeCollidedWith).</li>
 * <li>a ped (alive or dead) the player runs into is not solid: the player slows down and GTA IV is
 * told ({@link Proto#EV_BUMP}, every tick of contact, with the player's speed: its motion this tick, so
 * falls and elytra flight count) and moves the ped out of the way, trips it or knocks it down; a corpse
 * is pushed along. The contact is found along the player's whole move this tick, so a fast pass
 * doesn't skip a ped.</li>
 * </ul>
 */
public final class ProxyPushClient {
	private static final Map<Integer, Vec3> LAST = new HashMap<>();
	private static final Map<Integer, Vec3> SEEN = new HashMap<>();
	private static final Set<Integer> CONTACT = new HashSet<>();
	private static final Set<Integer> CONTACT_NOW = new HashSet<>();
	private static @org.jspecify.annotations.Nullable Vec3 lastPlayer;
	private static double lastVelocity;
	private static int logged;

	/** Player speed (m/s) from which a ped run into is knocked down (CombatMath.h kBumpKnockdownSpeed). */
	private static final double KNOCKDOWN_SPEED = 8.0;

	private ProxyPushClient() {
	}

	public static void tick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (player == null || minecraft.level == null || !HostClient.linked() || player.noPhysics || player.isSpectator() || player.isPassenger()
			|| HostDriveClient.driving()) {
			LAST.clear();
			CONTACT.clear();
			lastPlayer = null;
			return;
		}
		Vec3 here = player.position();
		Vec3 moved = lastPlayer == null ? Vec3.ZERO : here.subtract(lastPlayer);
		lastPlayer = here;
		// A teleport (the host moving the player, a resync) is not motion: far more than the velocity
		// accounts for. (On the ground the velocity left after friction is about half the step taken;
		// in the air, falling or gliding, it is the step; landing zeroes it, so last tick's counts too.)
		double velocity = Math.max(player.getDeltaMovement().length(), lastVelocity);
		lastVelocity = player.getDeltaMovement().length();
		if (moved.lengthSqr() > 25.0 || moved.length() > velocity * 3.0 + 0.3) {
			moved = Vec3.ZERO;
		}
		AABB box = player.getBoundingBox();
		AABB swept = box.expandTowards(moved.scale(-1.0)); // where the player went through this tick
		SEEN.clear();
		CONTACT_NOW.clear();
		double speed = moved.length() * 20.0;
		double slow = 1.0;
		for (Entity e : minecraft.level.getEntities(player, box.inflate(8.0).expandTowards(moved.scale(-1.0)), x -> x instanceof HostActorEntity)) {
			HostActorEntity proxy = (HostActorEntity) e;
			Vec3 now = proxy.position();
			Vec3 before = LAST.get(proxy.getId());
			SEEN.put(proxy.getId(), now);
			if (!proxy.isAlive()) {
				continue;
			}
			if (!proxy.isHostVehicle()) {
				slow = Math.min(slow, bump(proxy, swept, moved, speed, player.isSprinting(), player.isFallFlying()));
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
			player.move(MoverType.SHULKER, new Vec3(move[0], move[1], move[2]));
			box = player.getBoundingBox();
			double[] v = ProxyPush.shove(move, vx, vz);
			if (v[0] != 0.0 || v[2] != 0.0) {
				player.setDeltaMovement(player.getDeltaMovement().add(v[0], v[1], v[2]));
			}
		}
		if (slow < 1.0) {
			Vec3 d = player.getDeltaMovement();
			player.setDeltaMovement(d.x * slow, d.y, d.z * slow);
		}
		CONTACT.clear();
		CONTACT.addAll(CONTACT_NOW);
		LAST.clear();
		LAST.putAll(SEEN);
	}

	/**
	 * The player ran into a ped's stand-in this tick: tell GTA IV and return how much the player's
	 * horizontal motion keeps (1 if there was no contact).
	 */
	private static double bump(HostActorEntity proxy, AABB swept, Vec3 moved, double speed, boolean sprinting, boolean flying) {
		AABB pb = proxy.getBoundingBox();
		if (!swept.intersects(pb)) {
			return 1.0;
		}
		int id = proxy.formId();
		boolean fresh = !CONTACT.contains(id);
		CONTACT_NOW.add(id);
		// How far into it, horizontally (the smaller of the two overlaps).
		double ox = Math.min(swept.maxX, pb.maxX) - Math.max(swept.minX, pb.minX);
		double oz = Math.min(swept.maxZ, pb.maxZ) - Math.max(swept.minZ, pb.minZ);
		double overlap = Math.max(0.0, Math.min(ox, oz));
		double h = Math.hypot(moved.x, moved.z);
		float dx = h > 1.0E-4 ? (float) (moved.x / h) : 0.0F, dz = h > 1.0E-4 ? (float) (moved.z / h) : 0.0F;
		int flags = (sprinting ? Proto.BUMP_SPRINTING : 0) | (flying ? Proto.BUMP_FLYING : 0) | (fresh ? Proto.BUMP_NEW_CONTACT : 0);
		Link.pushEvent(Proto.EV_BUMP, id, (float) speed, dx, dz, (float) overlap, flags);
		if (fresh && logged < 30) {
			logged++;
			dev.libertycraft.LibertyCraft.LOG.info("[LibertyCraft] ran into {} {} at {} m/s{}{} (overlap {})", proxy.getName().getString(), Integer.toHexString(id),
				String.format("%.1f", speed), sprinting ? ", sprinting" : "", flying ? ", flying" : "", String.format("%.2f", overlap));
		}
		if (proxy.isHostCorpse()) {
			return 0.9; // dragging a body along
		}
		// A fast hit hands over some of the player's momentum, once; pushing through costs a little a tick.
		return fresh && speed >= KNOCKDOWN_SPEED ? 0.6 : 0.8;
	}
}
