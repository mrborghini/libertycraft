package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Pose;

/**
 * Logs why the player crouches when it isn't the sneak key: Minecraft crouches a player who can't stand where
 * they are (a ceiling or a solid stand-in less than 1.8 blocks over the feet; vanilla's rule, LocalPlayer.aiStep),
 * which a hand-over from GTA IV (after a cutscene or a mission scene, Niko where GTA's own collision has room)
 * can land them in. One line per change, with where and whether GTA IV drives the player.
 */
final class CrouchWatch {
	private static boolean crouching;

	private CrouchWatch() {
	}

	static void tick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		boolean now = player != null && player.isCrouching();
		if (now == crouching) {
			return;
		}
		crouching = now;
		if (player == null) {
			return;
		}
		boolean key = player.isShiftKeyDown();
		// (vanilla's own test, Player.canPlayerFitWithinBlocksAndEntitiesWhen, is protected)
		boolean canStand = player.level().noCollision(player, player.getDimensions(Pose.STANDING).makeBoundingBox(player.position()).deflate(1.0E-7));
		LibertyCraft.LOG.info("[LibertyCraft] the player {} at {} {} {} ({}; sneak key {}, room to stand {}, GTA IV drives {})", now ? "crouches" : "stands up",
			String.format("%.2f", player.getX()), String.format("%.2f", player.getY()), String.format("%.2f", player.getZ()),
			now ? (key ? "the sneak key" : "no room to stand there") : "", key ? "down" : "up", canStand, HostDriveClient.driving());
	}
}
