package dev.libertycraft.world;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.clock.ServerClockManager;
import net.minecraft.world.clock.WorldClock;
import net.minecraft.world.clock.WorldClocks;
import net.minecraft.world.level.saveddata.WeatherData;
import org.jspecify.annotations.Nullable;

/**
 * Minecraft's time and weather and GTA IV's, one sky. While GTA IV is linked and in game:
 * <ul>
 * <li>Minecraft's clock (the overworld clock, which the mirror world and the blocky city run on) follows
 * GTA IV's (SkyState's gameHour, every second): undead burn in GTA IV's sun, beds go by its night.</li>
 * <li>Changing Minecraft's time ({@code /time set} or {@code add}, a night slept through in a bed) sets
 * GTA IV's clock to the hour it stands for (Proto.EV_SET_TIME; tick 0 = 06:00), and the following waits
 * until GTA IV is there (3 s at most).</li>
 * <li>Minecraft's own weather cycle stops (ServerLevelWeatherMixin) and its weather follows GTA IV's
 * (SkyState's flags, Proto.SKY_WEATHER_SHIFT): rain while GTA IV rains, drizzles or storms, thunder in
 * its storms. Rain puts out fires and keeps the undead from burning, as in Minecraft.</li>
 * <li>{@code /weather clear, rain, thunder} (with or without a duration) force GTA IV's weather for that
 * long (Proto.EV_SET_WEATHER: EXTRASUNNY, RAIN, LIGHTNING), after which GTA IV's own weather comes back;
 * sleeping through a rainy night clears it, as in Minecraft.</li>
 * </ul>
 * Changes this class makes itself (the following) are never sent back ({@link #syncing}).
 */
public final class HostSky {
	private static final int TIME_EVERY = 20, WEATHER_EVERY = 10;
	private static final long PENDING_NANOS = 3_000_000_000L;
	/** Our own change of Minecraft's clock or weather is being made: the mixins don't report it. */
	private static boolean syncing;
	private static final Link.SkyState SKY = new Link.SkyState();
	private static boolean inGame;
	private static int gtaWeather = -1;
	// A change sent to GTA IV, until it shows (or PENDING_NANOS pass): the following waits.
	private static float pendingHour = Float.NaN;
	private static long pendingHourUntil;
	private static int pendingWeather = -1;
	private static long pendingWeatherUntil;

