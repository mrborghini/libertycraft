package dev.libertycraft.world.city;

import com.mojang.serialization.MapCodec;
import com.mojang.serialization.codecs.RecordCodecBuilder;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.WorldGenRegion;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.NoiseColumn;
import net.minecraft.world.level.StructureManager;
import net.minecraft.world.level.WorldGenLevel;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.BiomeSource;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.LeavesBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.chunk.ChunkGeneratorStructureState;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructureTemplateManager;
import org.jspecify.annotations.Nullable;

/**
 * The chunk generator of {@code libertycraft:blocky_city}: Liberty City as far as GTA IV has shown it
 * (CityStore), as Minecraft blocks, at the same coordinates as GTA IV (and the mirror world). Each
 * block column comes from CityPlan; where nobody has been yet there is only air, behind an invisible
 * wall of barriers along the edge of what is known, so nobody walks off into the void.
 */
public final class CityChunkGenerator extends ChunkGenerator {
	public static final MapCodec<CityChunkGenerator> CODEC = RecordCodecBuilder.mapCodec(
		i -> i.group(BiomeSource.CODEC.fieldOf("biome_source").forGetter(g -> g.biomeSource)).apply(i, CityChunkGenerator::new));

	/** Matches data/libertycraft/dimension_type/blocky_city.json. */
	public static final int MIN_Y = -64;
	public static final int HEIGHT = 512;
	/** The barrier wall reaches this far above and below what is known next to it. */
	private static final int BARRIER_MARGIN = 12;

	private static final ConcurrentHashMap<String, BlockState> STATES = new ConcurrentHashMap<>();

	public CityChunkGenerator(BiomeSource biomeSource) {
		super(biomeSource);
	}

	@Override
	protected MapCodec<? extends ChunkGenerator> codec() {
		return CODEC;
	}

	/** Receives a chunk's planned blocks: a column (local x, z; ids bottom up from y0, null air) or a barrier run. */
	public interface Sink {
		void column(int x, int z, int y0, String[] ids);

		void barrier(int x, int z, int yFrom, int yTo);
	}

	/**
	 * Plans chunk (cx, cz) from the store. Returns the store revision it was planned from (0: nothing
	 * known in or next to it).
	 */
	public static long plan(CityStore store, int cx, int cz, int minY, int maxY, Sink sink) {
		long rev = store.chunkSourceRevision(cx, cz);
		int ryMin = Integer.MAX_VALUE, ryMax = Integer.MIN_VALUE;
		int[][] ranges = new int[4][];
		for (int i = 0; i < 4; i++) {
			int[] r = store.columnRange(cx * 2 + (i & 1), cz * 2 + (i >> 1));
			ranges[i] = r;
			if (r != null) {
				ryMin = Math.min(ryMin, r[0]);
				ryMax = Math.max(ryMax, r[1]);
			}
		}
		if (ryMin <= ryMax) {
			planColumns(store, cx, cz, ryMin, ryMax, minY, maxY, sink);
		}
		// The edge of the known city: barriers along the side of an unknown region column that faces a known one.
		for (int i = 0; i < 4; i++) {
			if (ranges[i] != null) {
				continue;
			}
			int rx = cx * 2 + (i & 1), rz = cz * 2 + (i >> 1);
			int lx0 = (i & 1) * 8, lz0 = (i >> 1) * 8;
			int[][] sides = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
			for (int[] s : sides) {
				int[] n = store.columnRange(rx + s[0], rz + s[1]);
				if (n == null) {
					continue;
				}
				int yFrom = Math.max(minY, n[0] * 8 - BARRIER_MARGIN), yTo = Math.min(maxY, n[1] * 8 + 7 + BARRIER_MARGIN);
				for (int k = 0; k < 8; k++) {
					int x = s[0] == 0 ? lx0 + k : s[0] < 0 ? lx0 : lx0 + 7;
					int z = s[1] == 0 ? lz0 + k : s[1] < 0 ? lz0 : lz0 + 7;
					sink.barrier(x, z, yFrom, yTo);
				}
			}
		}
		return rev;
	}

	private static void planColumns(CityStore store, int cx, int cz, int ryMin, int ryMax, int minY, int maxY, Sink sink) {
		int rows = ryMax - ryMin + 1;
		long[][][] regions = new long[4][rows][];
		for (int i = 0; i < 4; i++) {
			for (int r = 0; r < rows; r++) {
				long[] cells = new long[512];
				if (store.region(cx * 2 + (i & 1), ryMin + r, cz * 2 + (i >> 1), cells)) {
					regions[i][r] = cells;
				}
			}
		}
		int dataLo = ryMin * 8 - 1, dataHi = ryMax * 8 + 7;
		int bx0 = cx * 16, bz0 = cz * 16;
		for (int z = 0; z < 16; z++) {
			for (int x = 0; x < 16; x++) {
				double water = store.waterAt(bx0 + x, bz0 + z);
				int yLo = dataLo, yHi = dataHi;
				if (!Double.isNaN(water)) {
					yLo = Math.min(yLo, (int) Math.floor(water) - CityPlan.MAX_WATER_DEPTH - 1);
					yHi = Math.max(yHi, (int) Math.ceil(water));
				}
				yLo = Math.max(yLo, minY);
				yHi = Math.min(yHi, maxY);
				if (yLo > yHi) {
					continue;
				}
				long[] cells = new long[yHi - yLo + 1];
				long[][] mine = regions[(x >> 3) + (z >> 3) * 2];
				int cell = (x & 7) + (z & 7) * 8;
				for (int y = Math.max(yLo, ryMin * 8); y <= Math.min(yHi, dataHi); y++) {
					long[] r = mine[(y >> 3) - ryMin];
					if (r != null) {
						cells[y - yLo] = r[cell + (y & 7) * 64];
					}
				}
				sink.column(x, z, yLo, CityPlan.column(cells, yLo, water));
			}
		}
	}

