package dev.libertycraft.world;

import com.mojang.brigadier.arguments.StringArgumentType;
import com.mojang.brigadier.context.CommandContext;
import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import dev.libertycraft.world.city.BlockyCity;
import dev.libertycraft.world.city.CityCells;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.OptionalDouble;
import java.util.Properties;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.permissions.Permissions;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.animal.Animal;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.level.storage.LevelResource;
import net.minecraft.world.phys.AABB;

/**
 * LibertyCraft's own mob spawning in the mirror world ({@code /libertycraft mobs on|off}, saved with
 * the world, off by default). Minecraft's natural spawning finds no ground there (it is a void over
 * GTA IV's collision), so this spawner stands mobs on GTA IV's ground by {@link SpawnRules}: at night by
 * GTA IV's clock hostile mobs (vanilla's overworld weights) 24 to 64 blocks from the player, outdoors
 * at about the player's height, up to 24 around them; by day now and then a few farm animals on GTA IV's
 * grass, up to 8. Minecraft then treats them as its own: they despawn far away as vanilla's do, undead
 * burn by day (Minecraft's day time follows GTA IV's clock while the link is up: {@link HostSky}),
 * they hunt GTA IV's peds and get run over.
 *
 * <p>Everything it spawns carries the entity tag {@link #TAG}. Turning it off discards those quietly
 * (no drops, no experience) in every loaded chunk, and any that load later while it is off; mobs from
 * spawn eggs, spawners and commands are never touched.
 */
public final class MobSpawner {
	/** Entity tag of what the spawner made. */
	public static final String TAG = "libertycraft_spawned";
	private static final String FILE = "libertycraft_mobs.properties";
	private static final int EVERY = 20;          // ticks between hostile spawn attempts
	private static final int ANIMALS_EVERY = 400; // ticks between animal attempts by day
	private static final int TRIES = 6;           // spots tried per attempt
	private static final double CAP_RANGE = 128.0;

	private static volatile boolean enabled;
	private static final Link.SkyState SKY = new Link.SkyState();
	private static int spawned;

	private MobSpawner() {
	}

	public static boolean enabled() {
		return enabled;
	}

	public static void init() {
		ServerLifecycleEvents.SERVER_STARTED.register(MobSpawner::load);
		ServerLifecycleEvents.SERVER_STOPPED.register(server -> enabled = false);
		ServerTickEvents.END_SERVER_TICK.register(MobSpawner::tick);
		ServerEntityEvents.ENTITY_LOAD.register((entity, level) -> {
			if (!enabled && entity.entityTags().contains(TAG)) {
				entity.discard(); // spawned before the spawner was turned off, in a chunk that wasn't loaded then
			}
		});
		CommandRegistrationCallback.EVENT.register((dispatcher, registry, environment) -> dispatcher.register(
			Commands.literal("libertycraft").then(Commands.literal("mobs").requires(MobSpawner::mayUse).executes(MobSpawner::status)
				.then(Commands.argument("state", StringArgumentType.word()).suggests((c, b) -> b.suggest("on").suggest("off").buildFuture())
					.executes(MobSpawner::command)))));
	}

	/** Operators, and in singleplayer the world's owner even without cheats. */
	private static boolean mayUse(CommandSourceStack source) {
		if (source.permissions().hasPermission(Permissions.COMMANDS_GAMEMASTER)) {
			return true;
		}
		ServerPlayer player = source.getPlayer();
		return player != null && source.getServer().isSingleplayerOwner(player.nameAndId());
	}

	private static int status(CommandContext<CommandSourceStack> c) {
		c.getSource().sendSuccess(() -> Component.literal("LibertyCraft mob spawning is " + (enabled ? "on" : "off")
			+ " (/libertycraft mobs on|off): hostile mobs at night by GTA IV's clock, animals on grass by day."), false);
		return enabled ? 1 : 0;
	}

