package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Proto;
import dev.libertycraft.world.city.BlockyCity;
import java.util.Locale;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import org.jspecify.annotations.Nullable;

/**
 * The blocky city, client side. Minecraft stays hidden and linked there like in the mirror world:
 * puppet mode goes on, the world exporter streams the city's sections and GTA IV draws them. GTA IV
 * is told (kMcBlockyCity in McState) so it hides its own map geometry, and the city's own blocks are
 * no obstacles for its peds and vehicles (the exporter leaves the generated blocks out of kRenSolids
 * and kRenLiquids; GTA IV's own collision is that same city). A portal between the mirror world and
 * the city keeps the player where the portal put them (same X/Z): no teleport to GTA IV's player.
 *
 * <p>Render thread only.
 */
public final class CityClient {
	private static volatile boolean inCity;
	private static @Nullable ClientLevel lastLevel;
	// The render distance outside the city (put back on the way out).
	private static int outsideDistance = -1;

	private CityClient() {
	}

	/** The client's level right now is the blocky city. */
	public static boolean inCity() {
		Minecraft minecraft = Minecraft.getInstance();
		return minecraft.level != null && BlockyCity.isCity(minecraft.level);
	}

	/**
	 * Once a frame (HostClient.beginFrame). True when the client's level changed between the mirror
	 * world and the blocky city since the last call: the player went through a portal.
	 */
	static boolean frame(Minecraft minecraft) {
		ClientLevel level = minecraft.level;
		if (level == null || level == lastLevel) {
			return false;
		}
		boolean crossed = lastLevel != null && (BlockyCity.isCity(level) || BlockyCity.isCity(lastLevel));
		lastLevel = level;
		boolean now = BlockyCity.isCity(level);
		if (now != inCity) {
			inCity = now;
			var p = minecraft.player;
			// The city's blocks are all there is to see there: Minecraft loads (and the exporter sends)
			// them further out than the mirror world's few placed blocks need.
			var distance = minecraft.options.renderDistance();
			if (now) {
				outsideDistance = distance.get();
				distance.set(BlockyCity.renderDistance);
			} else if (outsideDistance > 0) {
				distance.set(outsideDistance);
				outsideDistance = -1;
			}
			LibertyCraft.LOG.info("[LibertyCraft] blocky city: {}{}; GTA IV {} its map; render distance {} chunks", now ? "arrived" : "back in the mirror world",
				p != null ? String.format(Locale.ROOT, " at %.1f %.1f %.1f", p.getX(), p.getY(), p.getZ()) : "", now ? "hides" : "shows", distance.get());
		}
		return crossed;
	}

	/** McState flags: kMcBlockyCity while the player is in the city. */
	static int flags() {
		return inCity ? Proto.MC_BLOCKY_CITY : 0;
	}
}
