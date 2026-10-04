package dev.libertycraft.world.city;

import dev.libertycraft.LibertyCraft;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.level.WorldGenRegion;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.dimension.DimensionType;
import net.minecraft.world.level.storage.LevelResource;
import org.jspecify.annotations.Nullable;

/**
 * "Blocky Liberty City": a nether portal lit in the mirror world (the void Minecraft world under GTA IV)
 * leads to {@code libertycraft:blocky_city}, a Minecraft dimension that is a block copy of Liberty City
 * built from the collision GTA IV has streamed so far (CityStore, CityChunkGenerator), at the same X/Z
 * (1:1, not 1:8); a portal there leads back. Everything stays in GTA IV: Minecraft streams the city's
 * blocks like any others, puppet mode goes on, and GTA IV hides its own map geometry while the player
 * is there (kMcBlockyCity; client side: CityClient), so the blocks are the city around Niko.
 *
 * <p>What makes GTA IV's geometry act like Minecraft's (HostCollision's shapes, HostWater, digging, the
 * smooth collider) stays in the mirror world, {@link #isCity} gates it: the city's blocks are real.
 */
public final class BlockyCity {
	public static final ResourceKey<Level> CITY = ResourceKey.create(Registries.DIMENSION, Identifier.fromNamespaceAndPath(LibertyCraft.MOD_ID, "blocky_city"));
	public static final ResourceKey<DimensionType> MIRROR_TYPE =
		ResourceKey.create(Registries.DIMENSION_TYPE, Identifier.fromNamespaceAndPath(LibertyCraft.MOD_ID, "mirror"));
	private static final int REFRESH_EVERY_TICKS = 20;
	private static final int REFRESH_RADIUS = 6;       // chunks around each player in the city
	private static final int REFRESH_PER_ROUND = 2;    // chunks rebuilt per round at most
	private static final int REFRESH_MARGIN = 12;      // blocks above and below the known range swept for stale blocks

	/** config/libertycraft.properties {@code blockyCity}: nether portals lead to the blocky city (default true). */
	public static volatile boolean enabled = true;

	private static int tickCounter;

	private BlockyCity() {
	}

	public static void init() {
		Registry.register(BuiltInRegistries.CHUNK_GENERATOR, Identifier.fromNamespaceAndPath(LibertyCraft.MOD_ID, "blocky_city"), CityChunkGenerator.CODEC);
		loadConfig();
		ServerLifecycleEvents.SERVER_STARTING.register(server -> {
			CityStore.open(server.getWorldPath(LevelResource.ROOT).resolve("libertycraft_city"));
		});
		ServerLifecycleEvents.SERVER_STARTED.register(server -> {
			ServerLevel city = server.getLevel(CITY);
			LibertyCraft.LOG.info("[LibertyCraft] blocky city: {}; nether portals in the mirror world {}", city != null ? "dimension present" : "dimension MISSING",
				enabled && city != null && isMirror(server.overworld()) ? "lead there (1:1)" : "work as usual");
		});
		ServerLifecycleEvents.SERVER_STOPPED.register(server -> {
			CityStore.close();
			CityPlanCache.clear();
		});
		ServerTickEvents.END_SERVER_TICK.register(BlockyCity::tick);
		CityRecorder.startFlushing();
	}

	public static boolean isCity(@Nullable Level level) {
		return level != null && level.dimension() == CITY;
	}

	/** The block getter belongs to the blocky city (a level, or worldgen in it). Unknown getters: false. */
	public static boolean isCity(@Nullable BlockGetter getter) {
		if (getter instanceof Level level) {
			return isCity(level);
		}
		if (getter instanceof WorldGenRegion region) {
			return isCity(region.getLevel());
		}
		return false;
	}

	/** GTA IV's mirror world (the LibertyCraft world's overworld). */
	public static boolean isMirror(@Nullable Level level) {
		return level != null && level.dimension() == Level.OVERWORLD && level.dimensionTypeRegistration().is(MIRROR_TYPE);
	}

	/** Where a nether portal in {@code from} leads instead of the nether, or null (vanilla). */
	public static @Nullable ServerLevel portalTarget(MinecraftServer server, ServerLevel from) {
		if (!enabled) {
			return null;
		}
		if (isMirror(from)) {
			return server.getLevel(CITY);
		}
		if (isCity(from) && isMirror(server.overworld())) {
			return server.overworld();
		}
		return null;
	}

	/** The mirror or the city: portals between them pair up 1:1 and stand on the ground they lead to. */
	public static boolean isPortalPair(@Nullable Level level) {
		return enabled && (isMirror(level) || isCity(level));
	}

	// ---- keeping built chunks up to date ------------------------------------------------------------

	private static void tick(MinecraftServer server) {
		if (++tickCounter % REFRESH_EVERY_TICKS != 0) {
			return;
		}
		ServerLevel city = server.getLevel(CITY);
		CityStore store = CityStore.current();
		if (city == null || store == null || city.players().isEmpty()) {
			return;
		}
		int budget = REFRESH_PER_ROUND;
		for (ServerPlayer player : city.players()) {
			int pcx = player.getBlockX() >> 4, pcz = player.getBlockZ() >> 4;
			for (int r = 0; r <= REFRESH_RADIUS && budget > 0; r++) {
				for (int dx = -r; dx <= r && budget > 0; dx++) {
					for (int dz = -r; dz <= r && budget > 0; dz++) {
						if (Math.max(Math.abs(dx), Math.abs(dz)) != r) {
							continue;
						}
						int cx = pcx + dx, cz = pcz + dz;
						LevelChunk chunk = city.getChunkSource().getChunkNow(cx, cz);
						if (chunk == null) {
							continue;
						}
						long source = store.chunkSourceRevision(cx, cz);
						long applied = store.chunkApplied(cx, cz);
						if (source <= applied) {
							continue;
						}
						rebuild(city, store, cx, cz, applied);
						budget--;
					}
				}
			}
		}
	}

