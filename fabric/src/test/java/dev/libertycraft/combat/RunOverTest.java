package dev.libertycraft.combat;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

class RunOverTest {
	private static final double LENGTH = 4.6, WIDTH = 1.8, SLACK = 0.05, FLOOR = 64.0;
	private static final double DT = 0.05;
	// A zombie: 0.6 wide, 1.95 tall.
	private static final double R = 0.3, TALL = 1.95;

	/**
	 * A car's records as the host lays them out (CombatMath.h VehicleSegments): a row of square boxes
	 * along its axis, each (width + 2 slack) / e wide, e = |cos| + |sin| of its heading.
	 */
	private static RunOver.Pose car(double cx, double cz, double yaw) {
		double f = Math.toRadians(yaw);
		double fx = -Math.sin(f), fz = Math.cos(f);
		double e = Math.abs(fx) + Math.abs(fz);
		double side = (WIDTH + 2.0 * SLACK) / e;
		double reach = (LENGTH - WIDTH) * 0.5;
		int n = 5;
		double[] x = new double[n], y = new double[n], z = new double[n], w = new double[n], h = new double[n];
		for (int i = 0; i < n; i++) {
			double off = reach - 2.0 * reach * i / (n - 1);
			x[i] = cx + fx * off;
			z[i] = cz + fz * off;
			y[i] = FLOOR;
			w[i] = side;
			h[i] = 1.4;
		}
		return RunOver.pose(x, y, z, w, h, n, yaw);
	}

	private static RunOver.Contact contact(RunOver.Pose now, RunOver.Pose before, double mx, double mz, double px, double pz) {
		return RunOver.contact(now, before, DT, mx, mz, px, pz, R, FLOOR, FLOOR + TALL);
	}

	@Test
	void poseOfACarFacingAnAxis() {
		RunOver.Pose p = car(10.0, -3.0, 0.0);
		assertEquals(10.0, p.x(), 1.0E-9);
		assertEquals(-3.0, p.z(), 1.0E-9);
		assertEquals(LENGTH * 0.5 + SLACK, p.halfLength(), 1.0E-9);
		assertEquals(WIDTH * 0.5 + SLACK, p.halfWidth(), 1.0E-9);
		assertEquals(FLOOR, p.bottom(), 1.0E-9);
		assertEquals(FLOOR + 1.4, p.top(), 1.0E-9);
	}

	@Test
	void poseOfACarDriving45DegreesOff() {
		// Its square boxes stand diagonally to it: their corners reach a little further than the body.
		RunOver.Pose p = car(0.0, 0.0, 45.0);
		assertEquals(0.0, p.x(), 1.0E-9);
		assertEquals(0.0, p.z(), 1.0E-9);
		assertEquals(LENGTH * 0.5 + SLACK, p.halfLength(), 0.1);
		assertEquals(WIDTH * 0.5 + SLACK, p.halfWidth(), 0.1);
	}

	@Test
	void aCarDrivingIntoAZombieAt10msKillsIt() {
		// Facing south (+z) at 10 m/s: half a block a tick. The zombie stands just in front of it.
		RunOver.Pose before = car(0.0, 0.0, 0.0), now = car(0.0, 0.5, 0.0);
		double front = 0.5 + LENGTH * 0.5 + SLACK + R;
		RunOver.Contact c = contact(now, before, 0.2, front, 0.2, front);
		assertTrue(c.touching());
		assertTrue(c.hit());
		assertEquals(10.0, c.speed(), 1.0E-6);
		assertEquals(10.0, c.closing(), 1.0E-6);
		assertEquals(0.0, c.nx(), 1.0E-9);
		assertEquals(1.0, c.nz(), 1.0E-9);
		// 24 Minecraft damage: through a zombie's 2 armour points still more than its 20 health.
		assertTrue(RunOver.damage(c.closing()) * (1.0 - 0.4 / 25.0) > 20.0);
	}

	@Test
	void aSlowCarOnlyPushes() {
		// 2 m/s: 0.1 a tick.
		RunOver.Pose before = car(0.0, 0.0, 0.0), now = car(0.0, 0.1, 0.0);
		double front = 0.1 + LENGTH * 0.5 + SLACK + R;
		RunOver.Contact c = contact(now, before, 0.0, front, 0.0, front);
		assertTrue(c.touching());
		assertFalse(c.hit());
		assertEquals(0.0F, RunOver.damage(c.closing()));
	}

	@Test
	void brushingPastAtSpeedIsNoHit() {
		// 15 m/s south, the zombie beside its door.
		RunOver.Pose before = car(0.0, 0.0, 0.0), now = car(0.0, 0.75, 0.0);
		double beside = WIDTH * 0.5 + SLACK + R;
		RunOver.Contact c = contact(now, before, beside, 0.5, beside, 0.5);
		assertTrue(c.touching());
		assertFalse(c.hit());
		assertEquals(15.0, c.speed(), 1.0E-6);
		assertEquals(0.0, c.closing(), 1.0E-6);
	}

