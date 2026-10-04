package dev.libertycraft.world;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.OptionalDouble;
import java.util.Set;
import org.junit.jupiter.api.Test;

class SpawnRulesTest {
	/** A made-up stretch of GTA IV's collision: cells with how high their geometry reaches, all known unless listed. */
	private static final class FakeGround implements SpawnRules.Ground {
		final Map<Long, Float> tops = new HashMap<>();
		final Set<Long> unknown = new HashSet<>();

		static long key(int x, int y, int z) {
			return ((long) x & 0xFFFFF) << 40 | ((long) y & 0xFFFFF) << 20 | ((long) z & 0xFFFFF);
		}

		FakeGround fill(int x, int y, int z, float top) {
			this.tops.put(key(x, y, z), top);
			return this;
		}

		/** A flat street: the cell under y = level is full, so you stand on level. */
		FakeGround street(int x, int z, int level) {
			return this.fill(x, level - 1, z, 1.0F);
		}

		@Override
		public boolean known(int x, int y, int z) {
			return !this.unknown.contains(key(x, y, z));
		}

		@Override
		public boolean geometry(int x, int y, int z) {
			return this.tops.containsKey(key(x, y, z));
		}

		@Override
		public float top(int x, int y, int z) {
			return this.tops.getOrDefault(key(x, y, z), 0.0F);
		}
	}

	@Test
	void nightIsGtaIvsDarkHours() {
		assertTrue(SpawnRules.isNight(0.0F));
		assertTrue(SpawnRules.isNight(23.5F));
		assertTrue(SpawnRules.isNight(20.0F));
		assertTrue(SpawnRules.isNight(4.9F));
		assertFalse(SpawnRules.isNight(5.0F));
		assertFalse(SpawnRules.isNight(12.0F));
		assertFalse(SpawnRules.isNight(19.9F));
		assertTrue(SpawnRules.isNight(24.5F)); // (wraps)
	}

	@Test
	void minecraftsClockFollowsGtaIvsHour() {
		assertEquals(0L, SpawnRules.dayTimeOf(6.0F));    // sunrise
		assertEquals(6000L, SpawnRules.dayTimeOf(12.0F)); // noon
		assertEquals(18000L, SpawnRules.dayTimeOf(0.0F)); // midnight
		assertEquals(14000L, SpawnRules.dayTimeOf(20.0F));
		// Within 100 ticks: left alone; else the short way round, never a whole day.
		assertEquals(0L, SpawnRules.clockCorrection(5 * 24000L + 6050L, 12.0F));
		assertEquals(-500L, SpawnRules.clockCorrection(5 * 24000L + 6500L, 12.0F));
		assertEquals(1000L, SpawnRules.clockCorrection(5 * 24000L + 5000L, 12.0F));
		assertEquals(-200L, SpawnRules.clockCorrection(5 * 24000L + 100L, 5.9F)); // 05:54 is 23900: back across the day's 0
	}

	@Test
	void picksKindsByVanillasWeights() {
		int total = SpawnRules.totalWeight(SpawnRules.HOSTILES);
		assertEquals(95 + 100 + 100 + 100 + 10 + 5, total);
		assertEquals("minecraft:zombie", SpawnRules.pick(SpawnRules.HOSTILES, 0).id());
		assertEquals("minecraft:zombie", SpawnRules.pick(SpawnRules.HOSTILES, 94).id());
		assertEquals("minecraft:skeleton", SpawnRules.pick(SpawnRules.HOSTILES, 95).id());
		assertEquals("minecraft:witch", SpawnRules.pick(SpawnRules.HOSTILES, total - 1).id());
		Map<String, Integer> seen = new HashMap<>();
		for (int r = 0; r < total; r++) {
			seen.merge(SpawnRules.pick(SpawnRules.HOSTILES, r).id(), 1, Integer::sum);
		}
		assertEquals(10, seen.get("minecraft:enderman"));
		assertEquals(5, seen.get("minecraft:witch"));
		for (int roll = -50; roll < 50; roll++) {
			for (SpawnRules.Kind k : SpawnRules.HOSTILES) {
				int n = SpawnRules.packSize(k, roll);
				assertTrue(n >= k.minPack() && n <= k.maxPack());
			}
		}
	}

