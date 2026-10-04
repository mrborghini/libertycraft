package dev.libertycraft.world.city;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class CityStoreTest {
	@TempDir
	Path dir;

	private static long[] region(int seed) {
		long[] cells = new long[512];
		long[] full = { -1L, -1L, -1L, -1L, -1L, -1L, -1L, -1L };
		for (int i = seed % 7; i < 512; i += 11) {
			cells[i] = CityCells.withMaterials(CityCells.describe(full), seed & 0xFF, CityCells.MAT_NONE);
		}
		return cells;
	}

	@Test
	void keepsRegionsWaterAndChunksAcrossSessions() throws Exception {
		CityStore store = CityStore.open(this.dir);
		store.putRegion(147, 4, -26, region(8));
		store.putRegion(-3, -1, 300, region(14)); // another tile, negative coordinates
		store.putWater(1180, -210, 2, new float[] { 0.5F, Float.NaN, -1.0e30F, 2.25F });
		long rev = store.chunkSourceRevision(73, -13);
		assertTrue(rev > 0);
		store.setChunkApplied(73, -13, rev);
		CityStore.close();
		assertTrue(Files.list(this.dir).anyMatch(p -> p.getFileName().toString().startsWith("t.")));

		CityStore again = CityStore.open(this.dir);
		long[] out = new long[512];
		assertTrue(again.region(147, 4, -26, out));
		assertArrayEquals(region(8), out);
		assertTrue(again.region(-3, -1, 300, out));
		assertArrayEquals(region(14), out);
		assertFalse(again.region(147, 5, -26, out));
		assertArrayEquals(new int[] { 4, 4 }, again.columnRange(147, -26));
		assertNull(again.columnRange(148, -26));
		assertEquals(0.5, again.waterAt(1180, -210), 1e-9);
		assertTrue(Double.isNaN(again.waterAt(1181, -210)));
		assertTrue(Double.isNaN(again.waterAt(1180, -209)));
		assertEquals(2.25, again.waterAt(1181, -209), 1e-9);
		assertEquals(rev, again.chunkApplied(73, -13));
		assertEquals(rev, again.chunkSourceRevision(73, -13));
		// Newer data for a region bumps its column's revision; the same data again doesn't.
		long before = again.chunkSourceRevision(73, -13);
		again.putRegion(147, 4, -26, region(8));
		assertEquals(before, again.chunkSourceRevision(73, -13));
		again.putRegion(147, 4, -26, region(9));
		assertTrue(again.chunkSourceRevision(73, -13) > before);
		CityStore.close();
	}
}
