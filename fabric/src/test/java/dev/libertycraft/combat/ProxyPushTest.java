package dev.libertycraft.combat;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import dev.libertycraft.link.Proto;
import org.junit.jupiter.api.Test;

class ProxyPushTest {
	private static final double EPS = 1.0E-9;

	// The player (0.6 x 1.8) with its feet at (x, y, z).
	private static double[] sep(double x, double y, double z, double[] b, double vx, double vz) {
		return ProxyPush.separation(x - 0.3, y, z - 0.3, x + 0.3, y + 1.8, z + 0.3, b[0], b[1], b[2], b[3], b[4], b[5], vx, vz, ProxyPush.STEP_UP);
	}

	// A car piece: 2 x 1.5 x 2 centred on x = 0, z = 0, standing on y = 64.
	private static final double[] CAR = { -1.0, 64.0, -1.0, 1.0, 65.5, 1.0 };

	@Test
	void apartOrTouchingIsNoOverlap() {
		assertNull(sep(1.3, 64.0, 0.0, CAR, 0.0, 0.0));
		assertNull(sep(1.31, 64.0, 0.0, CAR, 0.0, 0.0));
		assertNull(sep(0.0, 65.5, 0.0, CAR, 0.0, 0.0)); // standing on the roof
	}

	@Test
	void standingStillPushesOutTheShortWay() {
		// 0.1 into its east side: back out east.
		double[] m = sep(1.2, 64.0, 0.0, CAR, 0.0, 0.0);
		assertArrayEquals(new double[] { 0.1, 0.0, 0.0 }, m, 1.0E-9);
		// 0.2 into its north side (-z): out north.
		m = sep(0.0, 64.0, -1.1, CAR, 0.0, 0.0);
		assertArrayEquals(new double[] { 0.0, 0.0, -0.2 }, m, 1.0E-9);
	}

	@Test
	void feetJustBelowTheTopStepUp() {
		double[] m = sep(0.5, 65.2, 0.5, CAR, 0.0, 0.0);
		assertArrayEquals(new double[] { 0.0, 0.3, 0.0 }, m, 1.0E-9);
	}

	@Test
	void aMovingCarNeverPushesBackThroughItself() {
		// The car drove east into the player, whose centre is now 0.1 west of the car's east end: out
		// east, ahead of it.
		double[] m = sep(0.9, 64.0, 0.0, CAR, 0.8, 0.0);
		assertTrue(m[0] > 0.0);
		// At its back (west) end the short way (0.5 west) is against its motion: out sideways instead.
		m = sep(-0.8, 64.0, 0.0, CAR, 0.8, 0.0);
		assertEquals(0.0, m[0], EPS);
		assertEquals(1.3, Math.abs(m[2]), EPS);
		// Sideways out of its path is fine (perpendicular to the motion).
		m = sep(0.0, 64.0, 1.2, CAR, 0.8, 0.0);
		assertArrayEquals(new double[] { 0.0, 0.0, 0.1 }, m, 1.0E-9);
	}

	@Test
	void shoveFollowsTheCar() {
		double[] v = ProxyPush.shove(new double[] { 0.5, 0.0, 0.0 }, 1.0, 0.0);
		assertEquals(1.2, v[0], EPS);
		assertEquals(0.0, v[2], EPS);
		assertTrue(v[1] > 0.0 && v[1] <= 0.4);
		// Moving away, or along the other axis: no shove.
		assertArrayEquals(new double[3], ProxyPush.shove(new double[] { 0.5, 0.0, 0.0 }, -1.0, 0.0), EPS);
		assertArrayEquals(new double[3], ProxyPush.shove(new double[] { 0.5, 0.0, 0.0 }, 0.0, 1.0), EPS);
		// Too slow to count.
		assertArrayEquals(new double[3], ProxyPush.shove(new double[] { 0.5, 0.0, 0.0 }, 0.01, 0.0), EPS);
	}

	@Test
	void clampShortensPedNudges() {
		double[] m = { 0.6, 0.0, 0.8 };
		ProxyPush.clamp(m, 0.3);
		assertEquals(0.18, m[0], EPS);
		assertEquals(0.24, m[2], EPS);
	}

	@Test
	void hurtDirections() {
		// combat_test.cpp: an attacker due north of the player (GTA +y) is MC yaw 180, i.e. toward -z.
		double[] north = ProxyPush.hurtDirection(Proto.HURT_HAS_DIRECTION | (180 << Proto.HURT_DIRECTION_SHIFT));
		assertEquals(0.0, north[0], 1.0E-9);
		assertEquals(-1.0, north[1], 1.0E-9);
		double[] east = ProxyPush.hurtDirection(Proto.HURT_HAS_DIRECTION | (270 << Proto.HURT_DIRECTION_SHIFT));
		assertEquals(1.0, east[0], 1.0E-9);
		assertEquals(0.0, east[1], 1.0E-9);
		assertNull(ProxyPush.hurtDirection(180 << Proto.HURT_DIRECTION_SHIFT)); // no flag, no direction
		assertEquals(1 << 2, Proto.HURT_HAS_DIRECTION);
		assertEquals(16, Proto.HURT_DIRECTION_SHIFT);
		assertEquals(0x4C, Proto.MS_VITALS_HEALTH);
		assertEquals(0xBC, Proto.MS_VITALS_ARMOUR);
	}

	@Test
	void vehicleIdsMatchTheProtocol() {
		// libertycraft_protocol.h: kActorVehicle = 1 << 4, formId = 'V' tag | handle << 4 | piece.
		assertEquals(1 << 4, Proto.ACTOR_VEHICLE);
		assertEquals(0x56000000, Proto.ACTOR_VEHICLE_TAG);
		assertEquals(16, Proto.ACTOR_VEHICLE_SEGMENTS);
		assertEquals(Proto.ACTOR_VEHICLE_SEGMENTS - 1, Proto.ACTOR_VEHICLE_PIECE_MASK);
		assertEquals(1 << 4, Proto.HIT_EXPLOSION);
		assertEquals(6, Proto.EV_HIT_POINT);
		int id = Proto.ACTOR_VEHICLE_TAG | (0x1234 << 4) | 2;
		assertEquals(0x56012342, id); // the same id combat_test.cpp checks on the host side
		assertEquals(Proto.ACTOR_VEHICLE_TAG | (0x1234 << 4), id & ~Proto.ACTOR_VEHICLE_PIECE_MASK);
	}
}
