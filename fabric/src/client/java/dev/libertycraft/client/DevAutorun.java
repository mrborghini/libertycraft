package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;

/**
 * Development helper: runs the Minecraft commands listed in {@code config/libertycraft-autorun.txt}
 * once per session, a few seconds after GTA IV has linked and the player is in the world. Nothing
 * happens when the file doesn't exist, so players never see it.
 *
 * <p>It exists for unattended tests and README screenshots: nothing can press keys in those runs,
 * so builds are placed with commands instead. Each command runs as the player, at the player and
 * with the player's facing ({@code execute as <player> at @s run ...}), so {@code ~} and {@code ^}
 * coordinates are relative to where the player stands and looks.
 *
 * <pre>
 * # delay 20            seconds after linking before the first command (default 5)
 * # camera third         then the camera goes to third person (behind), as F5 would
 * gamemode creative
 * fill ^-2 ^ ^5 ^2 ^4 ^9 minecraft:oak_planks hollow
 * </pre>
 */
public final class DevAutorun {
	private static final String FILE = "libertycraft-autorun.txt";

	private static boolean done;
	private static long linkedSince;

	private DevAutorun() {
	}

	/** Called every client tick from {@link HostClient#clientTick}. */
	static void tick(Minecraft minecraft, boolean linked) {
		if (done) {
			return;
		}
		var player = minecraft.player;
		var server = minecraft.getSingleplayerServer();
		if (!linked || player == null || minecraft.level == null || server == null) {
			linkedSince = 0;
			return;
		}
		Path file = FabricLoader.getInstance().getConfigDir().resolve(FILE);
		if (!Files.isRegularFile(file)) {
			done = true;
			return;
		}
		List<String> commands = new ArrayList<>();
		long delayMs = 5000;
		boolean thirdPerson = false;
		try {
			for (String raw : Files.readAllLines(file)) {
				String line = raw.strip();
				if (line.startsWith("# delay ")) {
					delayMs = (long) (Double.parseDouble(line.substring(8).strip()) * 1000);
				} else if (line.equals("# camera third")) {
					thirdPerson = true;
				} else if (!line.isEmpty() && !line.startsWith("#")) {
					commands.add(line.startsWith("/") ? line.substring(1) : line);
				}
			}
		} catch (IOException | NumberFormatException e) {
			LibertyCraft.LOG.warn("[LibertyCraft] couldn't read {}: {}", file, e.toString());
			done = true;
			return;
		}
		long now = System.currentTimeMillis();
		if (linkedSince == 0) {
			linkedSince = now;
		}
		if (now - linkedSince < delayMs) {
			return;
		}
		done = true;
		String who = player.getUUID().toString();
		LibertyCraft.LOG.info("[LibertyCraft] autorun: {} command(s) from {}{}", commands.size(), file.getFileName(), thirdPerson ? ", camera third person" : "");
		if (thirdPerson) {
			minecraft.options.setCameraType(net.minecraft.client.CameraType.THIRD_PERSON_BACK);
		}
		server.execute(() -> {
			var source = server.createCommandSourceStack().withSuppressedOutput();
			for (String command : commands) {
				server.getCommands().performPrefixedCommand(source, "execute as " + who + " at @s run " + command);
			}
		});
	}
}
