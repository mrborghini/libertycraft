package dev.libertycraft.world;

import static dev.libertycraft.link.Proto.COL_CLEAR;
import static dev.libertycraft.link.Proto.COL_FORGET;
import static dev.libertycraft.link.Proto.COL_REGION;
import static dev.libertycraft.link.Proto.COL_TRIS;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.Shapes;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

/** HostCollision's message handling: voxels per region, and dropping what GTA IV forgot once the player is far. */
class HostCollisionTest {
	private static final int EPOCH = 7;
	private static final long FULL = -1L;

	private final MemorySegment s = Arena.ofAuto().allocate(1 << 16);

	@BeforeEach
	void clear() {
		this.s.set(JAVA_INT, 0, EPOCH);
		HostCollision.handle(this.s, COL_CLEAR, 0);
	}

	private void header(int minX, int minY, int minZ, int maxX, int maxY, int maxZ, int count) {
		this.s.set(JAVA_INT, 0, minX);
		this.s.set(JAVA_INT, 4, minY);
		this.s.set(JAVA_INT, 8, minZ);
		this.s.set(JAVA_INT, 12, maxX);
		this.s.set(JAVA_INT, 16, maxY);
		this.s.set(JAVA_INT, 20, maxZ);
		this.s.set(JAVA_INT, 24, EPOCH);
		this.s.set(JAVA_INT, 28, count);
	}

	/** kColRegion for the box with these blocks: {x, y, z} and their 8 sub-voxel layers. */
	private void region(int minX, int minY, int minZ, int maxX, int maxY, int maxZ, int[][] at, long[][] layers) {
		header(minX, minY, minZ, maxX, maxY, maxZ, at.length);
		for (int i = 0; i < at.length; i++) {
			long e = 32 + i * 80L;
			this.s.set(JAVA_INT, e, at[i][0]);
			this.s.set(JAVA_INT, e + 4, at[i][1]);
			this.s.set(JAVA_INT, e + 8, at[i][2]);
			for (int k = 0; k < 8; k++) {
				this.s.set(JAVA_LONG, e + 16 + k * 8L, layers[i][k]);
			}
		}
		HostCollision.handle(this.s, COL_REGION, 0);
	}

	/** One whole region (rx, ry, rz) with these blocks. */
	private void region(int rx, int ry, int rz, int[][] at, long[][] layers) {
		region(rx * 8, ry * 8, rz * 8, rx * 8 + 7, ry * 8 + 7, rz * 8 + 7, at, layers);
	}

	/** kColTris for region (rx, ry, rz): one flat triangle across its floor at height y. */
	private void tris(int rx, int ry, int rz, float y) {
		header(rx * 8, ry * 8, rz * 8, rx * 8 + 7, ry * 8 + 7, rz * 8 + 7, 1);
		float[] v = { rx * 8, y, rz * 8, rx * 8 + 7, y, rz * 8, rx * 8, y, rz * 8 + 7 };
		for (int k = 0; k < 9; k++) {
			this.s.set(JAVA_FLOAT, 32 + k * 4L, v[k]);
		}
		this.s.set(JAVA_INT, 32 + 36, 0);
		HostCollision.handle(this.s, COL_TRIS, 0);
	}

	private void forget(int rx, int rz) {
		header(rx * 8, 0, rz * 8, rx * 8 + 7, 0, rz * 8 + 7, 0);
		HostCollision.handle(this.s, COL_FORGET, 0);
	}

	private static long[] full() {
		return new long[] { FULL, FULL, FULL, FULL, FULL, FULL, FULL, FULL };
	}

	private static long[] bottomLayer() {
		return new long[] { FULL, 0, 0, 0, 0, 0, 0, 0 };
	}

	private static int trisAt(double x, double y, double z) {
		List<HostTri> out = new ArrayList<>();
		HostCollision.trianglesNear(new AABB(x - 0.5, y - 0.5, z - 0.5, x + 0.5, y + 0.5, z + 0.5), out);
		return out.size();
	}

	@Test
	void regionVoxelsAnswerPerBlock() {
		region(1, 0, 2, new int[][] { { 9, 1, 17 }, { 10, 2, 18 }, { 15, 7, 23 } }, new long[][] { full(), bottomLayer(), bottomLayer() });
		assertSame(Shapes.block(), HostCollision.shapeAt(new BlockPos(9, 1, 17)));
		assertNotNull(HostCollision.shapeAt(new BlockPos(10, 2, 18)));
		assertNull(HostCollision.shapeAt(new BlockPos(11, 2, 18)));
		assertTrue(HostCollision.isKnown(8, 0, 16));
		assertTrue(HostCollision.isKnown(15, 7, 23));
		assertFalse(HostCollision.isKnown(16, 0, 16));
		assertEquals(64 / 512.0F, HostCollision.solidFraction(new BlockPos(10, 2, 18)));
		assertEquals(1.0F / 8, HostCollision.groundTop(new BlockPos(10, 2, 18)));
		assertEquals(1.0F, HostCollision.groundTop(new BlockPos(9, 1, 17)));
		assertTrue(HostCollision.supportsFromBelow(new BlockPos(10, 2, 18)));
		assertTrue(HostCollision.supportsFromBelow(new BlockPos(9, 2, 17))); // the full block below
		assertFalse(HostCollision.supportsFromBelow(new BlockPos(11, 2, 18)));
		assertTrue(HostCollision.hasSolidBelow(9, 4, 17, 3));
		assertFalse(HostCollision.hasSolidBelow(9, 6, 17, 3));
		assertEquals(3, HostCollision.blockCount());
		// the same sub-voxels share one shape
		assertSame(HostCollision.shapeAt(new BlockPos(10, 2, 18)), HostCollision.shapeAt(new BlockPos(15, 7, 23)));
	}