	private static int command(CommandContext<CommandSourceStack> c) {
		String state = StringArgumentType.getString(c, "state");
		if (!state.equals("on") && !state.equals("off")) {
			c.getSource().sendFailure(Component.literal("Use /libertycraft mobs on or /libertycraft mobs off"));
			return 0;
		}
		MinecraftServer server = c.getSource().getServer();
		boolean on = state.equals("on");
		enabled = on;
		save(server);
		int removed = on ? 0 : removeSpawned(server);
		LibertyCraft.LOG.info("[LibertyCraft] mob spawning {}{}", on ? "on" : "off", on ? "" : " (" + removed + " spawned mobs removed)");
		c.getSource().sendSuccess(() -> Component.literal(on ? "LibertyCraft mob spawning on: hostile mobs at night by GTA IV's clock, animals on grass by day."
			: "LibertyCraft mob spawning off; " + removed + " mobs it had spawned were removed."), true);
		return 1;
	}

	/** Discards every loaded mob the spawner made (and what rides them). */
	static int removeSpawned(MinecraftServer server) {
		List<Entity> doomed = new ArrayList<>();
		for (ServerLevel level : server.getAllLevels()) {
			for (Entity e : level.getAllEntities()) {
				if (e.entityTags().contains(TAG)) {
					doomed.add(e);
				}
			}
		}
		doomed.forEach(Entity::discard);
		return doomed.size();
	}

	private static Path file(MinecraftServer server) {
		return server.getWorldPath(LevelResource.ROOT).resolve(FILE);
	}

	private static void load(MinecraftServer server) {
		Properties p = new Properties();
		Path f = file(server);
		if (Files.isRegularFile(f)) {
			try (var in = Files.newBufferedReader(f)) {
				p.load(in);
			} catch (IOException e) {
				LibertyCraft.LOG.warn("[LibertyCraft] couldn't read {}", f, e);
			}
		}
		enabled = Boolean.parseBoolean(p.getProperty("spawning", "false"));
		LibertyCraft.LOG.info("[LibertyCraft] mob spawning is {} in this world (/libertycraft mobs on|off)", enabled ? "on" : "off");
	}

	private static void save(MinecraftServer server) {
		Path f = file(server);
		try {
			Files.writeString(f, "# LibertyCraft: /libertycraft mobs on|off\nspawning=" + enabled + "\n");
		} catch (IOException e) {
			LibertyCraft.LOG.warn("[LibertyCraft] couldn't write {}", f, e);
		}
	}

	private static void tick(MinecraftServer server) {
		if (!Link.active() || server.getTickCount() % EVERY != 0) {
			return;
		}
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (players.isEmpty() || !Link.readSkyState(SKY) || !SKY.inGame() || SKY.loading()) {
			return;
		}
		ServerPlayer player = players.getFirst();
		ServerLevel level = player.level();
		if (!BlockyCity.isMirror(level)) {
			return;
		}
		if (!enabled || !HostCollision.active() || player.isSpectator()) {
			return;
		}
		boolean night = SpawnRules.isNight(SKY.gameHour);
		if (night) {
			spawnPack(level, player, SpawnRules.HOSTILES, SpawnRules.HOSTILE_CAP, true);
		} else if (server.getTickCount() % ANIMALS_EVERY == 0) {
			spawnPack(level, player, SpawnRules.ANIMALS, SpawnRules.PASSIVE_CAP, false);
		}
	}

	private static final SpawnRules.Ground GROUND = new SpawnRules.Ground() {
		private final BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();

		@Override
		public boolean known(int x, int y, int z) {
			return HostCollision.isKnown(x, y, z);
		}

		@Override
		public boolean geometry(int x, int y, int z) {
			return HostCollision.hasGeometry(this.pos.set(x, y, z));
		}

		@Override
		public float top(int x, int y, int z) {
			return HostCollision.groundTop(this.pos.set(x, y, z));
		}
	};

