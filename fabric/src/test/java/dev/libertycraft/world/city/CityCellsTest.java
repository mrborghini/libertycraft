package dev.libertycraft.world.city;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

class CityCellsTest {
	/** Voxel layers filled from layer {@code from} to {@code to} (whole 8x8 layers). */
	private static long[] layers(int from, int to) {
		long[] l = new long[8];
		for (int y = from; y <= to; y++) {
			l[y] = -1L;
		}
		return l;
	}

	/** A vertical sheet one voxel thick at x = {@code x}, over all y and z. */
	private static long[] wallX(int x) {
		long[] l = new long[8];
		for (int y = 0; y < 8; y++) {
			for (int z = 0; z < 8; z++) {
				l[y] |= 1L << (z * 8 + x);
			}
		}
		return l;
	}

	private static int index(String name) {
		for (int i = 0; i < CityCells.GTA_MATERIALS.length; i++) {
			if (CityCells.GTA_MATERIALS[i].equals(name)) {
				return i;
			}
		}
		throw new IllegalArgumentException(name);
	}

	@Test
	void describesVoxels() {
		long d = CityCells.describe(layers(0, 2));
		assertEquals(192, CityCells.count(d));
		assertEquals(0, CityCells.bottom(d));
		assertEquals(2, CityCells.top(d));
		assertEquals(64, CityCells.projY(d));
		assertEquals(24, CityCells.projX(d)); // 3 layers x 8 rows
		assertEquals(24, CityCells.projZ(d));
		long w = CityCells.describe(wallX(3));
		assertEquals(64, CityCells.count(w));
		assertEquals(64, CityCells.projX(w));
		assertEquals(8, CityCells.projY(w));
		assertEquals(8, CityCells.projZ(w));
		assertEquals(0L, CityCells.describe(new long[8]));
	}

	@Test
	void keepsMaterials() {
		long d = CityCells.withMaterials(CityCells.describe(layers(0, 7)), 8, 14);
		assertEquals(8, CityCells.floorMaterial(d));
		assertEquals(14, CityCells.wallMaterial(d));
		assertEquals(512, CityCells.count(d));
	}

	@Test
	void groundRoundsToTheNearestBlock() {
		// The street's surface 0.3 into a block: the block under it is the top.
		assertEquals(CityCells.SOLID_BELOW, CityCells.classify(CityCells.describe(layers(0, 1))));
		assertEquals(CityCells.SOLID_BELOW, CityCells.classify(CityCells.describe(layers(0, 2))));
		// Half way or more: this block.
		assertEquals(CityCells.SOLID, CityCells.classify(CityCells.describe(layers(0, 3))));
		assertEquals(CityCells.SOLID, CityCells.classify(CityCells.describe(layers(0, 7))));
	}

	@Test
	void sheetsAndWalls() {
		// A roof sheet (one voxel) high in its block fills it; low in it, the block below.
		assertEquals(CityCells.SOLID, CityCells.classify(CityCells.describe(layers(5, 5))));
		assertEquals(CityCells.SOLID_BELOW, CityCells.classify(CityCells.describe(layers(1, 1))));
		// A wall one voxel thick is a solid block.
		assertEquals(CityCells.SOLID, CityCells.classify(CityCells.describe(wallX(0))));
		// A thin post (2x2 voxels all the way up) is not.
		long[] post = new long[8];
		for (int y = 0; y < 8; y++) {
			post[y] = 0b11L | 0b11L << 8;
		}
		assertEquals(CityCells.EMPTY, CityCells.classify(CityCells.describe(post)));
		// Half a wall (the top half of a railing) neither.
		long[] half = wallX(2);
		for (int y = 4; y < 8; y++) {
			half[y] = 0;
		}
		assertEquals(CityCells.EMPTY, CityCells.classify(CityCells.describe(half)));
	}

