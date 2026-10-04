package dev.libertycraft.world;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import dev.libertycraft.link.Proto;
import org.junit.jupiter.api.Test;

class SkyRulesTest {
	@Test
	void minecraftsTicksAreGtaIvsHours() {
		assertEquals(6.0F, SkyRules.hourOf(0L), 1.0E-6);       // sunrise
		assertEquals(7.0F, SkyRules.hourOf(1000L), 1.0E-6);    // /time set day
		assertEquals(12.0F, SkyRules.hourOf(6000L), 1.0E-6);   // noon
		assertEquals(19.0F, SkyRules.hourOf(13000L), 1.0E-6);  // night
		assertEquals(0.0F, SkyRules.hourOf(18000L), 1.0E-6);   // midnight
		assertEquals(5.5F, SkyRules.hourOf(23500L), 1.0E-6);   // 05:30
		assertEquals(7.0F, SkyRules.hourOf(5 * 24000L + 1000L), 1.0E-6);  // any day
		// And back: SpawnRules.dayTimeOf is the inverse.
		for (long t = 0; t < 24000; t += 250) {
			assertEquals(t, SpawnRules.dayTimeOf(SkyRules.hourOf(t)));
		}
	}

	@Test
	void hoursApartTheShortWay() {
		assertEquals(1.0F, SkyRules.hourDiff(23.5F, 0.5F), 1.0E-6);
		assertEquals(-1.0F, SkyRules.hourDiff(0.5F, 23.5F), 1.0E-6);
		assertEquals(6.0F, SkyRules.hourDiff(6.0F, 12.0F), 1.0E-6);
	}

	@Test
	void weathersMeetHalfway() {
		assertEquals(Proto.GTA_EXTRA_SUNNY, SkyRules.gtaWeatherFor(false, false));
		assertEquals(Proto.GTA_RAIN, SkyRules.gtaWeatherFor(true, false));
		assertEquals(Proto.GTA_LIGHTNING, SkyRules.gtaWeatherFor(true, true));
		assertTrue(SkyRules.raining(Proto.GTA_RAIN) && SkyRules.raining(Proto.GTA_DRIZZLE) && SkyRules.raining(Proto.GTA_LIGHTNING));
		assertFalse(SkyRules.raining(Proto.GTA_CLOUDY) || SkyRules.raining(Proto.GTA_FOGGY) || SkyRules.raining(Proto.GTA_SUNNY));
		assertTrue(SkyRules.thundering(Proto.GTA_LIGHTNING));
		assertFalse(SkyRules.thundering(Proto.GTA_RAIN));
		// SkyState flags: the type + 1 in bits 8 to 11, 0 when the host doesn't say.
		assertEquals(-1, SkyRules.weatherOfFlags(Proto.SKY_IN_GAME));
		assertEquals(Proto.GTA_LIGHTNING, SkyRules.weatherOfFlags(Proto.SKY_IN_GAME | (Proto.GTA_LIGHTNING + 1) << Proto.SKY_WEATHER_SHIFT));
		assertEquals(Proto.GTA_EXTRA_SUNNY, SkyRules.weatherOfFlags(1 << Proto.SKY_WEATHER_SHIFT));
	}
}
