package dev.libertycraft.combat;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

class FireworkBlastTest {
	@Test
	void noStarsNoBlast() {
		assertEquals(0.0F, FireworkBlast.radius(0));
		assertEquals(0.0F, FireworkBlast.radius(-1));
	}

	@Test
	void growsWithStarsUpToTheRocketBlast() {
		assertEquals(4.0F, FireworkBlast.radius(1));
		assertEquals(5.0F, FireworkBlast.radius(2));
		assertEquals(7.0F, FireworkBlast.radius(4));
		assertEquals(FireworkBlast.MAX_RADIUS, FireworkBlast.radius(5));
		assertEquals(FireworkBlast.MAX_RADIUS, FireworkBlast.radius(7)); // a crafted rocket's most
		assertEquals(FireworkBlast.MAX_RADIUS, FireworkBlast.radius(200));
	}
}
