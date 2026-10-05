package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import dev.libertycraft.world.HostDrive;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * The local player at speed against GTA IV's world:
 * <ul>
 * <li>every tick it moves at {@link Proto#MOVER_MIN_SPEED} or more (sprinting, in elytra flight, or riding a
 * Minecraft mount: then the mount's box) GTA IV is told ({@link Proto#EV_MOVER}); GTA IV knocks over the props
 * (traffic lights, lamp posts, bins...) it runs into fast enough, taking them out of the collision first, and
 * says how much of its speed the mover keeps ({@link Proto#IN_PROP_HIT}, {@link #propHit});</li>
 * <li>an elytra crash ({@link #fallFlyingMoved}, from LivingEntityFallFlyingMixin): flying into a wall hurts the
 * player as vanilla does ({@code flyIntoWall}), also against GTA IV's walls, which the integrated server's own
 * check can't see (the player collides with GTA IV's triangles on the client only); a hard crash, into a wall or
 * onto the ground, is sent to GTA IV ({@link Proto#EV_IMPACT}), which knocks the player over.</li>
 * </ul>
 */
public final class MoverClient {
	private static @Nullable Vec3 last;
	private static @Nullable Entity lastMover;
	private static long lastCrashTick = -100;
	private static int logged;
	private static boolean wasFlying;
	private static int flightTicks; // ticks the elytra has been gliding (a ground crash needs a real flight)
	private static double lastSpeed; // m/s, the mover's last tick (log: a hard stop)
	// HostCollider: whether GTA IV's triangles cut the local player's horizontal move this time.
	private static boolean hostCutHorizontal;

	/** A crash under this (m/s) isn't sent (GTA IV's own thresholds sit above it: drive/PropHit.h). */
	private static final double SEND_WALL = 6.0, SEND_GROUND = 8.0;
	private static final int MIN_FLIGHT_TICKS = 10;

	private MoverClient() {
	}

	public static void tick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (player == null || minecraft.level == null || !HostClient.linked() || HostDriveClient.driving() || player.isSpectator()) {
			last = null;
			lastMover = null;
			return;
		}
		if (player.isFallFlying() != wasFlying && logged < 40) {
			logged++;
			LibertyCraft.LOG.info("[LibertyCraft] elytra flight {}", player.isFallFlying() ? "started" : "over");
		}
		wasFlying = player.isFallFlying();
		if (!wasFlying) {
			flightTicks = 0;
		}
		Entity vehicle = player.getVehicle();
		boolean riding = vehicle instanceof LivingEntity && !HostDrive.isMount(vehicle);
		Entity mover = riding ? vehicle : player;
		Vec3 here = mover.position();
		boolean same = last != null && mover == lastMover;
		Vec3 moved = same ? here.subtract(last) : Vec3.ZERO;
		last = here;
		lastMover = mover;
		double speed = moved.length() * 20.0;
		if (same && lastSpeed >= 8.0 && speed < lastSpeed * 0.6 && moved.lengthSqr() <= 25.0 && logged < 40) {
			logged++;
			LibertyCraft.LOG.info("[LibertyCraft] {} stopped hard: {} to {} m/s at {} {} {} (ran into something: {}, {} wide)", riding ? "the mount" : "the player",
				String.format("%.1f", lastSpeed), String.format("%.1f", speed), String.format("%.2f", here.x), String.format("%.2f", here.y), String.format("%.2f", here.z),
				mover.horizontalCollision, String.format("%.2f", mover.getBbWidth()));
		}
		lastSpeed = moved.lengthSqr() > 25.0 ? 0.0 : speed; // (a teleport isn't a speed)
		if (speed < Proto.MOVER_MIN_SPEED || moved.lengthSqr() > 25.0) {
			return; // slow, or a teleport
		}
		AABB box = mover.getBoundingBox();
		int widthCm = (int) Math.min(1023, Math.round(box.getXsize() * 100.0));
		int heightCm = (int) Math.min(1023, Math.round(box.getYsize() * 100.0));
		int flags = (riding ? Proto.MOVER_RIDING : 0) | (player.isFallFlying() ? Proto.MOVER_FLYING : 0) | (player.isSprinting() ? Proto.MOVER_SPRINTING : 0);
		float yaw = (float) Math.toDegrees(Math.atan2(-moved.x, moved.z));
		float pitch = (float) Math.toDegrees(-Math.asin(Math.max(-1.0, Math.min(1.0, moved.y / moved.length()))));
		Link.pushEventLeaving(128, Proto.EV_MOVER, flags | widthCm << Proto.MOVER_WIDTH_SHIFT | heightCm << Proto.MOVER_HEIGHT_SHIFT, (float) ((box.minX + box.maxX) * 0.5),
			(float) box.minY, (float) ((box.minZ + box.maxZ) * 0.5), (float) speed, Float.floatToRawIntBits(yaw), Float.floatToRawIntBits(pitch));
	}

	/** GTA IV knocked over a prop the mover ran into: it keeps {@code keepPercent} of its speed. */
	static void propHit(Minecraft minecraft, int keepPercent, double x, double y, double z) {
		LocalPlayer player = minecraft.player;
		if (player == null) {
			return;
		}
		Entity vehicle = player.getVehicle();
		Entity mover = vehicle instanceof LivingEntity && !HostDrive.isMount(vehicle) ? vehicle : player;
		double keep = Math.max(0.0, Math.min(1.0, keepPercent / 100.0));
		Vec3 v = mover.getDeltaMovement();
		mover.setDeltaMovement(v.x * keep, v.y, v.z * keep);
		player.playSound(SoundEvents.ZOMBIE_ATTACK_IRON_DOOR, 0.6F, 0.7F + (float) keep * 0.3F);
		if (logged++ < 30) {
			LibertyCraft.LOG.info("[LibertyCraft] knocked over a GTA IV prop at {} {} {} going {} m/s: keeps {}% of the speed", String.format("%.1f", x),
				String.format("%.1f", y), String.format("%.1f", z), String.format("%.1f", v.horizontalDistance() * 20.0), keepPercent);
		}
	}

	/** HostCollider, for the local player's move: did GTA IV's triangles cut it horizontally. */
	public static void hostCollided(boolean cutHorizontal) {
		hostCutHorizontal = cutHorizontal;
	}

	/** LivingEntityFallFlyingMixin, before the local player's elytra move. */
	public static void beforeFallFlyingMove() {
		hostCutHorizontal = false;
	}

	/**
	 * LivingEntityFallFlyingMixin, after the local player's elytra move: {@code before} its motion before it
	 * (blocks a tick), {@code after} what the collisions left of it.
	 */
	public static void fallFlyingMoved(LocalPlayer player, Vec3 before, Vec3 after) {
		if (!HostClient.linked() || HostDriveClient.driving()) {
			return;
		}
		flightTicks++;
		double lostH = (before.horizontalDistance() - after.horizontalDistance()) * 20.0; // m/s
		boolean wall = player.horizontalCollision && lostH > 0.0;
		// Onto the ground while gliding, after a real flight: a fall that opened the elytra just before landing
		// (or any plain fall) keeps Minecraft's own fall damage and no more.
		boolean ground = player.verticalCollisionBelow && before.y < 0.0 && flightTicks > MIN_FLIGHT_TICKS;
		// Vanilla hurts the player flying into a wall on the server; against GTA IV's walls the server can't
		// see the collision (its copy of the player ignores GTA IV's geometry), so it is dealt here.
		if (wall && hostCutHorizontal) {
			float damage = (float) (lostH / 20.0 * 10.0 - 3.0);
			if (damage > 0.0F) {
				player.playSound(damage > 4.0F ? SoundEvents.PLAYER_BIG_FALL : SoundEvents.PLAYER_SMALL_FALL, 1.0F, 1.0F);
				flyIntoWall(Minecraft.getInstance(), damage);
			}
		}
		double hSpeed = before.horizontalDistance() * 20.0;
		double crash = wall ? lostH : ground ? Math.sqrt(before.y * 20.0 * before.y * 20.0 + hSpeed * 0.5 * hSpeed * 0.5) : 0.0;
		if (crash < (wall ? SEND_WALL : SEND_GROUND) || player.tickCount - lastCrashTick < 20) {
			return;
		}
		lastCrashTick = player.tickCount;
		double h = before.horizontalDistance();
		float dx = h > 1.0E-6 ? (float) (before.x / h) : 0.0F, dz = h > 1.0E-6 ? (float) (before.z / h) : 0.0F;
		Link.pushEvent(Proto.EV_IMPACT, 0, (float) crash, dx, dz, (float) hSpeed, wall ? Proto.IMPACT_WALL : Proto.IMPACT_GROUND);
		LibertyCraft.LOG.info("[LibertyCraft] elytra crash into {} at {} m/s (flying {} m/s{})", wall ? "a wall" : "the ground", String.format("%.1f", crash),
			String.format("%.1f", hSpeed), wall && hostCutHorizontal ? ", one of GTA IV's" : "");
	}

	private static void flyIntoWall(Minecraft minecraft, float damage) {
		var server = minecraft.getSingleplayerServer();
		if (minecraft.player == null || server == null) {
			return;
		}
		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			if (player != null && player.isAlive()) {
				player.hurtServer(player.level(), player.damageSources().flyIntoWall(), damage);
			}
		});
	}
}
