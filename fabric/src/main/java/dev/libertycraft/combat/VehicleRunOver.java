package dev.libertycraft.combat;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import dev.libertycraft.world.HostDrive;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageType;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.decoration.ArmorStand;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * GTA IV's vehicles run Minecraft's mobs over, server side, once per actor table (HostCombat).
 *
 * <p>Every vehicle in the table (traffic, parked cars, and the one the player sits in:
 * {@link Proto#ACTOR_PLAYER_VEHICLE}) is rebuilt as one pose from its records and compared with its
 * pose in the previous table, over the time between the two tables (the host's stamp, see
 * {@link Link#readActors(List, long[])}). A living mob (not a player, a stand-in, an armour stand or
 * the player's mount) it drives into at 2.5 m/s or more is hurt by the speed and thrown along the
 * vehicle's way with some lift ({@link RunOver}); slower, it is only pushed aside (ProxyPush). One hit
 * per mob and vehicle as long as they stay in touch (a vehicle is one pose, so its pieces can't hit
 * twice), and a thrown mob isn't shoved by the stand-ins for a moment. The player's own vehicle has no
 * stand-ins: its pieces push mobs aside here, and what it runs over is the player's kill (the damage's
 * attacker is the player: drops and experience as from the player's own hit); traffic's hits have no
 * attacker. The player's mount itself never touches anything (HostMountBoatMixin, HostDrive).
 */
public final class VehicleRunOver {
	/** LibertyCraft's damage type for a vehicle running something over (data/libertycraft/damage_type/vehicle.json). */
	public static final ResourceKey<DamageType> VEHICLE_DAMAGE =
		ResourceKey.create(Registries.DAMAGE_TYPE, Identifier.fromNamespaceAndPath(LibertyCraft.MOD_ID, "vehicle"));

	/** A mob out of touch with a vehicle this long (ticks) can be hit by it again. */
	private static final int RECONTACT_TICKS = 10;
	/** A thrown mob isn't shoved by the stand-ins for this long (ticks): it flies. */
	private static final int THROWN_TICKS = 10;

	private record Track(RunOver.Pose pose, long stamp, long nanos) {
	}

	private static final Map<Integer, Track> TRACKS = new HashMap<>();
	private static final Map<Long, Long> CONTACTS = new HashMap<>();
	private static final Map<Integer, Long> THROWN = new HashMap<>();
	private static final Map<Integer, double[]> PLAYER_PIECES = new HashMap<>();
	private static final Map<Integer, List<Link.Actor>> BY_VEHICLE = new HashMap<>();
	private static int hits;

	private VehicleRunOver() {
	}

	/** {@code entity} was thrown by a vehicle a moment ago (ProxyPush leaves it alone meanwhile). */
	public static boolean recentlyThrown(Entity entity) {
		Long until = THROWN.get(entity.getId());
		return until != null && until > entity.level().getGameTime();
	}

	/** Mobs a vehicle can run over. */
	static boolean runnable(Entity e) {
		return e instanceof LivingEntity && e.isAlive() && !(e instanceof Player) && !(e instanceof HostActorEntity) && !(e instanceof ArmorStand)
			&& !HostDrive.isMount(e) && !e.isSpectator() && !e.noPhysics && !e.isPassenger();
	}

	/** Server thread, once per actor table read: {@code stamp} is when the host wrote it (0: unknown). */
	static void tick(ServerLevel level, List<Link.Actor> actors, long stamp, List<ServerPlayer> online) {
		long nanos = System.nanoTime();
		long now = level.getGameTime();
		BY_VEHICLE.clear();
		for (Link.Actor a : actors) {
			if ((a.flags() & Proto.ACTOR_VEHICLE) != 0 && (a.flags() & Proto.ACTOR_DEAD) == 0) {
				BY_VEHICLE.computeIfAbsent(a.formId() & ~Proto.ACTOR_VEHICLE_PIECE_MASK, k -> new ArrayList<>(8)).add(a);
			}
		}
		Set<Integer> seenPieces = new HashSet<>();
		for (Map.Entry<Integer, List<Link.Actor>> e : BY_VEHICLE.entrySet()) {
			List<Link.Actor> pieces = e.getValue();
			int n = pieces.size();
			double[] x = new double[n], y = new double[n], z = new double[n], w = new double[n], h = new double[n];
			boolean players = false;
			for (int i = 0; i < n; i++) {
				Link.Actor a = pieces.get(i);
				x[i] = a.x();
				y[i] = a.y();
				z[i] = a.z();
				w[i] = a.width();
				h[i] = a.height();
				players |= (a.flags() & Proto.ACTOR_PLAYER_VEHICLE) != 0;
			}
			RunOver.Pose pose = RunOver.pose(x, y, z, w, h, n, pieces.getFirst().yaw());
			Track before = TRACKS.get(e.getKey());
			if (before != null && stamp != 0 && before.stamp() == stamp) {
				continue; // the same table as last tick (GTA IV didn't write a new one): nothing moved
			}
			TRACKS.put(e.getKey(), new Track(pose, stamp, nanos));
			double dt = before == null ? 0.0 : stamp != 0 && before.stamp() != 0 ? (stamp - before.stamp()) * 1.0E-7 : (nanos - before.nanos()) * 1.0E-9;
			if (dt > 0.005 && dt < 0.3) {
				runOver(level, e.getKey(), pose, before.pose(), dt, players, players ? driver(online) : null, pieces.getFirst().name(), now);
			}
			if (players) {
				pushAside(level, pieces, seenPieces); // (after: the run-over looks at where the mobs came from)
			}
		}
		TRACKS.keySet().retainAll(BY_VEHICLE.keySet());
		PLAYER_PIECES.keySet().retainAll(seenPieces);
		if (now % 200 == 0) {
			CONTACTS.values().removeIf(t -> now - t > 200);
			THROWN.values().removeIf(t -> t < now);
		}
	}

	private static @Nullable ServerPlayer driver(List<ServerPlayer> players) {
		for (ServerPlayer p : players) {
			if (p.getVehicle() != null && HostDrive.isMount(p.getVehicle())) {
				return p;
			}
		}
		return players.isEmpty() ? null : players.getFirst();
	}

	private static void runOver(ServerLevel level, int vehicle, RunOver.Pose now, RunOver.Pose before, double dt, boolean players,
		@Nullable ServerPlayer driver, String name, long tick) {
		double reach = Math.max(now.halfLength(), now.halfWidth()) + 2.0;
		AABB area = new AABB(now.x() - reach, now.bottom() - 1.0, now.z() - reach, now.x() + reach, now.top() + 1.0, now.z() + reach);
		for (LivingEntity mob : level.getEntitiesOfClass(LivingEntity.class, area, VehicleRunOver::runnable)) {
			AABB box = mob.getBoundingBox();
			RunOver.Contact c = RunOver.contact(now, before, dt, mob.getX(), mob.getZ(), mob.xo, mob.zo, mob.getBbWidth() * 0.5, box.minY, box.maxY);
			if (!c.touching()) {
				continue;
			}
			long key = (long) vehicle << 32 | (mob.getId() & 0xFFFFFFFFL);
			Long last = CONTACTS.get(key);
			if (last != null && tick - last <= RECONTACT_TICKS) {
				CONTACTS.put(key, tick); // still the same pass
				continue;
			}
			if (!c.hit()) {
				continue; // too slow, or brushing past: ProxyPush moves it aside
			}
			CONTACTS.put(key, tick);
			hit(level, mob, c, now, players ? driver : null, players, name, tick);
		}
	}

	private static void hit(ServerLevel level, LivingEntity mob, RunOver.Contact c, RunOver.Pose pose, @Nullable ServerPlayer driver, boolean players,
		String name, long tick) {
		float damage = RunOver.damage(c.closing());
		DamageSource source = source(level, driver, pose);
		float before = mob.getHealth();
		// Out of the vehicle first, through the face it was hit by (else its pieces hold it in place).
		double tx = pose.worldX(c.outA(), c.outB()), tz = pose.worldZ(c.outA(), c.outB());
		mob.move(MoverType.SHULKER, new Vec3(tx - mob.getX(), 0.0, tz - mob.getZ()));
		boolean hurt = mob.hurtServer(level, source, damage);
		double[] v = RunOver.throwVelocity(c.closing(), c.vx(), c.vz(), c.nx(), c.nz(), mob.getAttributeValue(Attributes.KNOCKBACK_RESISTANCE));
		mob.setDeltaMovement(v[0], v[1], v[2]);
		mob.needsSync = true;
		THROWN.put(mob.getId(), tick + THROWN_TICKS);
		hits++;
		LibertyCraft.LOG.info("[LibertyCraft] run over: {} by {} ({}) at {} m/s ({} into it): {} damage{}, health {} -> {}{}, thrown {} m/s and {} up (run-over #{})",
			mob.getType().toShortString(), players ? "the player's vehicle" : "a GTA IV vehicle", name, String.format("%.1f", c.speed()),
			String.format("%.1f", c.closing()), String.format("%.1f", damage), driver != null ? " by " + driver.getPlainTextName() : "",
			String.format("%.1f", before), String.format("%.1f", mob.getHealth()), hurt ? (mob.isDeadOrDying() ? ", killed" : "") : " (not hurt)",
			String.format("%.1f", Math.hypot(v[0], v[2]) * 20.0), String.format("%.2f", v[1]), hits);
	}

	private static DamageSource source(ServerLevel level, @Nullable ServerPlayer driver, RunOver.Pose pose) {
		var type = level.registryAccess().lookupOrThrow(Registries.DAMAGE_TYPE).get(VEHICLE_DAMAGE);
		if (type.isEmpty()) {
			return driver != null ? level.damageSources().playerAttack(driver) : level.damageSources().generic(); // (the data pack is missing)
		}
		return driver != null ? new DamageSource(type.get(), driver) : new DamageSource(type.get(), new Vec3(pose.x(), pose.bottom(), pose.z()));
	}

	/** The player's vehicle has no stand-ins: its pieces put mobs back out of themselves and shove them, as ProxyPush does. */
	private static void pushAside(ServerLevel level, List<Link.Actor> pieces, Set<Integer> seen) {
		for (Link.Actor a : pieces) {
			seen.add(a.formId());
			double[] was = PLAYER_PIECES.put(a.formId(), new double[] { a.x(), a.z() });
			double r = a.width() * 0.5;
			AABB box = new AABB(a.x() - r, a.y(), a.z() - r, a.x() + r, a.y() + a.height(), a.z() + r);
			ProxyPush.shoveMobs(level, null, box, true, was != null ? a.x() - was[0] : 0.0, was != null ? a.z() - was[1] : 0.0);
		}
	}

	/** The formId of the player's vehicle's piece nearest (x, z) in the last actor table, 0 if none. */
	static int nearestPlayerPiece(double x, double z) {
		int best = 0;
		double bestD = Double.MAX_VALUE;
		for (Map.Entry<Integer, double[]> e : PLAYER_PIECES.entrySet()) {
			double dx = e.getValue()[0] - x, dz = e.getValue()[1] - z, d = dx * dx + dz * dz;
			if (d < bestD) {
				bestD = d;
				best = e.getKey();
			}
		}
		return best;
	}

	static void clear() {
		TRACKS.clear();
		CONTACTS.clear();
		THROWN.clear();
		PLAYER_PIECES.clear();
	}
}
