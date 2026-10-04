package dev.libertycraft.kit;

import com.mojang.brigadier.context.CommandContext;
import dev.libertycraft.LibertyCraft;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Properties;
import java.util.Set;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import net.minecraft.core.component.DataComponents;
import net.minecraft.network.chat.Component;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.permissions.Permissions;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.crafting.BrewingRecipe;
import net.minecraft.world.item.crafting.RecipeHolder;

/**
 * Gives the starter kit ({@link StarterKit}): once per player on joining (a fresh world starts with
 * it), and on request with {@code /libertycraft kit}, which only fills empty inventory slots and says
 * what didn't fit. {@code starterKit=false} in config/libertycraft.properties turns the first one off.
 */
public final class KitGiver {
	/** Entity tag: this player has had the kit (so a rejoin doesn't give it again). */
	public static final String TAG = "libertycraft_kit";
	/** Entity tag of the old builder kit: a player from a world made before the kit existed. */
	private static final String OLD_TAG = "libertycraft_builder_kit";

	/** config/libertycraft.properties {@code starterKit}: new players get the kit (default true). */
	public static volatile boolean enabled = true;

	private KitGiver() {
	}

	public static void init() {
		loadConfig();
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> onJoin(handler.getPlayer(), server));
		CommandRegistrationCallback.EVENT.register((dispatcher, registry, environment) -> dispatcher.register(
			Commands.literal("libertycraft").then(Commands.literal("kit").requires(KitGiver::mayUse).executes(KitGiver::command))));
	}

	/** Operators, and in singleplayer the world's owner even without cheats. */
	private static boolean mayUse(CommandSourceStack source) {
		if (source.permissions().hasPermission(Permissions.COMMANDS_GAMEMASTER)) {
			return true;
		}
		ServerPlayer player = source.getPlayer();
		return player != null && source.getServer().isSingleplayerOwner(player.nameAndId());
	}

	private static void onJoin(ServerPlayer player, MinecraftServer server) {
		if (player.entityTags().contains(TAG)) {
			return;
		}
		String name = player.getName().getString();
		if (!enabled) {
			LibertyCraft.LOG.info("[LibertyCraft] starter kit: off (starterKit=false), {} gets none", name);
			return;
		}
		if (player.entityTags().contains(OLD_TAG)) {
			// A world from before the kit: the player already has the old builder kit, and the new one
			// would land on top of whatever they collected since. Say how to get it instead.
			player.addTag(TAG);
			player.sendSystemMessage(Component.literal("LibertyCraft: type /libertycraft kit for the starter kit (nine themed shulker boxes; it only fills empty slots)."));
			LibertyCraft.LOG.info("[LibertyCraft] starter kit: {} is from a world made before it; told them about /libertycraft kit", name);
			return;
		}
		Given given = give(player, server);
		player.addTag(TAG);
		LibertyCraft.LOG.info("[LibertyCraft] starter kit for {} (first join): {}", name, given.summary());
	}

	private static int command(CommandContext<CommandSourceStack> context) throws com.mojang.brigadier.exceptions.CommandSyntaxException {
		ServerPlayer player = context.getSource().getPlayerOrException();
		Given given = give(player, context.getSource().getServer());
		player.addTag(TAG);
		LibertyCraft.LOG.info("[LibertyCraft] starter kit for {} (/libertycraft kit): {}", player.getName().getString(), given.summary());
		int placed = given.placed().size();
		int total = placed + given.left().size();
		context.getSource().sendSuccess(() -> Component.literal("Starter kit: " + placed + " of " + total + " items placed in empty slots."), false);
		if (!given.left().isEmpty()) {
			var names = Component.literal("No empty slot for: ");
			for (int i = 0; i < given.left().size(); i++) {
				names.append(i == 0 ? Component.empty() : Component.literal(", "));
				names.append(given.left().get(i).getHoverName());
			}
			names.append(". Make room and run /libertycraft kit again.");
			context.getSource().sendFailure(names);
		}
		return placed;
	}

	/** What a give placed (slot numbers) and what found no empty slot. */
	record Given(List<Integer> placed, List<ItemStack> left) {
		String summary() {
			List<String> leftNames = new ArrayList<>();
			for (ItemStack stack : left) {
				leftNames.add(stack.has(DataComponents.CUSTOM_NAME) ? stack.getHoverName().getString() : KitPacker.describe(stack));
			}
			return placed.size() + " of " + (placed.size() + left.size()) + " items placed (slots " + placed + ")"
				+ (left.isEmpty() ? "" : "; no empty slot for " + String.join(", ", leftNames));
		}
	}

	/** Builds the kit for this world and puts it in the player's empty slots. */
	static Given give(ServerPlayer player, MinecraftServer server) {
		StarterKit.Kit kit = StarterKit.build(context(server));
		log(kit);
		return place(player.getInventory(), kit.placements());
	}

	/**
	 * Each item into its own slot if that is empty, then the others into the first empty slots (boxes
	 * look in the main inventory first, hotbar items in the hotbar); never onto or into anything
	 * already there.
	 */
	static Given place(Inventory inventory, List<StarterKit.Placement> placements) {
		List<Integer> placed = new ArrayList<>();
		List<ItemStack> left = new ArrayList<>();
		List<StarterKit.Placement> displaced = new ArrayList<>();
		for (StarterKit.Placement placement : placements) {
			if (inventory.getItem(placement.slot()).isEmpty()) {
				inventory.setItem(placement.slot(), placement.stack().copy());
				placed.add(placement.slot());
			} else {
				displaced.add(placement);
			}
		}
		for (StarterKit.Placement placement : displaced) {
			int slot = firstEmpty(inventory, placement.slot() >= Inventory.SELECTION_SIZE);
			if (slot < 0) {
				left.add(placement.stack());
				continue;
			}
			inventory.setItem(slot, placement.stack().copy());
			placed.add(slot);
		}
		inventory.setChanged();
		return new Given(placed, left);
	}

	private static int firstEmpty(Inventory inventory, boolean mainFirst) {
		int size = Inventory.INVENTORY_SIZE;
		int hotbar = Inventory.SELECTION_SIZE;
		for (int pass = 0; pass < 2; pass++) {
			boolean main = mainFirst == (pass == 0);
			for (int slot = main ? hotbar : 0; slot < (main ? size : hotbar); slot++) {
				if (inventory.getItem(slot).isEmpty()) {
					return slot;
				}
			}
		}
		return -1;
	}

	/** One line per box, and what the kit asked for that this game doesn't have. */
	private static void log(StarterKit.Kit kit) {
		for (StarterKit.Box box : kit.boxes()) {
			KitPacker.Result packed = box.packed();
			if (packed.overflow().isEmpty()) {
				LibertyCraft.LOG.info("[LibertyCraft] starter kit box {}", packed.summary());
			} else {
				LibertyCraft.LOG.warn("[LibertyCraft] starter kit box {}", packed.summary());
			}
			if (!packed.duplicates().isEmpty()) {
				LibertyCraft.LOG.info("[LibertyCraft] starter kit box '{}': listed twice, kept once: {}", box.title(), String.join(", ", packed.duplicates()));
			}
		}
		if (!kit.missing().isEmpty()) {
			LibertyCraft.LOG.warn("[LibertyCraft] starter kit: not available here, left out: {}", String.join("; ", kit.missing()));
		}
	}

	/** The registries, feature flags and brewing recipes of the running world. */
	static StarterKit.Context context(MinecraftServer server) {
		List<ItemStack> brewed = new ArrayList<>();
		Set<Item> reagents = new LinkedHashSet<>();
		for (RecipeHolder<?> holder : server.getRecipeManager().getRecipes()) {
			if (holder.value() instanceof BrewingRecipe recipe) {
				brewed.add(recipe.getOutput().create());
				recipe.getReagent().ingredient().items().forEach(item -> reagents.add(item.value()));
			}
		}
		return new StarterKit.Context(server.registryAccess(), server.getWorldData().enabledFeatures(), brewed, new ArrayList<>(reagents));
	}

	// ---- config/libertycraft.properties: starterKit ---------------------------------------------

	private static void loadConfig() {
		Path file = FabricLoader.getInstance().getConfigDir().resolve("libertycraft.properties");
		Properties props = new Properties();
		try {
			if (Files.exists(file)) {
				try (var in = Files.newBufferedReader(file)) {
					props.load(in);
				}
			}
			String on = props.getProperty("starterKit");
			if (on == null) {
				List<String> lines = Files.exists(file) ? new ArrayList<>(Files.readAllLines(file)) : new ArrayList<>(List.of("# LibertyCraft"));
				lines.add("# A new player starts with the starter kit: nine themed shulker boxes and tools in the hotbar");
				lines.add("# (false: an empty inventory; /libertycraft kit still gives it).");
				lines.add("starterKit=true");
				Files.createDirectories(file.getParent());
				Files.write(file, lines);
			}
			enabled = Boolean.parseBoolean(on == null ? "true" : on.trim());
		} catch (IOException e) {
			LibertyCraft.LOG.warn("[LibertyCraft] couldn't read {}", file, e);
		}
		LibertyCraft.LOG.info("[LibertyCraft] starter kit: {} (starterKit in {})", enabled ? "on" : "off", file.getFileName());
	}
}
