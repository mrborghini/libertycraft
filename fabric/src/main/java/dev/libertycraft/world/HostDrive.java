package dev.libertycraft.world;

import dev.libertycraft.LibertyCraft;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.UUID;
import net.fabricmc.fabric.api.entity.event.v1.ServerLivingEntityEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.animal.equine.AbstractHorse;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * GTA IV drives the player (Niko mode, a vehicle, a cutscene; SkyState kSkyHostDrives): the server
 * side. The player's client reports where GTA IV has them every tick (LcNet.Drive); meanwhile they
 * take no damage, and in a vehicle (kSkyInVehicle) they ride a mount (an oak boat unless
 * {@code vehicleMount} in config/libertycraft.properties says horse, minecart or none) that sits
 * exactly where their seat is. The mount is invulnerable, weightless, passes through everything,
 * and goes when GTA IV lets go (or the reports stop). Server thread only.
 */
public final class HostDrive {
	/** Entities with this tag are mounts; any left in a saved world (a crash) are removed on load. */
	public static final String MOUNT_TAG = "libertycraft_mount";
	public static final int FLAG_DRIVES = 1;
	public static final int FLAG_IN_VEHICLE = 2;
	private static final String CONFIG_KEY = "vehicleMount";
	private static final String SHOW_MOUNT_KEY = "showVehicleMount";
	// The mount still exists in Minecraft (the player sits on it), it's just not drawn in GTA IV,
	// so the rider looks like they sit in the car's seat. Read by the client's scene exporter.
	private static volatile boolean showVehicleMount = false;

	/** Whether GTA IV should draw the mount the player rides while GTA IV drives a vehicle. */
	public static boolean showVehicleMount() {
		return showVehicleMount;
	}
	// Reports come every client tick; this long without one means GTA IV (or the client) is gone.
	private static final int STALE_TICKS = 40;

	public enum MountType {
		BOAT, HORSE, MINECART, NONE;

		public static MountType parse(@Nullable String text, MountType fallback) {
			if (text == null) {
				return fallback;
			}
			return switch (text.trim().toLowerCase(Locale.ROOT)) {
				case "boat", "oak_boat" -> BOAT;
				case "horse" -> HORSE;
				case "minecart", "cart" -> MINECART;
				case "none", "off", "" -> NONE;
				default -> fallback;
			};
		}
	}

	private static final class State {
		@Nullable Entity mount;
		MountType type = MountType.NONE;
		int lastTick;
	}

	private static final Map<UUID, State> STATES = new HashMap<>();
	private static @Nullable Entity spawning;
	private static volatile MountType mountType = MountType.BOAT;
	/** The client's own mount while GTA IV drives (client thread writes, collision mixins read). */
	public static volatile @Nullable Entity clientMount;
	/** Set while HostCombat applies one of GTA IV's own hits (kInHurt): it reaches a player GTA IV drives. Server thread. */
	public static boolean applyingHostHurt;

	private HostDrive() {
	}

	public static void init() {
		ServerLifecycleEvents.SERVER_STARTED.register(server -> loadConfig());
		ServerLifecycleEvents.SERVER_STOPPING.register(HostDrive::releaseAll);
		ServerTickEvents.END_SERVER_TICK.register(HostDrive::serverTick);
		// While GTA IV has them, the player can't be hurt in Minecraft: it isn't Minecraft moving them. GTA IV's
		// own hits (HostCombat.hurtPlayer) still reach them: seated in a vehicle or knocked over, the host
		// sends what GTA IV did to them, and Minecraft owns their health there too.
		ServerLivingEntityEvents.ALLOW_DAMAGE.register((entity, source, amount) -> !isMount(entity) && (!isFollowing(entity) || applyingHostHurt));
		ServerEntityEvents.ENTITY_LOAD.register((entity, level) -> {
			if (entity.entityTags().contains(MOUNT_TAG) && !isActiveMount(entity)) {
				LibertyCraft.LOG.info("[LibertyCraft] removing a leftover vehicle mount ({})", entity.getType().toShortString());
				entity.discard();
			}
		});
	}

	public static MountType mountType() {
		return mountType;
	}

	/** A host-vehicle mount: tagged on the server, the local player's own while GTA IV drives on the client. */
	public static boolean isMount(@Nullable Entity entity) {
		return entity != null && (entity == clientMount || entity.entityTags().contains(MOUNT_TAG));
	}

	private static boolean isFollowing(Entity entity) {
		return entity instanceof ServerPlayer && STATES.containsKey(entity.getUUID());
	}

	/** GTA IV drives this player (Niko mode, a vehicle, getting back up): Minecraft takes no damage of its own then. */
	public static boolean drivenByHost(Entity entity) {
		return isFollowing(entity);
	}