	/** A block id from CityCells/CityPlan as a block state (leaves never decay; unknown ids: stone). */
	public static BlockState state(String id) {
		return STATES.computeIfAbsent(id, key -> {
			Block block = BuiltInRegistries.BLOCK.getOptional(Identifier.parse(key)).orElse(Blocks.STONE);
			BlockState state = block.defaultBlockState();
			if (block instanceof LeavesBlock) {
				state = state.setValue(LeavesBlock.PERSISTENT, true);
			}
			return state;
		});
	}

	@Override
	public CompletableFuture<ChunkAccess> buildTerrain(ChunkAccess chunk, Blender blender, RandomState randomState, StructureManager structureManager,
		BiomeManager biomeManager, WorldGenRegion region, Set<Holder<Biome>> biomes) {
		CityStore store = CityStore.current();
		if (store == null) {
			return CompletableFuture.completedFuture(chunk);
		}
		int cx = chunk.getPos().x(), cz = chunk.getPos().z();
		Heightmap ocean = chunk.getOrCreateHeightmapUnprimed(Heightmap.Types.OCEAN_FLOOR_WG);
		Heightmap surface = chunk.getOrCreateHeightmapUnprimed(Heightmap.Types.WORLD_SURFACE_WG);
		BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
		int minY = chunk.getMinY(), maxY = chunk.getMaxY();
		BlockState barrier = Blocks.BARRIER.defaultBlockState();
		long rev = plan(store, cx, cz, minY, maxY, new Sink() {
			@Override
			public void column(int x, int z, int y0, String[] ids) {
				for (int i = 0; i < ids.length; i++) {
					if (ids[i] == null) {
						continue;
					}
					BlockState state = state(ids[i]);
					chunk.setBlockState(pos.set(x, y0 + i, z), state, 0);
					ocean.update(x, y0 + i, z, state);
					surface.update(x, y0 + i, z, state);
				}
			}

			@Override
			public void barrier(int x, int z, int yFrom, int yTo) {
				for (int y = yFrom; y <= yTo; y++) {
					if (chunk.getBlockState(pos.set(x, y, z)).isAir()) {
						chunk.setBlockState(pos, barrier, 0);
						surface.update(x, y, z, barrier);
					}
				}
			}
		});
		store.setChunkApplied(cx, cz, rev);
		return CompletableFuture.completedFuture(chunk);
	}

	@Override
	public void applyBiomeDecoration(WorldGenLevel level, ChunkAccess chunk, StructureManager structureManager) {
		// Nothing grows here: the city is GTA IV's.
	}

	@Override
	public void createStructures(RegistryAccess registryAccess, ChunkGeneratorStructureState state, StructureManager structureManager, ChunkAccess chunk,
		StructureTemplateManager templates, ResourceKey<Level> level) {
	}

	@Override
	public void createReferences(WorldGenLevel level, StructureManager structureManager, ChunkAccess chunk) {
	}

	@Override
	public void spawnOriginalMobs(WorldGenRegion region) {
	}

	@Override
	public int getGenDepth() {
		return HEIGHT;
	}

	@Override
	public int getSeaLevel() {
		return 0;
	}

	@Override
	public int getMinY() {
		return MIN_Y;
	}

	@Override
	public int getBaseHeight(int x, int z, Heightmap.Types type, LevelHeightAccessor level, RandomState randomState) {
		BlockState[] column = baseColumn(x, z, level);
		for (int i = column.length - 1; i >= 0; i--) {
			if (type.isOpaque().test(column[i])) {
				return level.getMinY() + i + 1;
			}
		}
		return level.getMinY();
	}

	@Override
	public NoiseColumn getBaseColumn(int x, int z, LevelHeightAccessor level, RandomState randomState) {
		return new NoiseColumn(level.getMinY(), baseColumn(x, z, level));
	}

	private static BlockState[] baseColumn(int x, int z, LevelHeightAccessor level) {
		BlockState[] out = new BlockState[level.getHeight()];
		java.util.Arrays.fill(out, Blocks.AIR.defaultBlockState());
		CityStore store = CityStore.current();
		if (store == null) {
			return out;
		}
		int cx = Math.floorDiv(x, 16), cz = Math.floorDiv(z, 16), lx = Math.floorMod(x, 16), lz = Math.floorMod(z, 16);
		int minY = level.getMinY();
		plan(store, cx, cz, minY, level.getMaxY(), new Sink() {
			@Override
			public void column(int cxl, int czl, int y0, String[] ids) {
				if (cxl != lx || czl != lz) {
					return;
				}
				for (int i = 0; i < ids.length; i++) {
					if (ids[i] != null && y0 + i - minY >= 0 && y0 + i - minY < out.length) {
						out[y0 + i - minY] = state(ids[i]);
					}
				}
			}

			@Override
			public void barrier(int bx, int bz, int yFrom, int yTo) {
			}
		});
		return out;
	}

	@Override
	public void addDebugScreenInfo(List<String> info, RandomState randomState, BlockPos pos, @Nullable SamplerContext context) {
		CityStore store = CityStore.current();
		info.add("LibertyCraft blocky city" + (store != null ? " (" + store.stats() + ")" : ""));
	}
}
