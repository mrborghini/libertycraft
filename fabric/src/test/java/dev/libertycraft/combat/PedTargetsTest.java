package dev.libertycraft.combat;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

class PedTargetsTest {
	@Test
	void aTieGoesToThePlayer() {
		assertFalse(PedTargets.picksPed(25.0, 25.0));
		assertTrue(PedTargets.picksPed(24.0, 25.0));
		assertFalse(PedTargets.picksPed(26.0, 25.0));
	}

	@Test
	void turnsOnlyToSomeoneClearlyNearer() {
		assertTrue(PedTargets.switchesTo(19.8, 1.0)); // the player ran off, a ped stands beside it
		assertTrue(PedTargets.switchesTo(5.0, 0.9));
		assertFalse(PedTargets.switchesTo(5.0, 1.0)); // just the margin nearer: stays
		assertFalse(PedTargets.switchesTo(5.0, 4.0));
		assertFalse(PedTargets.switchesTo(1.5, 0.5)); // fighting at arm's length: stays
	}
}
