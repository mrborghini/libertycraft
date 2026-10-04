package dev.libertycraft.world;

import dev.libertycraft.link.Proto;

/**
 * Minecraft's time and weather against GTA IV's ({@link HostSky}), without Minecraft's classes (tested
 * by SkyRulesTest). Minecraft's tick 0 is 06:00 and a day is 24000 ticks; GTA IV's weather is one of
 * its types (Proto.GTA_*), Minecraft's is raining and thundering.
 */
public final class SkyRules {
	private SkyRules() {
	}

	/** GTA IV's hour (0 to 24, the minutes as the fraction) for Minecraft's clock at {@code ticks}. */
	public static float hourOf(long ticks) {
		long t = Math.floorMod(ticks, 24000L);
		return (float) ((t / 1000.0 + 6.0) % 24.0);
	}

	/** The hours from {@code a} to {@code b} the short way round the clock (-12 to 12). */
	public static float hourDiff(float a, float b) {
		float d = ((b - a) % 24.0F + 24.0F) % 24.0F;
		return d > 12.0F ? d - 24.0F : d;
	}

	/** GTA IV's weather type rains (rain, drizzle, a thunderstorm). */
	public static boolean raining(int gtaWeather) {
		return gtaWeather == Proto.GTA_RAIN || gtaWeather == Proto.GTA_DRIZZLE || gtaWeather == Proto.GTA_LIGHTNING;
	}

	/** GTA IV's weather type thunders. */
	public static boolean thundering(int gtaWeather) {
		return gtaWeather == Proto.GTA_LIGHTNING;
	}

	/** The GTA IV weather type for Minecraft's weather: clear EXTRASUNNY, rain RAIN, thunder LIGHTNING. */
	public static int gtaWeatherFor(boolean raining, boolean thundering) {
		return thundering ? Proto.GTA_LIGHTNING : raining ? Proto.GTA_RAIN : Proto.GTA_EXTRA_SUNNY;
	}

	/** GTA IV's weather from SkyState's flags, -1 if the host doesn't say. */
	public static int weatherOfFlags(int flags) {
		return ((flags & Proto.SKY_WEATHER_MASK) >>> Proto.SKY_WEATHER_SHIFT) - 1;
	}
}