	/**
	 * Brings a built chunk up to what the store says now, as far as that can't undo a player's work:
	 * air gets what is new there, barriers go where the city is known now, water may give way to
	 * ground; a block that is there stays (it may be the player's: measured, a rebuild took away a
	 * glass block placed in the city, and the player standing on it fell). Blocks that newer data no
	 * longer has stay until the chunk is built again (a new world).
	 */
	private static void rebuild(ServerLevel city, CityStore store, int cx, int cz, long applied) {
		Map<Long, BlockState> wanted = new HashMap<>();
		int[] span = { Integer.MAX_VALUE, Integer.MIN_VALUE };
		int minY = city.getMinY(), maxY = city.getMaxY();
		BlockState barrier = Blocks.BARRIER.defaultBlockState();
		long rev = CityChunkGenerator.plan(store, cx, cz, minY, maxY, new CityChunkGenerator.Sink() {
			@Override
			public void column(int x, int z, int y0, String[] ids) {
				for (int i = 0; i < ids.length; i++) {
					if (ids[i] != null) {
						wanted.put(BlockPos.asLong(cx * 16 + x, y0 + i, cz * 16 + z), CityChunkGenerator.state(ids[i]));
					}
				}
				span[0] = Math.min(span[0], y0);
				span[1] = Math.max(span[1], y0 + ids.length - 1);
			}

			@Override
			public void barrier(int x, int z, int yFrom, int yTo) {
				for (int y = yFrom; y <= yTo; y++) {
					wanted.putIfAbsent(BlockPos.asLong(cx * 16 + x, y, cz * 16 + z), barrier);
				}
				span[0] = Math.min(span[0], yFrom);
				span[1] = Math.max(span[1], yTo);
			}
		});
		// Stale city blocks can sit anywhere the neighbourhood's data (and its barriers) reached.
		for (int rx = cx * 2 - 1; rx <= cx * 2 + 2; rx++) {
			for (int rz = cz * 2 - 1; rz <= cz * 2 + 2; rz++) {
				int[] range = store.columnRange(rx, rz);
				if (range != null) {
					span[0] = Math.min(span[0], range[0] * 8 - REFRESH_MARGIN);
					span[1] = Math.max(span[1], range[1] * 8 + 7 + REFRESH_MARGIN);
				}
			}
		}
		int changed = 0;
		if (span[0] <= span[1]) {
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			BlockState air = Blocks.AIR.defaultBlockState();
			int y0 = Math.max(minY, span[0]), y1 = Math.min(maxY, span[1]);
			for (int x = cx * 16; x < cx * 16 + 16; x++) {
				for (int z = cz * 16; z < cz * 16 + 16; z++) {
					for (int y = y0; y <= y1; y++) {
						BlockState want = wanted.getOrDefault(BlockPos.asLong(x, y, z), air);
						BlockState have = city.getBlockState(pos.set(x, y, z));
						if (have == want || !mayReplace(have, want)) {
							continue;
						}
						city.setBlock(pos, want, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
						changed++;
					}
				}
			}
		}
		store.setChunkApplied(cx, cz, rev);
		if (changed > 0) {
			LibertyCraft.LOG.info("[LibertyCraft] blocky city: chunk {} {} rebuilt from newer data (revision {} -> {}): {} blocks changed", cx, cz, applied, rev, changed);
		}
	}

	/** What a rebuild may change: air, barriers, and water where ground is wanted now. */
	private static boolean mayReplace(BlockState have, BlockState want) {
		return have.isAir() || have.is(Blocks.BARRIER) || have.is(Blocks.WATER) && !want.isAir();
	}

	// ---- config/libertycraft.properties: blockyCity ---------------------------------------------------

	private static void loadConfig() {
		Path file = FabricLoader.getInstance().getConfigDir().resolve("libertycraft.properties");
		Properties props = new Properties();
		try {
			if (Files.exists(file)) {
				try (var in = Files.newBufferedReader(file)) {
					props.load(in);
				}
			}
			String on = props.getProperty("blockyCity");
			if (on == null) {
				List<String> lines = Files.exists(file) ? new ArrayList<>(Files.readAllLines(file)) : new ArrayList<>(List.of("# LibertyCraft"));
				lines.add("# A nether portal lit in GTA IV's world leads to a blocky copy of Liberty City (false: portals do nothing).");
				lines.add("blockyCity=true");
				Files.createDirectories(file.getParent());
				Files.write(file, lines);
			}
			enabled = Boolean.parseBoolean(on == null ? "true" : on.trim());
		} catch (IOException e) {
			LibertyCraft.LOG.warn("[LibertyCraft] couldn't read {}", file, e);
		}
		LibertyCraft.LOG.info("[LibertyCraft] blocky city: {} (blockyCity in {})", enabled ? "on" : "off", file.getFileName());
	}
}