	@Test
	void reversingIntoAMobHitsIt() {
		// Facing south, reversing north (-z) at 5 m/s into a mob behind it.
		RunOver.Pose before = car(0.0, 0.0, 0.0), now = car(0.0, -0.25, 0.0);
		double back = -0.25 - (LENGTH * 0.5 + SLACK + R);
		RunOver.Contact c = contact(now, before, 0.0, back, 0.0, back);
		assertTrue(c.hit());
		assertEquals(5.0, c.closing(), 1.0E-6);
		assertEquals(-1.0, c.nz(), 1.0E-9);
	}

	@Test
	void drivingAwayIsNoHit() {
		RunOver.Pose before = car(0.0, 0.0, 0.0), now = car(0.0, 0.5, 0.0);
		double back = 0.5 - (LENGTH * 0.5 + SLACK + R);
		RunOver.Contact c = contact(now, before, 0.0, back, 0.0, back);
		assertTrue(c.touching());
		assertFalse(c.hit());
	}

	@Test
	void aFastCarStillHitsWhatItPassedIntoBetweenTwoTables() {
		// 30 m/s: 1.5 blocks a tick. The zombie was half a block in front of it and is now well inside.
		RunOver.Pose before = car(0.0, 0.0, 0.0), now = car(0.0, 1.5, 0.0);
		double was = LENGTH * 0.5 + SLACK + R + 0.5;
		RunOver.Contact c = contact(now, before, 0.0, was, 0.0, was);
		assertTrue(c.hit());
		assertEquals(30.0, c.closing(), 1.0E-6);
		// It goes out through the front.
		assertEquals(LENGTH * 0.5 + SLACK + R + 0.05, c.outA(), 1.0E-9);
	}

	@Test
	void aTurningCarSwingsItsTailIntoAMob() {
		// Standing, turning 10 degrees in a tick (200 degrees a second): the tail sweeps east into the mob.
		RunOver.Pose before = car(0.0, 0.0, 0.0), now = car(0.0, 0.0, 10.0);
		double mx = WIDTH * 0.5 + SLACK + R - 0.05, mz = -1.9;
		RunOver.Contact c = contact(now, before, mx, mz, mx, mz);
		assertTrue(c.touching());
		assertTrue(c.hit());
		assertTrue(c.closing() > 5.0 && c.closing() < 10.0, "closing " + c.closing());
	}

	@Test
	void aMobOnTheRoofOrFarAwayIsUntouched() {
		RunOver.Pose before = car(0.0, 0.0, 0.0), now = car(0.0, 0.5, 0.0);
		assertFalse(RunOver.contact(now, before, DT, 0.0, 0.5, 0.0, 0.5, R, FLOOR + 1.4, FLOOR + 1.4 + TALL).touching());
		assertFalse(contact(now, before, 3.0, 0.0, 3.0, 0.0).touching());
	}

	@Test
	void aTeleportIsNoHit() {
		// 6 blocks in a tick: 120 m/s.
		RunOver.Pose before = car(0.0, -6.0, 0.0), now = car(0.0, 0.0, 0.0);
		double front = LENGTH * 0.5 + SLACK + R;
		assertFalse(contact(now, before, 0.0, front, 0.0, front).hit());
	}

	@Test
	void damageGrowsWithSpeed() {
		assertEquals(0.0F, RunOver.damage(RunOver.MIN_SPEED - 0.01));
		assertEquals(1.5F, RunOver.damage(RunOver.MIN_SPEED), 1.0E-6);
		assertEquals(24.0F, RunOver.damage(10.0), 1.0E-6);
		assertTrue(RunOver.damage(20.0) > RunOver.damage(10.0));
	}

	@Test
	void theThrowFollowsTheCarWithLift() {
		double[] v = RunOver.throwVelocity(10.0, 0.0, 10.0, 0.0, 1.0, 0.0);
		assertEquals(0.0, v[0], 1.0E-9);
		assertEquals(0.5, v[2], 1.0E-9); // 10 m/s
		assertEquals(0.45, v[1], 1.0E-9);
		// A glancing blow through the side: mostly along the car, a little out of the side.
		double[] g = RunOver.throwVelocity(5.0, 0.0, 10.0, 1.0, 0.0, 0.0);
		assertTrue(g[2] > 0.0 && g[0] > 0.0 && g[0] < g[2]);
		// An iron golem (knockback resistance 1) flies less far.
		double[] golem = RunOver.throwVelocity(10.0, 0.0, 10.0, 0.0, 1.0, 1.0);
		assertTrue(golem[2] < v[2] && golem[1] < v[1]);
		// Never faster than 30 m/s.
		assertEquals(RunOver.THROW_MAX, RunOver.throwVelocity(60.0, 1.0, 0.0, 1.0, 0.0, 0.0)[0], 1.0E-9);
	}
}
