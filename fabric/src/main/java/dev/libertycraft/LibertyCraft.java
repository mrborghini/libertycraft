// LibertyCraft: Minecraft inside GTA IV. MIT licensed (see the repository's LICENSE).
// A port of SkyCraft by chasmlol (MIT, https://github.com/chasmlol/SkyCraft); most of this mod is
// SkyCraft's code with Skyrim swapped for GTA IV.
package dev.libertycraft;

import dev.libertycraft.combat.HostCombat;
import net.fabricmc.api.ModInitializer;
import net.minecraft.world.entity.EquipmentSlot;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.gamerules.GameRules;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public final class LibertyCraft implements ModInitializer {
	public static final String MOD_ID = "libertycraft";
	public static final String WORLD_NAME = "LibertyCraft";
	public static final Logger LOG = LoggerFactory.getLogger(MOD_ID);

	@Override
	public void onInitialize() {
		HostCombat.init();
		dev.libertycraft.net.LcNet.init();
		dev.libertycraft.world.HostDig.init();
		dev.libertycraft.world.HostDrive.init();
		dev.libertycraft.world.city.BlockyCity.init();
		ServerLifecycleEvents.SERVER_STARTED.register(LibertyCraft::configureServer);
		dev.libertycraft.kit.KitGiver.init(); // the starter kit: on first join, and /libertycraft kit
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> dressTestGuest(handler.getPlayer()));
	}

	/** The mirror world is a void that only exists to host the player; GTA IV drives time and spawning. */
	private static void configureServer(MinecraftServer server) {
		GameRules rules = server.getGameRules();
		rules.set(GameRules.ADVANCE_TIME, false, server);
		rules.set(GameRules.ADVANCE_WEATHER, false, server);
		rules.set(GameRules.SPAWN_MOBS, false, server);
		rules.set(GameRules.SPAWN_MONSTERS, false, server);
		rules.set(GameRules.SPAWN_PHANTOMS, false, server);
		rules.set(GameRules.SPAWN_PATROLS, false, server);
		rules.set(GameRules.SPAWN_WANDERING_TRADERS, false, server);
		rules.set(GameRules.PLAYER_MOVEMENT_CHECK, false, server);
		defaultRules(server, rules);
		rules.set(GameRules.IMMEDIATE_RESPAWN, true, server);
		rules.set(GameRules.SHOW_ADVANCEMENT_MESSAGES, false, server);
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set noon");
		LOG.info("[LibertyCraft] mirror world configured");
	}

	private static final String DEFAULTS_FILE = "libertycraft_defaults.properties";

	/**
	 * Game rules LibertyCraft only sets once per world, as defaults the player can change with
	 * /gamerule (the rules above are the mirror world's and set on every start): keepInventory, so a
	 * death in Liberty City (GTA IV kills the player too) never costs the starter kit. Worlds from
	 * before this get it once as well. Remembered in the world's libertycraft_defaults.properties.
	 */
	private static void defaultRules(MinecraftServer server, GameRules rules) {
		java.nio.file.Path file = server.getWorldPath(net.minecraft.world.level.storage.LevelResource.ROOT).resolve(DEFAULTS_FILE);
		java.util.Properties done = new java.util.Properties();
		if (java.nio.file.Files.isRegularFile(file)) {
			try (var in = java.nio.file.Files.newBufferedReader(file)) {
				done.load(in);
			} catch (java.io.IOException e) {
				LOG.warn("[LibertyCraft] couldn't read {}", file, e);
			}
		}
		if (done.getProperty("keepInventory") == null) {
			rules.set(GameRules.KEEP_INVENTORY, true, server);
			done.setProperty("keepInventory", "set");
			try {
				java.nio.file.Files.writeString(file, "# LibertyCraft: game rule defaults already given to this world (change them with /gamerule)\nkeepInventory=set\n");
			} catch (java.io.IOException e) {
				LOG.warn("[LibertyCraft] couldn't write {}", file, e);
			}
			LOG.info("[LibertyCraft] keepInventory on for this world (a default: /gamerule keep_inventory false turns it off)");
		} else {
			LOG.info("[LibertyCraft] keepInventory is {} in this world (/gamerule keep_inventory)", rules.get(GameRules.KEEP_INVENTORY));
		}
	}

	/**
	 * Local multiplayer test guests (tools/fake_guest.py; named Guest, Guest2, ...) wear a random
	 * mix of iron and diamond armour, so they're easy to tell apart.
	 */
	private static void dressTestGuest(ServerPlayer player) {
		if (!player.getName().getString().startsWith("Guest")) {
			return;
		}
		var random = player.getRandom();
		EquipmentSlot[] slots = { EquipmentSlot.HEAD, EquipmentSlot.CHEST, EquipmentSlot.LEGS, EquipmentSlot.FEET };
		net.minecraft.world.item.Item[][] pieces = {
			{ Items.IRON_HELMET, Items.DIAMOND_HELMET },
			{ Items.IRON_CHESTPLATE, Items.DIAMOND_CHESTPLATE },
			{ Items.IRON_LEGGINGS, Items.DIAMOND_LEGGINGS },
			{ Items.IRON_BOOTS, Items.DIAMOND_BOOTS },
		};
		for (int i = 0; i < slots.length; i++) {
			player.setItemSlot(slots[i], new ItemStack(pieces[i][random.nextBoolean() ? 1 : 0]));
		}
		LOG.info("[LibertyCraft] dressed test guest {} in iron and diamond", player.getName().getString());
	}
}