	@Test
	void mapsGtaMaterialsToBlocks() {
		assertEquals("minecraft:gray_concrete", CityCells.blockForMaterial(index("TARMAC"), true));
		assertEquals("minecraft:gray_concrete", CityCells.blockForMaterial(index("PAINTED_ROAD"), true));
		assertEquals("minecraft:smooth_stone", CityCells.blockForMaterial(index("CONCRETE"), true));
		assertEquals("minecraft:light_gray_concrete", CityCells.blockForMaterial(index("CONCRETE"), false));
		assertEquals("minecraft:smooth_stone", CityCells.blockForMaterial(index("PAVING_SLABS"), true));
		assertEquals("minecraft:bricks", CityCells.blockForMaterial(index("BRICK_WALL"), false));
		assertEquals("minecraft:grass_block", CityCells.blockForMaterial(index("GRASS"), true));
		assertEquals("minecraft:dirt", CityCells.blockForMaterial(index("GRASS"), false));
		assertEquals("minecraft:oak_planks", CityCells.blockForMaterial(index("WOOD_BOARD"), true));
		assertEquals("minecraft:glass", CityCells.blockForMaterial(index("GLASS_MEDIUM"), false));
		assertEquals("minecraft:iron_block", CityCells.blockForMaterial(index("SOLID_METAL_PANEL"), false));
		assertEquals("minecraft:iron_bars", CityCells.blockForMaterial(index("METAL_RAILING"), false));
		assertEquals("minecraft:sand", CityCells.blockForMaterial(index("SAND"), true));
		assertEquals("minecraft:stone_bricks", CityCells.blockForMaterial(index("DEFAULT"), false));
		assertEquals("minecraft:light_gray_concrete", CityCells.blockForMaterial(index("DEFAULT"), true));
		// No GTA material: by the kind of surface.
		assertEquals("minecraft:smooth_stone", CityCells.blockForMaterial(CityCells.MAT_TERRAIN, true));
		assertEquals("minecraft:stone_bricks", CityCells.blockForMaterial(CityCells.MAT_WALL, false));
		assertEquals("minecraft:light_gray_concrete", CityCells.blockForMaterial(CityCells.MAT_NONE, true));
		// Everything maps to a namespaced id.
		for (String id : CityCells.allBlockIds()) {
			assertTrue(id.startsWith("minecraft:"), id);
		}
		assertEquals("TARMAC", CityCells.materialName(8));
		assertEquals("GRASS", CityCells.materialName(37));
	}

	@Test
	void groundUnderTheSurface() {
		int grass = index("GRASS");
		assertEquals("minecraft:grass_block", CityCells.blockFor(grass, CityCells.MAT_NONE, CityCells.Role.TOP, 0));
		assertEquals("minecraft:dirt", CityCells.blockFor(grass, CityCells.MAT_NONE, CityCells.Role.INSIDE, 2));
		assertEquals("minecraft:stone", CityCells.blockFor(grass, CityCells.MAT_NONE, CityCells.Role.INSIDE, 5));
		assertEquals("minecraft:stone", CityCells.blockFor(index("TARMAC"), CityCells.MAT_NONE, CityCells.Role.INSIDE, 1));
		// Inside a brick wall: bricks.
		assertEquals("minecraft:bricks", CityCells.blockFor(CityCells.MAT_NONE, index("BRICK_WALL"), CityCells.Role.INSIDE, 3));
	}

	@Test
	void votesPickTheMostCommonMaterial() {
		CityCells.Votes v = new CityCells.Votes(0, 0, 0);
		int tarmac = index("TARMAC"), concrete = index("CONCRETE");
		// A floor at y 3.2 over block (1, z 1): it rounds to the top of block y 2.
		v.triangle(1.0, 3.2, 1.0, 2.0, 3.2, 1.0, 1.0, 3.2, 2.0, 1.0, tarmac);
		v.triangle(1.1, 3.2, 1.1, 1.2, 3.2, 1.1, 1.1, 3.2, 1.2, 1.0, concrete);
		int block = 1 + 8 * 1 + 64 * 2;
		assertEquals(tarmac, v.best(block, 0));
		assertEquals(CityCells.MAT_NONE, v.best(block, 1));
		// A wall in block (4, 5, 4).
		v.triangle(4.5, 5.0, 4.0, 4.5, 6.0, 4.0, 4.5, 5.0, 5.0, 0.0, concrete);
		assertEquals(concrete, v.best(4 + 8 * 4 + 64 * 5, 1));
	}