	private static boolean isActiveMount(Entity entity) {
		if (entity == spawning) {
			return true;
		}
		for (State st : STATES.values()) {
			if (st.mount == entity) {
				return true;
			}
		}
		return false;
	}

	/** The player's client: where GTA IV has them (feet, or the rider's feet in a vehicle), and how. */
	public static void follow(ServerPlayer player, double x, double y, double z, float yaw, int flags) {
		if ((flags & FLAG_DRIVES) == 0) {
			if (release(player, "GTA IV let go")) {
				// Where GTA IV let go (its teleport says the same; whichever arrives first wins nothing).
				player.teleportTo(x, y, z);
			}
			return;
		}
		MinecraftServer server = player.level().getServer();
		State st = STATES.get(player.getUUID());
		if (st == null) {
			st = new State();
			STATES.put(player.getUUID(), st);
			LibertyCraft.LOG.info("[LibertyCraft] GTA IV drives {}: following it (no physics; only GTA IV's own hits hurt)", player.getPlainTextName());
		}
		st.lastTick = server != null ? server.getTickCount() : 0;
		player.resetFallDistance();
		MountType type = (flags & FLAG_IN_VEHICLE) != 0 ? mountType : MountType.NONE;
		if (type == MountType.NONE) {
			dropMount(player, st);
			// Riding something of Minecraft's own (a horse) when GTA IV took Niko (Niko mode, a knockdown, a
			// cutscene): off it, so the player follows him; the horse stays where it is.
			Entity own = player.getVehicle();
			if (own != null && !isMount(own)) {
				player.stopRiding();
				LibertyCraft.LOG.info("[LibertyCraft] {} got off the {}: GTA IV drives Niko", player.getPlainTextName(), own.getType().toShortString());
			}
			return;
		}
		Entity mount = st.mount;
		if (mount == null || mount.isRemoved() || mount.level() != player.level() || st.type != type) {
			dropMount(player, st);
			mount = spawn(type, player.level(), x, y, z, yaw);
			if (mount == null) {
				return;
			}
			st.mount = mount;
			st.type = type;
		}
		if (player.getVehicle() != mount) {
			// force: a boat refuses riders while it's under (GTA IV's) water.
			boolean ok = player.startRiding(mount, true, false);
			LibertyCraft.LOG.info("[LibertyCraft] {} {} the {}", player.getPlainTextName(), ok ? "mounted" : "COULDN'T mount", type.name().toLowerCase(Locale.ROOT));
		}
		place(mount, player, x, y, z, yaw);
	}

	/**
	 * Puts the mount where its rider's feet land on (x, y, z), facing yaw, and the rider on it. Used by
	 * the server and by the client (whose copy of a boat or saddled horse is the authoritative one).
	 */
	public static void place(Entity mount, Entity rider, double x, double y, double z, float yaw) {
		mount.setYRot(yaw);
		mount.setXRot(0.0F);
		if (mount instanceof LivingEntity living) {
			living.yBodyRot = yaw;
			living.yBodyRotO = yaw;
			living.setYHeadRot(yaw);
			living.yHeadRotO = yaw;
		}
		mount.setDeltaMovement(Vec3.ZERO);
		mount.setPos(x, y, z);
		if (rider.getVehicle() == mount) {
			Vec3 seat = mount.getPassengerRidingPosition(rider).subtract(rider.getVehicleAttachmentPoint(mount));
			mount.setPos(x - (seat.x - mount.getX()), y - (seat.y - mount.getY()), z - (seat.z - mount.getZ()));
			mount.positionRider(rider);
		}
		mount.resetFallDistance();
	}

	private static @Nullable Entity spawn(MountType type, ServerLevel level, double x, double y, double z, float yaw) {
		Entity mount = switch (type) {
			case BOAT -> EntityTypes.OAK_BOAT.create(level, EntitySpawnReason.EVENT);
			case HORSE -> EntityTypes.HORSE.create(level, EntitySpawnReason.EVENT);
			case MINECART -> EntityTypes.MINECART.create(level, EntitySpawnReason.EVENT);
			case NONE -> null;
		};
		if (mount == null) {
			return null;
		}
		mount.snapTo(x, y, z, yaw, 0.0F);
		mount.setNoGravity(true);
		mount.noPhysics = true;
		mount.setPermanentlyInvulnerable(true);
		mount.setSilent(true);
		mount.addTag(MOUNT_TAG);
		if (mount instanceof AbstractHorse horse) {
			horse.setTamed(true);
			horse.setItemSlot(EquipmentSlot.SADDLE, new ItemStack(Items.SADDLE));
		}
		if (mount instanceof Mob mob) {
			mob.setNoAi(true);
		}
		spawning = mount; // ENTITY_LOAD fires inside addFreshEntity: not a leftover
		try {
			if (!level.addFreshEntity(mount)) {
				LibertyCraft.LOG.warn("[LibertyCraft] couldn't add the vehicle mount ({})", type);
				return null;
			}
		} finally {
			spawning = null;
		}
		LibertyCraft.LOG.info("[LibertyCraft] vehicle mount: {} at {} {} {}", type.name().toLowerCase(Locale.ROOT), String.format(Locale.ROOT, "%.2f", x),
			String.format(Locale.ROOT, "%.2f", y), String.format(Locale.ROOT, "%.2f", z));
		return mount;
	}

