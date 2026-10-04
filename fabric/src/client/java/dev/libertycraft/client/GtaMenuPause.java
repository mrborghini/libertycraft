package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import net.minecraft.client.Minecraft;

/**
 * GTA IV's pause menu pauses Minecraft too. While GTA IV shows it (SkyState's kSkyMenuOpen, however it
 * opened: Esc, the window losing the focus, a test hook) a singleplayer world stands still the way vanilla's
 * pause screen stops it: Minecraft.runTick's pause test counts GTA IV's menu as a pausing screen
 * (GtaMenuPauseMixin), so the integrated server stops ticking (vanilla saves then: "Saving and pausing
 * game...") and so does the client's level. No Minecraft screen opens, so none is left behind. Esc itself
 * stays GTA IV's: the plugin never hands it to Minecraft unless a Minecraft screen is open.
 *
 * <p>Sounds pause with it, music too (vanilla keeps music playing under its own pause screen; GTA IV's menu
 * has its own), and resume when GTA IV's menu closes (vanilla resumes them when its pause screen closes,
 * and none was opened). A world opened to friends doesn't pause (vanilla's rule): there the plugin ignores
 * Minecraft's combat events while its menu is up.
 *
 * <p>GTA IV's scenes pause Minecraft the same way (SkyState's kSkyScene: a cutscene, or a mission script's own
 * camera): its mobs stand still, their blasts and sounds wait, so a creeper can't blow up a mission's
 * characters while the player watches and the cutscene's dialogue isn't drowned out. The hostile mobs
 * aren't drawn in GTA IV's picture meanwhile (AvatarExporter).
 */
public final class GtaMenuPause {
	private static boolean active;
	private static boolean scene;
	private static String cause = "";

	private GtaMenuPause() {
	}

	/** True while GTA IV's pause menu is open or it shows a scene (and linked). */
	public static boolean active() {
		return active;
	}

	/** True while GTA IV shows a cutscene or a mission script's camera (and linked). */
	public static boolean scene() {
		return scene;
	}

	/** Every frame from HostClient.beginFrame, after the SkyState read. */
	static void frame(Minecraft minecraft, boolean linked, Link.SkyState sky) {
		boolean menu = linked && sky.menuOpen();
		scene = linked && sky.scene() && !sky.loading();
		boolean want = menu || scene;
		if (want == active) {
			return;
		}
		active = want;
		if (want) {
			minecraft.getSoundManager().pauseAllExcept();
		} else {
			minecraft.getSoundManager().resume();
		}
		boolean pausable = minecraft.hasSingleplayerServer() && !minecraft.getSingleplayerServer().isPublished();
		if (want) {
			cause = menu ? "GTA IV's pause menu opened" : "GTA IV shows a scene (a cutscene or a mission's camera)";
			LibertyCraft.LOG.info("[LibertyCraft] {}: {}", cause, pausable ? "Minecraft pauses too, sounds paused" : "sounds paused (a world open to friends keeps running)");
		} else {
			LibertyCraft.LOG.info("[LibertyCraft] over ({}): Minecraft goes on, sounds resumed", cause);
		}
	}
}