	private static void spawnPack(ServerLevel level, ServerPlayer player, List<SpawnRules.Kind> kinds, int cap, boolean hostile) {
		AABB around = player.getBoundingBox().inflate(CAP_RANGE);
		int count = level.getEntitiesOfClass(Mob.class, around, m -> m.isAlive() && (hostile ? m instanceof Enemy : m instanceof Animal)).size();
		if (count >= cap) {
			return;
		}
		RandomSource random = level.getRandom();
		SpawnRules.Kind kind = SpawnRules.pick(kinds, random.nextInt(SpawnRules.totalWeight(kinds)));
		EntityType<?> type = BuiltInRegistries.ENTITY_TYPE.getValue(Identifier.parse(kind.id()));
		if (type == null) {
			return;
		}
		for (int attempt = 0; attempt < TRIES; attempt++) {
			double angle = random.nextDouble() * Math.PI * 2.0;
			double dist = SpawnRules.MIN_DISTANCE + random.nextDouble() * (SpawnRules.MAX_DISTANCE - SpawnRules.MIN_DISTANCE);
			int x = (int) Math.floor(player.getX() + Math.cos(angle) * dist), z = (int) Math.floor(player.getZ() + Math.sin(angle) * dist);
			OptionalDouble y = SpawnRules.surface(GROUND, x, z, player.getY(), type.getHeight());
			if (y.isEmpty() || !farFromPlayers(level, x + 0.5, z + 0.5) || (!hostile && !onGrass(x + 0.5, y.getAsDouble(), z + 0.5))) {
				continue;
			}
			int n = Math.min(SpawnRules.packSize(kind, random.nextInt()), cap - count);
			int made = 0;
			for (int i = 0; i < n * 3 && made < n; i++) {
				int px = i == 0 ? x : x + random.nextInt(5) - 2, pz = i == 0 ? z : z + random.nextInt(5) - 2;
				OptionalDouble py = i == 0 ? y : SpawnRules.surface(GROUND, px, pz, player.getY(), type.getHeight());
				if (py.isPresent() && farFromPlayers(level, px + 0.5, pz + 0.5) && spawn(level, type, px + 0.5, py.getAsDouble(), pz + 0.5)) {
					made++;
				}
			}
			if (made > 0) {
				spawned += made;
				LibertyCraft.LOG.info("[LibertyCraft] spawned {} {} at {} {} {} ({} blocks from the player; {} {} around, {} spawned this session)", made,
					kind.id().substring(kind.id().indexOf(':') + 1), x, String.format("%.2f", y.getAsDouble()), z, (int) Math.round(Math.hypot(x - player.getX(), z - player.getZ())),
					count + made, hostile ? "hostiles" : "animals", spawned);
			}
			return;
		}
	}

	private static boolean farFromPlayers(ServerLevel level, double x, double z) {
		for (ServerPlayer p : level.players()) {
			double dx = p.getX() - x, dz = p.getZ() - z;
			if (dx * dx + dz * dz < SpawnRules.MIN_DISTANCE * SpawnRules.MIN_DISTANCE) {
				return false;
			}
		}
		return true;
	}

	/** GTA IV's surface under (x, y, z) is grass (its floor triangle's material). */
	private static boolean onGrass(double x, double y, double z) {
		List<HostTri> tris = new ArrayList<>();
		HostCollision.trianglesNear(new AABB(x - 0.3, y - 0.6, z - 0.3, x + 0.3, y + 0.4, z + 0.3), tris);
		for (HostTri t : tris) {
			if (t.walkable && t.gtaMaterial >= 0 && SpawnRules.isGrass(CityCells.materialName(t.gtaMaterial))) {
				return true;
			}
		}
		return false;
	}

	private static boolean spawn(ServerLevel level, EntityType<?> type, double x, double y, double z) {
		if (!level.noCollision(type.getSpawnAABB(x, y, z))) {
			return false;
		}
		Entity e = type.create(level, EntitySpawnReason.NATURAL);
		if (!(e instanceof Mob mob)) {
			return false;
		}
		mob.snapTo(x, y, z, level.getRandom().nextFloat() * 360.0F, 0.0F);
		mob.finalizeSpawn(level, level.getCurrentDifficultyAt(mob.blockPosition()), EntitySpawnReason.NATURAL, null);
		mob.addTag(TAG);
		for (Entity rider : mob.getIndirectPassengers()) {
			rider.addTag(TAG); // a spider jockey's skeleton
		}
		return level.tryAddFreshEntityWithPassengers(mob);
	}
}