	private HostSky() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(HostSky::tick);
	}

	/** Minecraft's weather follows GTA IV's now (its own cycle is stopped). */
	public static boolean followsGta() {
		return inGame && gtaWeather >= 0 && Link.active();
	}

	/** Our own change is being made (the mixins leave it alone). */
	public static boolean syncing() {
		return syncing;
	}

	private static @Nullable Holder<WorldClock> clock(MinecraftServer server) {
		return server.registryAccess().lookupOrThrow(Registries.WORLD_CLOCK).get(WorldClocks.OVERWORLD).orElse(null);
	}

	private static void tick(MinecraftServer server) {
		int t = server.getTickCount();
		if (t % WEATHER_EVERY != 0) {
			return;
		}
		if (!Link.active() || !Link.readSkyState(SKY)) {
			inGame = false;
			return;
		}
		inGame = SKY.inGame() && !SKY.loading();
		gtaWeather = inGame ? SkyRules.weatherOfFlags(SKY.flags) : -1;
		if (!inGame) {
			return;
		}
		followWeather(server);
		if (t % TIME_EVERY == 0) {
			followTime(server, SKY.gameHour);
		}
	}

	/** Minecraft's clock to GTA IV's hour (its day count stays), unless a change of ours is on its way. */
	static void followTime(MinecraftServer server, float hour) {
		Holder<WorldClock> clock = clock(server);
		if (clock == null) {
			return;
		}
		if (!Float.isNaN(pendingHour)) {
			if (Math.abs(SkyRules.hourDiff(hour, pendingHour)) > 0.25F && System.nanoTime() < pendingHourUntil) {
				return;
			}
			pendingHour = Float.NaN;
		}
		ServerClockManager clocks = server.clockManager();
		long now = clocks.getInstance(clock).totalTicks();
		long d = SpawnRules.clockCorrection(now, hour);
		if (d == 0) {
			return;
		}
		syncing = true;
		try {
			clocks.setTotalTicks(clock, Math.max(0L, now + d));
		} finally {
			syncing = false;
		}
		if (Math.abs(d) > 1000) {
			LibertyCraft.LOG.info("[LibertyCraft] Minecraft's clock follows GTA IV's: {} ({} ticks)", clockText(hour), d > 0 ? "+" + d : d);
		}
	}

	/** Minecraft's weather to GTA IV's, unless a change of ours is on its way. */
	private static void followWeather(MinecraftServer server) {
		if (gtaWeather < 0) {
			return;
		}
		boolean raining = SkyRules.raining(gtaWeather), thundering = SkyRules.thundering(gtaWeather);
		if (pendingWeather >= 0) {
			boolean arrived = SkyRules.raining(pendingWeather) == raining && SkyRules.thundering(pendingWeather) == thundering;
			if (!arrived && System.nanoTime() < pendingWeatherUntil) {
				return;
			}
			pendingWeather = -1;
		}
		WeatherData weather = server.getWeatherData();
		if (weather.isRaining() == raining && weather.isThundering() == thundering) {
			return;
		}
		var random = server.overworld().getRandom();
		syncing = true;
		try {
			// Vanilla's durations, for when the link goes and Minecraft's own cycle takes over again.
			server.setWeatherParameters(raining ? 0 : ServerLevel.RAIN_DELAY.sample(random),
				raining ? (thundering ? ServerLevel.THUNDER_DURATION : ServerLevel.RAIN_DURATION).sample(random) : 0, raining, thundering);
		} finally {
			syncing = false;
		}
		LibertyCraft.LOG.info("[LibertyCraft] Minecraft's weather follows GTA IV's {}: {}", weatherName(gtaWeather), thundering ? "thunder" : raining ? "rain" : "clear");
	}

	/** Mixin (ServerClockManager): Minecraft's clock was set, added to or moved to a time marker. */
	public static void clockChanged(ServerClockManager clocks, Holder<WorldClock> clock, String how) {
		if (syncing || !inGame || !Link.active() || !clock.is(WorldClocks.OVERWORLD)) {
			return;
		}
		long ticks = clocks.getInstance(clock).totalTicks();
		float hour = SkyRules.hourOf(ticks);
		Link.pushEvent(Proto.EV_SET_TIME, 0, hour, 0.0F, 0.0F, 0.0F, 0);
		pendingHour = hour;
		pendingHourUntil = System.nanoTime() + PENDING_NANOS;
		LibertyCraft.LOG.info("[LibertyCraft] Minecraft's time {} (tick {}): GTA IV's clock to {}", how, ticks, clockText(hour));
	}

	/** Mixin (MinecraftServer.setWeatherParameters): /weather, or a mod; durations in ticks. */
	public static void weatherSet(int clearTicks, int rainTicks, boolean raining, boolean thundering, String how) {
		if (syncing || !inGame || !Link.active()) {
			return;
		}
		int type = SkyRules.gtaWeatherFor(raining, thundering);
		float seconds = Math.max(0, raining ? rainTicks : clearTicks) / 20.0F;
		Link.pushEvent(Proto.EV_SET_WEATHER, type, seconds, 0.0F, 0.0F, 0.0F, 0);
		pendingWeather = type;
		pendingWeatherUntil = System.nanoTime() + PENDING_NANOS;
		LibertyCraft.LOG.info("[LibertyCraft] Minecraft's weather {}: GTA IV's to {} for {} s", how, weatherName(type), Math.round(seconds));
	}

	static String clockText(float hour) {
		int h = (int) Math.floor(hour) % 24, m = Math.min(59, (int) ((hour - Math.floor(hour)) * 60.0));
		return String.format("%02d:%02d", h, m);
	}

	static String weatherName(int type) {
		String[] names = { "EXTRASUNNY", "SUNNY", "SUNNY_WINDY", "CLOUDY", "RAIN", "DRIZZLE", "FOGGY", "LIGHTNING" };
		return type >= 0 && type < names.length ? names[type] : "weather " + type;
	}
}