	@Test
	void negativeCoordinatesLandInTheirRegion() {
		region(-1, -1, -1, new int[][] { { -1, -1, -1 }, { -8, -8, -8 } }, new long[][] { full(), bottomLayer() });
		assertSame(Shapes.block(), HostCollision.shapeAt(new BlockPos(-1, -1, -1)));
		assertNotNull(HostCollision.shapeAt(new BlockPos(-8, -8, -8)));
		assertNull(HostCollision.shapeAt(new BlockPos(-2, -1, -1)));
		assertTrue(HostCollision.isKnown(-8, -8, -8));
		assertFalse(HostCollision.isKnown(0, -1, -1));
	}

	@Test
	void aRegionSentAgainReplacesTheOldOne() {
		region(0, 0, 0, new int[][] { { 1, 1, 1 }, { 2, 2, 2 } }, new long[][] { full(), full() });
		region(0, 0, 0, new int[][] { { 3, 3, 3 } }, new long[][] { full() });
		assertNull(HostCollision.shapeAt(new BlockPos(1, 1, 1)));
		assertNull(HostCollision.shapeAt(new BlockPos(2, 2, 2)));
		assertNotNull(HostCollision.shapeAt(new BlockPos(3, 3, 3)));
		// an empty region is still known
		region(0, 0, 0, new int[0][], new long[0][]);
		assertNull(HostCollision.shapeAt(new BlockPos(3, 3, 3)));
		assertTrue(HostCollision.isKnown(3, 3, 3));
	}

	@Test
	void aPartialBoxKeepsTheRestOfTheRegion() {
		region(0, 0, 0, new int[][] { { 1, 1, 1 }, { 6, 6, 6 } }, new long[][] { full(), full() });
		region(0, 0, 0, 3, 3, 3, new int[][] { { 2, 2, 2 } }, new long[][] { bottomLayer() });
		assertNull(HostCollision.shapeAt(new BlockPos(1, 1, 1)));
		assertNotNull(HostCollision.shapeAt(new BlockPos(2, 2, 2)));
		assertSame(Shapes.block(), HostCollision.shapeAt(new BlockPos(6, 6, 6)));
	}

	@Test
	void forgottenColumnsAreDroppedOnlyOnceThePlayerIsFar() {
		for (int ry = -1; ry <= 2; ry++) {
			tris(0, ry, 0, ry * 8 + 0.5F);
			region(0, ry, 0, new int[][] { { 1, ry * 8, 1 } }, new long[][] { full() });
		}
		tris(1, 0, 0, 0.5F);
		region(1, 0, 0, new int[][] { { 9, 0, 1 } }, new long[][] { full() });
		forget(0, 0);
		// near the player: everything stays
		HostCollision.evictAround(HostCollision.KEEP_TRIS, 0, 0);
		assertEquals(1, trisAt(1, 0.5, 1));
		assertTrue(HostCollision.isKnown(1, 0, 1));
		// a little further: the triangles go, the voxels stay
		HostCollision.evictAround(HostCollision.KEEP_TRIS + 1, 0, 0);
		assertEquals(0, trisAt(1, 0.5, 1));
		assertEquals(0, trisAt(1, 16.5, 1));
		assertNotNull(HostCollision.shapeAt(new BlockPos(1, 16, 1)));
		assertEquals(1, trisAt(9, 0.5, 1)); // not forgotten
		// far: the column is gone, every row of it
		HostCollision.evictAround(-HostCollision.KEEP_VOXELS - 1, 0, 0);
		for (int ry = -1; ry <= 2; ry++) {
			assertFalse(HostCollision.isKnown(1, ry * 8, 1));
			assertNull(HostCollision.shapeAt(new BlockPos(1, ry * 8, 1)));
		}
		// what GTA IV still tracks stays however far
		assertTrue(HostCollision.isKnown(9, 0, 1));
		assertEquals(1, trisAt(9, 0.5, 1));
	}

	@Test
	void aColumnSentAgainIsNoLongerForgotten() {
		region(0, 0, 0, new int[][] { { 1, 1, 1 } }, new long[][] { full() });
		forget(0, 0);
		region(0, 0, 0, new int[][] { { 1, 1, 1 } }, new long[][] { full() });
		HostCollision.evictAround(100, 100, 0);
		assertTrue(HostCollision.isKnown(1, 1, 1));
		forget(0, 0);
		HostCollision.evictAround(100, 100, 0);
		assertFalse(HostCollision.isKnown(1, 1, 1));
		assertFalse(HostCollision.active());
	}

	@Test
	void aForgetFromAnotherEpochIsIgnored() {
		region(0, 0, 0, new int[][] { { 1, 1, 1 } }, new long[][] { full() });
		header(0, 0, 0, 7, 0, 7, 0);
		this.s.set(JAVA_INT, 24, EPOCH + 1);
		HostCollision.handle(this.s, COL_FORGET, 0);
		HostCollision.evictAround(100, 100, 0);
		assertTrue(HostCollision.isKnown(1, 1, 1));
	}
}
