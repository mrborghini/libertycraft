package dev.libertycraft.world.city;

import java.util.LinkedHashMap;
import java.util.Map;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import org.jspecify.annotations.Nullable;

/**
 * What the blocky city's generator puts at a block (CityChunkGenerator.plan), per chunk, for telling
 * the city's own blocks from what the player built there: the world exporter leaves the city's own
 * blocks out of what GTA IV's peds and vehicles collide with (GTA IV's own collision is that same city).
 * Plans are kept for the most recent chunks and redone when the store has newer data for them.
 */
public final class CityPlanCache {
	private static final int MAX_CHUNKS = 128;

	/** One chunk's plan: each column's ids from its y0 (null: air), and its barrier run. */
	public static final class Plan {
		final long revision;
		final String[][] ids = new String[256][];
		final int[] y0 = new int[256];
		final int[] barrierFrom = new int[256];
		final int[] barrierTo = new int[256];

		Plan(long revision) {
			this.revision = revision;
			java.util.Arrays.fill(this.barrierFrom, Integer.MAX_VALUE);
			java.util.Arrays.fill(this.barrierTo, Integer.MIN_VALUE);
		}

		/** The block the city has at (x, y, z) (in this chunk): null for air. */
		public @Nullable BlockState planned(int x, int y, int z) {
			int c = (z & 15) * 16 + (x & 15);
			String[] col = this.ids[c];
			if (col != null && y >= this.y0[c] && y < this.y0[c] + col.length && col[y - this.y0[c]] != null) {
				return CityChunkGenerator.state(col[y - this.y0[c]]);
			}
			return y >= this.barrierFrom[c] && y <= this.barrierTo[c] ? Blocks.BARRIER.defaultBlockState() : null;
		}

		/** The block there is the city's own (as generated), not something the player put there. */
		public boolean isCityBlock(int x, int y, int z, BlockState state) {
			BlockState planned = planned(x, y, z);
			return planned != null && (planned == state || planned.is(Blocks.WATER) && state.is(Blocks.WATER));
		}
	}

	private static final Map<Long, Plan> PLANS = new LinkedHashMap<>(MAX_CHUNKS, 0.75F, true) {
		@Override
		protected boolean removeEldestEntry(Map.Entry<Long, Plan> eldest) {
			return size() > MAX_CHUNKS;
		}
	};

	private CityPlanCache() {
	}

	/** Chunk (cx, cz)'s plan as the store has it now, or null without a store. */
	public static synchronized @Nullable Plan chunk(int cx, int cz, int minY, int maxY) {
		CityStore store = CityStore.current();
		if (store == null) {
			return null;
		}
		long key = (long) cx << 32 | (cz & 0xFFFFFFFFL);
		long rev = store.chunkSourceRevision(cx, cz);
		Plan plan = PLANS.get(key);
		if (plan != null && plan.revision == rev) {
			return plan;
		}
		Plan fresh = new Plan(rev);
		CityChunkGenerator.plan(store, cx, cz, minY, maxY, new CityChunkGenerator.Sink() {
			@Override
			public void column(int x, int z, int y0, String[] ids) {
				fresh.ids[z * 16 + x] = ids;
				fresh.y0[z * 16 + x] = y0;
			}

			@Override
			public void barrier(int x, int z, int yFrom, int yTo) {
				int c = z * 16 + x;
				fresh.barrierFrom[c] = Math.min(fresh.barrierFrom[c], yFrom);
				fresh.barrierTo[c] = Math.max(fresh.barrierTo[c], yTo);
			}
		});
		PLANS.put(key, fresh);
		return fresh;
	}

	/** Drops every plan (a new world). */
	public static synchronized void clear() {
		PLANS.clear();
	}
}