	@Test
	void theBandIsVanillas() {
		assertFalse(SpawnRules.inBand(10.0, 10.0));
		assertTrue(SpawnRules.inBand(24.0, 0.0));
		assertTrue(SpawnRules.inBand(30.0, 30.0));
		assertFalse(SpawnRules.inBand(64.0, 1.0));
	}

	@Test
	void standsOnTheStreetAtThePlayersHeight() {
		FakeGround g = new FakeGround().street(5, 7, 31);
		OptionalDouble y = SpawnRules.surface(g, 5, 7, 31.0, 1.95);
		assertTrue(y.isPresent());
		assertEquals(31.0, y.getAsDouble(), 1.0E-9);
		// Ground low in its cell (a kerb's top at 31.375): it stands on that.
		FakeGround low = new FakeGround().street(5, 7, 31).fill(5, 31, 7, 0.375F);
		assertEquals(31.375, SpawnRules.surface(low, 5, 7, 31.2, 1.95).getAsDouble(), 1.0E-6);
		// Ground high in its cell (0.75): it stands on its top.
		FakeGround high = new FakeGround().fill(5, 30, 7, 0.75F);
		assertEquals(30.75, SpawnRules.surface(high, 5, 7, 31.0, 1.95).getAsDouble(), 1.0E-6);
	}

	@Test
	void notIndoorsUnderBridgesOrOnRoofs() {
		// A ceiling 3 blocks over the street: indoors.
		FakeGround indoors = new FakeGround().street(0, 0, 31).fill(0, 34, 0, 0.2F);
		assertTrue(SpawnRules.surface(indoors, 0, 0, 31.0, 1.95).isEmpty());
		// A bridge 10 blocks up: under it, no.
		FakeGround bridge = new FakeGround().street(0, 0, 31).fill(0, 41, 0, 1.0F);
		assertTrue(SpawnRules.surface(bridge, 0, 0, 31.0, 1.95).isEmpty());
		// A roof 12 blocks above the player and nothing at street level: out of reach.
		FakeGround roof = new FakeGround().fill(0, 42, 0, 1.0F);
		assertTrue(SpawnRules.surface(roof, 0, 0, 31.0, 1.95).isEmpty());
		// A flat roof 3 blocks up over a room at street level: neither (the street is covered, the roof isn't the street).
		FakeGround house = new FakeGround().street(0, 0, 31).fill(0, 34, 0, 1.0F);
		assertTrue(SpawnRules.surface(house, 0, 0, 31.0, 1.95).isEmpty());
		// No ground at all (the void), or GTA IV hasn't described it: nothing.
		assertTrue(SpawnRules.surface(new FakeGround(), 0, 0, 31.0, 1.95).isEmpty());
		FakeGround unknown = new FakeGround().street(0, 0, 31);
		unknown.unknown.add(FakeGround.key(0, 35, 0));
		assertTrue(SpawnRules.surface(unknown, 0, 0, 31.0, 1.95).isEmpty());
	}

	@Test
	void aLowWallsTopIsGround() {
		// A 2-block wall where the street would be: it stands on the wall's top (open above it).
		FakeGround wall = new FakeGround().street(0, 0, 31).fill(0, 31, 0, 1.0F).fill(0, 32, 0, 1.0F);
		assertEquals(33.0, SpawnRules.surface(wall, 0, 0, 31.0, 1.95).getAsDouble(), 1.0E-9);
	}

	@Test
	void grassIsGtaIvsGrass() {
		assertTrue(SpawnRules.isGrass("GRASS"));
		assertTrue(SpawnRules.isGrass("GRASS_LONG"));
		assertTrue(SpawnRules.isGrass("SHORT_GRASS"));
		assertFalse(SpawnRules.isGrass("TARMAC"));
		assertFalse(SpawnRules.isGrass("PAVING_SLABS"));
	}
}