	private static void dropMount(ServerPlayer player, State st) {
		Entity mount = st.mount;
		if (mount == null) {
			return;
		}
		if (player.getVehicle() == mount) {
			// removeVehicle, not stopRiding: no dismount teleport; GTA IV says where the player goes.
			player.removeVehicle();
		}
		mount.discard();
		st.mount = null;
		st.type = MountType.NONE;
		LibertyCraft.LOG.info("[LibertyCraft] vehicle mount removed");
	}

	/** Ends following (no-op if it wasn't). True if the player was riding a mount that is now gone. */
	public static boolean release(ServerPlayer player, String why) {
		State st = STATES.remove(player.getUUID());
		if (st == null) {
			return false;
		}
		boolean hadMount = st.mount != null;
		dropMount(player, st);
		player.resetFallDistance();
		LibertyCraft.LOG.info("[LibertyCraft] {} is Minecraft's again ({})", player.getPlainTextName(), why);
		return hadMount;
	}

	private static void serverTick(MinecraftServer server) {
		if (STATES.isEmpty()) {
			return;
		}
		int now = server.getTickCount();
		List<UUID> stale = null;
		for (Map.Entry<UUID, State> e : STATES.entrySet()) {
			ServerPlayer player = server.getPlayerList().getPlayer(e.getKey());
			State st = e.getValue();
			if (player == null || now - st.lastTick > STALE_TICKS) {
				if (stale == null) {
					stale = new ArrayList<>();
				}
				stale.add(e.getKey());
			} else if (st.mount != null) {
				st.mount.setDeltaMovement(Vec3.ZERO);
			}
		}
		if (stale != null) {
			for (UUID id : stale) {
				ServerPlayer player = server.getPlayerList().getPlayer(id);
				if (player != null) {
					release(player, "no word from GTA IV");
				} else {
					State st = STATES.remove(id);
					if (st != null && st.mount != null) {
						st.mount.discard();
					}
				}
			}
		}
	}

	private static void releaseAll(MinecraftServer server) {
		for (Iterator<Map.Entry<UUID, State>> it = STATES.entrySet().iterator(); it.hasNext(); ) {
			Map.Entry<UUID, State> e = it.next();
			ServerPlayer player = server.getPlayerList().getPlayer(e.getKey());
			if (player != null && e.getValue().mount != null && player.getVehicle() == e.getValue().mount) {
				player.removeVehicle();
			}
			if (e.getValue().mount != null) {
				e.getValue().mount.discard();
			}
			it.remove();
		}
	}

	// ---- config/libertycraft.properties: vehicleMount=boat|horse|minecart|none --------------------------

	private static void loadConfig() {
		Path file = FabricLoader.getInstance().getConfigDir().resolve("libertycraft.properties");
		Properties props = new Properties();
		try {
			if (Files.exists(file)) {
				try (var in = Files.newBufferedReader(file)) {
					props.load(in);
				}
			}
			String value = props.getProperty(CONFIG_KEY);
			String show = props.getProperty(SHOW_MOUNT_KEY);
			if (value == null || show == null) {
				List<String> lines = Files.exists(file) ? new ArrayList<>(Files.readAllLines(file)) : new ArrayList<>(List.of("# LibertyCraft"));
				if (value == null) {
					lines.add("# What you sit on while GTA IV drives a vehicle: boat, horse, minecart or none.");
					lines.add(CONFIG_KEY + "=boat");
				}
				if (show == null) {
					lines.add("# Draw that mount in GTA IV (false: you appear to sit in the car's seat).");
					lines.add(SHOW_MOUNT_KEY + "=false");
				}
				Files.createDirectories(file.getParent());
				Files.write(file, lines);
			}
			mountType = MountType.parse(value, MountType.BOAT);
			showVehicleMount = Boolean.parseBoolean(show == null ? "false" : show.trim());
		} catch (IOException e) {
			LibertyCraft.LOG.warn("[LibertyCraft] couldn't read {}", file, e);
		}
		LibertyCraft.LOG.info("[LibertyCraft] vehicle mount: {} ({} in {}), drawn in GTA IV: {}", mountType.name().toLowerCase(Locale.ROOT), CONFIG_KEY,
			file.getFileName(), showVehicleMount);
	}
}