	@Test
	void readsTheHostsMaterialBits() {
		int gta = dev.libertycraft.link.Proto.TRI_GTA_MATERIAL;
		int shift = dev.libertycraft.link.Proto.TRI_GTA_MATERIAL_SHIFT;
		assertEquals(1 << 4, gta);
		assertEquals(16, shift);
		assertEquals(8, CityRecorder.materialOf(gta | 8 << shift, 1.0));
		assertEquals(14, CityRecorder.materialOf(gta | 14 << shift | dev.libertycraft.link.Proto.TRI_TERRAIN, 0.0));
		// Without GTA's material: by the kind of surface.
		assertEquals(CityCells.MAT_TERRAIN, CityRecorder.materialOf(dev.libertycraft.link.Proto.TRI_TERRAIN, 0.95));
		assertEquals(CityCells.MAT_FLOOR, CityRecorder.materialOf(0, 0.9));
		assertEquals(CityCells.MAT_WALL, CityRecorder.materialOf(0, 0.1));
		assertEquals(CityCells.MAT_CEILING, CityRecorder.materialOf(0, -1.0));
		// A code past GTA's materials isn't taken.
		assertEquals(CityCells.MAT_WALL, CityRecorder.materialOf(gta | 0xF2 << shift, 0.0));
	}

	@Test
	void packsRegions() {
		long[] cells = new long[512];
		cells[0] = CityCells.describe(layers(0, 7));
		cells[77] = CityCells.withMaterials(CityCells.describe(wallX(1)), 8, 14);
		cells[511] = CityCells.withMaterials(0L, 37, CityCells.MAT_NONE);
		byte[] packed = CityStore.pack(cells);
		assertEquals(64 + 3 * 8, packed.length);
		long[] back = new long[512];
		java.util.Arrays.fill(back, 99L);
		CityStore.unpack(packed, back);
		assertArrayEquals(cells, back);
	}

	@Test
	void plansAColumn() {
		// Ground filled up to 0.3 into block y 10 (fill 2 blocks), tarmac on top; a roof sheet high in y 20.
		long[] cells = new long[16];
		int y0 = 7;
		int tarmac = index("TARMAC");
		cells[8 - y0] = CityCells.describe(layers(3, 7));
		cells[9 - y0] = CityCells.withMaterials(CityCells.describe(layers(0, 7)), tarmac, CityCells.MAT_NONE);
		cells[10 - y0] = CityCells.describe(layers(0, 1));
		cells[20 - y0] = CityCells.withMaterials(CityCells.describe(layers(6, 6)), index("ROOFING_FELT"), CityCells.MAT_NONE);
		String[] out = CityPlan.column(cells, y0, Double.NaN);
		assertNull(out[10 - y0]);
		assertEquals("minecraft:gray_concrete", out[9 - y0]);
		assertEquals("minecraft:stone", out[8 - y0]);
		assertEquals("minecraft:gray_concrete", out[20 - y0]);
		assertNull(out[19 - y0]);
		assertNull(out[21 - y0]);
	}

	@Test
	void fillsWater() {
		// The sea at y 0.4 over ground whose top is y -3: water in y -2, -1 and 0 (its middle is under).
		long[] cells = new long[10];
		int y0 = -6;
		for (int y = -6; y <= -3; y++) {
			cells[y - y0] = CityCells.describe(layers(0, 7));
		}
		String[] out = CityPlan.column(cells, y0, 0.6);
		assertEquals(CityPlan.WATER, out[0 - y0]);
		assertEquals(CityPlan.WATER, out[-2 - y0]);
		assertNull(out[1 - y0]);
		assertEquals("minecraft:smooth_stone", out[-3 - y0]);
		// Deep water with nothing known below: a gravel bed MAX_WATER_DEPTH down.
		String[] deep = CityPlan.column(new long[41], -40, 0.6);
		assertEquals(CityPlan.WATER, deep[0 + 40]);
		assertEquals(CityPlan.SEABED, deep[-CityPlan.MAX_WATER_DEPTH + 40]);
		assertNull(deep[-CityPlan.MAX_WATER_DEPTH - 1 + 40]);
	}
}
