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
	private static final String KIT2_TAG = "libertycraft_builder_kit";

	@Override
	public void onInitialize() {
		HostCombat.init();
		dev.libertycraft.net.LcNet.init();
		dev.libertycraft.world.HostDig.init();
		dev.libertycraft.world.HostDrive.init();
		dev.libertycraft.world.city.BlockyCity.init();
		ServerLifecycleEvents.SERVER_STARTED.register(LibertyCraft::configureServer);
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
			giveStarterKit(handler.getPlayer());
			giveBuilderKit(handler.getPlayer());
			dressTestGuest(handler.getPlayer());
		});
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
		rules.set(GameRules.KEEP_INVENTORY, true, server);
		rules.set(GameRules.IMMEDIATE_RESPAWN, true, server);
		rules.set(GameRules.SHOW_ADVANCEMENT_MESSAGES, false, server);
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set noon");
		LOG.info("[LibertyCraft] mirror world configured");
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

	private static void giveStarterKit(ServerPlayer player) {
		if (!player.getInventory().isEmpty()) {
			return;
		}
		player.getInventory().add(new ItemStack(Items.DIAMOND_SWORD));
		player.getInventory().add(new ItemStack(Items.DIAMOND_PICKAXE));
		player.getInventory().add(new ItemStack(Items.BOW));
		player.getInventory().add(new ItemStack(Items.COOKED_BEEF, 32));
		player.getInventory().add(new ItemStack(Items.OAK_PLANKS, 64));
		player.getInventory().add(new ItemStack(Items.TORCH, 32));
		player.getInventory().add(new ItemStack(Items.ARROW, 64));
		player.setItemSlot(net.minecraft.world.entity.EquipmentSlot.OFFHAND, new ItemStack(Items.SHIELD));
		LOG.info("[LibertyCraft] gave starter kit to {}", player.getName().getString());
	}

	/**
	 * Once per player: armor (GTA IV's enemies hit back now) and building materials, since there is
	 * no Minecraft terrain to mine in GTA IV.
	 */
	private static void giveBuilderKit(ServerPlayer player) {
		if (player.entityTags().contains(KIT2_TAG)) {
			return;
		}
		equipIfEmpty(player, EquipmentSlot.HEAD, Items.IRON_HELMET);
		equipIfEmpty(player, EquipmentSlot.CHEST, Items.IRON_CHESTPLATE);
		equipIfEmpty(player, EquipmentSlot.LEGS, Items.IRON_LEGGINGS);
		equipIfEmpty(player, EquipmentSlot.FEET, Items.IRON_BOOTS);
		var inventory = player.getInventory();
		inventory.add(new ItemStack(Items.COBBLESTONE, 64));
		inventory.add(new ItemStack(Items.STONE_BRICKS, 64));
		inventory.add(new ItemStack(Items.OAK_LOG, 64));
		inventory.add(new ItemStack(Items.GLASS, 64));
		inventory.add(new ItemStack(Items.OAK_STAIRS, 64));
		inventory.add(new ItemStack(Items.OAK_SLAB, 64));
		inventory.add(new ItemStack(Items.OAK_DOOR, 8));
		inventory.add(new ItemStack(Items.LADDER, 32));
		inventory.add(new ItemStack(Items.LANTERN, 16));
		inventory.add(new ItemStack(Items.CRAFTING_TABLE));
		inventory.add(new ItemStack(Items.WATER_BUCKET));
		inventory.add(new ItemStack(Items.ARROW, 64));
		inventory.add(new ItemStack(Items.GOLDEN_APPLE, 4));
		player.addTag(KIT2_TAG);
		LOG.info("[LibertyCraft] gave builder kit to {}", player.getName().getString());
	}

	private static void equipIfEmpty(ServerPlayer player, EquipmentSlot slot, net.minecraft.world.item.Item item) {
		if (player.getItemBySlot(slot).isEmpty()) {
			player.setItemSlot(slot, new ItemStack(item));
		} else {
			player.getInventory().add(new ItemStack(item));
		}
	}
}
