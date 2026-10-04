package dev.libertycraft.combat;

/**
 * A firework rocket with stars as GTA IV's RPG (FireworkRocketMixin sends its burst as a kEvExplosion
 * with Proto.EXPLOSION_FIREWORK). GTA IV's rocket blast reaches 8 m; a rocket's blast grows with its
 * stars up to that. Minecraft's own firework damage reaches 5 blocks whatever the stars, 5 + 2 per star
 * at the centre.
 */
public final class FireworkBlast {
	/** The radius of GTA IV's rocket blast (explosionFx.dat, ROCKET: END_RADIUS 8.0 m). */
	public static final float MAX_RADIUS = 8.0F;

	private FireworkBlast() {
	}

	/**
	 * The blast radius (blocks, = metres in GTA IV) of a rocket carrying {@code stars} firework stars: none
	 * without stars (vanilla: no burst), 4 for one, one more per star, GTA's full rocket blast (8) from five.
	 */
	public static float radius(int stars) {
		if (stars <= 0) {
			return 0.0F;
		}
		return Math.min(3.0F + stars, MAX_RADIUS);
	}
}
