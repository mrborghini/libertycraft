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
 * # from ingame         the delay counts from when GTA IV is in game (not loading) instead of from linking
 * # wait 10             the commands below run 10 seconds later (as many waits as needed)
 * # screen inventory    the client opens the inventory (or "chat", "pause"; "none" closes it)
 * # hotbar 3            the client selects hotbar slot 3 (1 to 9)
 * # view third          the camera goes to third person (behind); "first" back to first person
 * tp @s ~ ~ ~4
 * ? execute if block ~ ~-1 ~ minecraft:glass    "?": the command's feedback goes to the log (a check)
 * </pre>
 */
public final class DevAutorun {
	private static final String FILE = "libertycraft-autorun.txt";

	private static boolean done;
	private static long linkedSince;
	private static final String WAIT = "\u0000wait ";
	private static final String CLIENT = "\u0000client ";
	// After a "# wait": the commands still to run, and when the next ones are due.
	private static List<String> pending;
	private static long pendingAt;

	private DevAutorun() {
	}

	/** Called every client tick from {@link HostClient#clientTick}. */
	static void tick(Minecraft minecraft, boolean linked) {
		if (pending != null && minecraft.player != null && minecraft.getSingleplayerServer() != null && System.currentTimeMillis() >= pendingAt) {
			run(minecraft, pending);
		}
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
		boolean fromIngame = false;
		try {
			for (String raw : Files.readAllLines(file)) {
				String line = raw.strip();
				if (line.startsWith("# delay ")) {
					delayMs = (long) (Double.parseDouble(line.substring(8).strip()) * 1000);
				} else if (line.equals("# camera third")) {
					thirdPerson = true;
				} else if (line.equals("# from ingame")) {
					fromIngame = true;
				} else if (line.startsWith("# wait ")) {
					commands.add(WAIT + (long) (Double.parseDouble(line.substring(7).strip()) * 1000));
				} else if (line.startsWith("# screen ") || line.startsWith("# hotbar ") || line.startsWith("# view ")) {
					commands.add(CLIENT + line.substring(2));
				} else if (line.startsWith("? ")) {
					commands.add(line); // a check: its feedback is logged
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
		if (fromIngame && (!HostClient.sky().inGame() || HostClient.sky().loading())) {
			linkedSince = 0;
			return;
		}
		if (linkedSince == 0) {
			linkedSince = now;
		}
		if (now - linkedSince < delayMs) {
			return;
		}
		done = true;
		LibertyCraft.LOG.info("[LibertyCraft] autorun: {} command(s) from {}{}", commands.size(), file.getFileName(), thirdPerson ? ", camera third person" : "");
		if (thirdPerson) {
			minecraft.options.setCameraType(net.minecraft.client.CameraType.THIRD_PERSON_BACK);
		}
		run(minecraft, commands);
	}

	/** A client-side step: a screen opened or closed, a hotbar slot selected. */
	private static void client(Minecraft minecraft, String step) {
		LibertyCraft.LOG.info("[LibertyCraft] autorun: {}", step);
		var player = minecraft.player;
		switch (step) {
			case "screen inventory" -> minecraft.gui.setScreen(new net.minecraft.client.gui.screens.inventory.InventoryScreen(player));
			case "screen chat" -> minecraft.gui.setScreen(new net.minecraft.client.gui.screens.ChatScreen("hello from the autorun", false));
			case "screen pause" -> minecraft.gui.setScreen(new net.minecraft.client.gui.screens.PauseScreen(true));
			case "screen none" -> minecraft.gui.setScreen(null);
			case "view third" -> minecraft.options.setCameraType(net.minecraft.client.CameraType.THIRD_PERSON_BACK);
			case "view first" -> minecraft.options.setCameraType(net.minecraft.client.CameraType.FIRST_PERSON);
			default -> {
				if (step.startsWith("hotbar ")) {
					player.getInventory().setSelectedSlot(Math.clamp(Integer.parseInt(step.substring(7).strip()) - 1, 0, 8));
				} else {
					LibertyCraft.LOG.warn("[LibertyCraft] autorun: unknown step {}", step);
				}
			}
		}
	}

	/** Runs commands up to the next "# wait" (the rest stays pending for later). */
	private static void run(Minecraft minecraft, List<String> commands) {
		var server = minecraft.getSingleplayerServer();
		String who = minecraft.player.getUUID().toString();
		List<String> now = new ArrayList<>();
		pending = null;
		for (int i = 0; i < commands.size(); i++) {
			String command = commands.get(i);
			if (command.startsWith(WAIT)) {
				pending = new ArrayList<>(commands.subList(i + 1, commands.size()));
				pendingAt = System.currentTimeMillis() + Long.parseLong(command.substring(WAIT.length()));
				LibertyCraft.LOG.info("[LibertyCraft] autorun: waiting {} s, then {} more command(s)", Long.parseLong(command.substring(WAIT.length())) / 1000.0, pending.size());
				break;
			}
			if (command.startsWith(CLIENT)) {
				client(minecraft, command.substring(CLIENT.length()));
				continue;
			}
			now.add(command);
		}
		server.execute(() -> {
			var quiet = server.createCommandSourceStack().withSuppressedOutput();
			var loud = server.createCommandSourceStack();
			for (String command : now) {
				boolean check = command.startsWith("? ");
				String text = check ? command.substring(2).strip() : command;
				LibertyCraft.LOG.info("[LibertyCraft] autorun{}: {}", check ? " check" : "", text);
				server.getCommands().performPrefixedCommand(check ? loud : quiet, "execute as " + who + " at @s run " + text);
			}
		});
	}
}
