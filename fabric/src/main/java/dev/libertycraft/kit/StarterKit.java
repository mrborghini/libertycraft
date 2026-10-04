package dev.libertycraft.kit;

import dev.libertycraft.kit.KitPacker.Bundled;
import dev.libertycraft.kit.KitPacker.Chested;
import dev.libertycraft.kit.KitPacker.Loose;
import dev.libertycraft.kit.KitPacker.Part;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.function.Predicate;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.flag.FeatureFlagSet;
import net.minecraft.world.item.DyeColor;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.MinecartItem;
import net.minecraft.world.item.SpawnEggItem;
import net.minecraft.world.item.alchemy.Potion;
import net.minecraft.world.item.alchemy.PotionContents;
import net.minecraft.world.item.component.DyedItemColor;
import net.minecraft.world.item.component.FireworkExplosion;
import net.minecraft.world.item.component.Fireworks;
import net.minecraft.world.item.equipment.trim.ArmorTrim;
import net.minecraft.world.item.equipment.trim.TrimMaterial;
import net.minecraft.world.item.equipment.trim.TrimPattern;
import it.unimi.dsi.fastutil.ints.IntList;
import net.minecraft.world.item.enchantment.Enchantment;
import net.minecraft.world.item.enchantment.Enchantments;
import net.minecraft.world.level.block.state.properties.WoodType;

/**
 * The starter kit: thirteen themed shulker boxes (each up to 27 different items, see {@link KitPacker})
 * and a few things loose in the hotbar, so nobody has to open a box to get going.
 *
 * <p>The lists come from the registries where the game has them (spawn eggs, the potions the
 * brewing recipes make, dye colours, wood types, copper's oxidation and waxed variants), so new
 * items in a later version show up by themselves. Items asked for by name that this version (or
 * this world's feature flags) doesn't have are collected in {@link Kit#missing()} for the log.
 */
public final class StarterKit {
	/** Inventory slot of the first box (the main inventory's top row; the hotbar is 0 to 8). */
	public static final int FIRST_BOX_SLOT = 9;

	/**
	 * What the kit is built from. {@code brewed}: one stack per brewing recipe result (potions in
	 * their three forms); {@code reagents}: the items the brewing recipes add.
	 */
	public record Context(HolderLookup.Provider registries, FeatureFlagSet features, List<ItemStack> brewed, List<Item> reagents) {
	}

	/** A box of the kit: its title, its shulker box item and how it was packed. */
	public record Box(String title, DyeColor colour, ItemStack stack, KitPacker.Result packed) {
	}

	/** An item for a given inventory slot (where it goes if that slot is free). */
	public record Placement(int slot, ItemStack stack) {
	}

	/** The whole kit: the placements (hotbar items, then the boxes), the boxes, what wasn't available. */
	public record Kit(List<Placement> placements, List<Box> boxes, List<String> missing) {
	}

	private final Context context;
	private final List<String> missing = new ArrayList<>();

	private StarterKit(Context context) {
		this.context = context;
	}

	public static Kit build(Context context) {
		return new StarterKit(context).build();
	}

	private Kit build() {
		List<Placement> placements = new ArrayList<>();
		// The hotbar: tools, blocks to bridge with, light, food, a crafting table and an ender chest.
		placements.add(new Placement(0, silkTouchPickaxe()));
		placements.add(new Placement(1, one("diamond_pickaxe")));
		placements.add(new Placement(2, full("cobblestone")));
		placements.add(new Placement(3, full("oak_planks")));
		placements.add(new Placement(4, full("torch")));
		placements.add(new Placement(5, full("cooked_beef")));
		placements.add(new Placement(6, one("crafting_table")));
		placements.add(new Placement(7, one("ender_chest")));
		placements.removeIf(p -> p.stack().isEmpty());

		List<Box> boxes = new ArrayList<>();
		boxes.add(box("Spawn Eggs", DyeColor.YELLOW, spawnEggs()));
		boxes.add(box("Redstone", DyeColor.RED, redstone()));
		boxes.add(box("Travel", DyeColor.LIGHT_BLUE, travel()));
		boxes.add(box("Building", DyeColor.ORANGE, building()));
		boxes.add(box("Combat", DyeColor.BLACK, combat()));
		boxes.add(box("Armor Sets", DyeColor.GRAY, armorSets()));
		boxes.add(box("Enchanted Armor", DyeColor.CYAN, enchantedArmor()));
		boxes.add(box("Trimmed Armor", DyeColor.PINK, trimmedArmor()));
		boxes.add(box("Fireworks", DyeColor.LIME, fireworks()));
		boxes.add(box("Food & Farming", DyeColor.GREEN, food()));
		boxes.add(box("Potions & Utility", DyeColor.MAGENTA, potions()));
		boxes.add(box("Nether & End", DyeColor.PURPLE, netherAndEnd()));
		boxes.add(box("Spares", DyeColor.WHITE, spares()));
		for (int i = 0; i < boxes.size(); i++) {
			placements.add(new Placement(FIRST_BOX_SLOT + i, boxes.get(i).stack()));
		}
		return new Kit(placements, boxes, List.copyOf(new LinkedHashSet<>(missing)));
	}

	// ---- the boxes ----------------------------------------------------------------------------

	private List<Part> spawnEggs() {
		List<Part> parts = new ArrayList<>();
		parts.add(new Loose(of("spawner", 16)));
		List<ItemStack> eggs = new ArrayList<>();
		for (Item item : scan(item -> item instanceof SpawnEggItem, id -> true)) {
			eggs.add(fullOf(item));
		}
		// Each bundle is named after its mobs, so the right one is found without opening them all.
		parts.add(new Bundled("Spawn eggs", eggs, inside -> {
			var name = Component.literal("Eggs: ");
			for (int i = 0; i < inside.size(); i++) {
				name.append(i == 0 ? Component.empty() : Component.literal(", "));
				name.append(SpawnEggItem.getType(inside.get(i)).getDescription());
			}
			return name;
		}));
		return parts;
	}

	private List<Part> redstone() {
		List<Part> parts = loose(
			"redstone", "redstone_torch", "repeater", "comparator", "piston", "sticky_piston", "observer", "dispenser", "dropper", "hopper",
			"note_block", "target", "daylight_detector", "redstone_lamp", "lever", "tripwire_hook", "slime_block", "honey_block",
			"redstone_block", "rail", "powered_rail", "detector_rail", "activator_rail");
		parts.add(new Bundled("Buttons", fulls(scan(id -> id.endsWith("_button")))));
		parts.add(new Bundled("Pressure plates", fulls(scan(id -> id.endsWith("_pressure_plate")))));
		parts.add(new Bundled("Doors", fulls(scan(id -> id.endsWith("_door")))));
		parts.add(new Bundled("Trapdoors", fulls(scan(id -> id.endsWith("_trapdoor")))));
		return parts;
	}

	private List<Part> travel() {
		List<Part> parts = new ArrayList<>();
		parts.add(new Loose(one("saddle")));
		parts.add(new Loose(one("elytra")));
		parts.add(new Loose(full("lead")));
		parts.add(new Loose(full("name_tag")));
		ItemStack rockets = full("firework_rocket");
		if (!rockets.isEmpty()) {
			rockets.set(DataComponents.FIREWORKS, new Fireworks(3, List.of()));
			parts.add(new Loose(rockets));
		}
		parts.add(new Loose(one("red_bed")));
		parts.add(new Loose(one("compass")));
		parts.add(new Loose(one("clock")));
		parts.add(new Loose(one("spyglass")));
		for (Item armour : scan(id -> id.endsWith("_horse_armor"))) {
			parts.add(new Loose(new ItemStack(armour)));
		}
		for (Item cart : scan(item -> item instanceof MinecartItem, id -> true)) {
			if (cart == Items.COMMAND_BLOCK_MINECART) {
				missing.add("command_block_minecart (left out: operators only)");
				continue;
			}
			parts.add(new Loose(new ItemStack(cart)));
		}
		List<ItemStack> boats = new ArrayList<>();
		for (String wood : woods()) {
			boats.add(one(firstOf(wood + " boat", wood + "_boat", wood + "_raft")));
			boats.add(one(firstOf(wood + " chest boat", wood + "_chest_boat", wood + "_chest_raft")));
		}
		parts.add(new Chested("Boats and rafts", boats));
		List<ItemStack> harnesses = new ArrayList<>();
		for (DyeColor colour : DyeColor.values()) {
			harnesses.add(one(colour.getName() + "_harness"));
		}
		parts.add(new Chested("Happy ghast harnesses", harnesses));
		parts.add(new Loose(of("dried_ghast", 4)));
		parts.add(new Loose(one("carrot_on_a_stick")));
		parts.add(new Loose(one("warped_fungus_on_a_stick")));
		parts.add(new Loose(full("ender_pearl")));
		parts.add(new Loose(of("map", 16)));
		return parts;
	}

	private List<Part> building() {
		List<Part> parts = loose("stone", "stone_bricks", "smooth_stone", "deepslate", "cobbled_deepslate", "deepslate_bricks", "glass", "glass_pane", "bricks");
		List<String> woods = woods();
		parts.add(new Chested("Planks and logs", fulls(concat(
			woods.stream().map(w -> w + "_planks").toList(),
			woods.stream().map(w -> firstOf(w + " log", w + "_log", w + "_stem", w + "_block")).toList()))));
		parts.add(new Chested("Wooden slabs and stairs", fulls(concat(
			woods.stream().map(w -> w + "_slab").toList(),
			woods.stream().map(w -> w + "_stairs").toList()))));
		parts.add(new Chested("Fences and fence gates", fulls(concat(
			woods.stream().map(w -> w + "_fence").toList(),
			woods.stream().map(w -> w + "_fence_gate").toList()))));
		parts.add(new Chested("Wool", fulls(colours("%s_wool"))));
		parts.add(new Chested("Concrete", fulls(colours("%s_concrete"))));
		parts.add(new Chested("Terracotta", fulls(concat(List.of("terracotta"), colours("%s_terracotta")))));
		parts.add(new Chested("Glazed terracotta", fulls(colours("%s_glazed_terracotta"))));
		parts.add(new Chested("Stained glass", fulls(concat(colours("%s_stained_glass"), List.of("tinted_glass")))));
		parts.add(new Chested("Stained glass panes", fulls(colours("%s_stained_glass_pane"))));
		parts.addAll(copperChests());
		parts.add(new Chested("Stone", fulls(List.of(
			"granite", "polished_granite", "diorite", "polished_diorite", "andesite", "polished_andesite", "calcite", "tuff", "polished_tuff",
			"tuff_bricks", "chiseled_tuff", "chiseled_tuff_bricks", "mossy_cobblestone", "mossy_stone_bricks", "cracked_stone_bricks",
			"chiseled_stone_bricks", "sandstone", "cut_sandstone", "smooth_sandstone", "red_sandstone", "smooth_red_sandstone", "mud_bricks",
			"packed_mud", "quartz_block", "prismarine", "dark_prismarine", "dripstone_block"))));
		parts.add(new Chested("Deepslate", fulls(List.of(
			"polished_deepslate", "cracked_deepslate_bricks", "deepslate_tiles", "cracked_deepslate_tiles", "chiseled_deepslate",
			"cobbled_deepslate_slab", "cobbled_deepslate_stairs", "cobbled_deepslate_wall", "polished_deepslate_slab", "polished_deepslate_stairs",
			"polished_deepslate_wall", "deepslate_brick_slab", "deepslate_brick_stairs", "deepslate_brick_wall", "deepslate_tile_slab",
			"deepslate_tile_stairs", "deepslate_tile_wall"))));
		parts.add(new Chested("Stone slabs, stairs and walls", fulls(List.of(
			"stone_slab", "stone_stairs", "smooth_stone_slab", "cobblestone_slab", "cobblestone_stairs", "cobblestone_wall", "stone_brick_slab",
			"stone_brick_stairs", "stone_brick_wall", "mossy_stone_brick_slab", "mossy_stone_brick_stairs", "brick_slab", "brick_stairs",
			"brick_wall", "sandstone_slab", "sandstone_stairs", "granite_slab", "diorite_slab", "andesite_slab", "tuff_slab", "tuff_stairs",
			"tuff_brick_slab", "tuff_brick_stairs", "mud_brick_slab", "mud_brick_stairs", "quartz_slab", "quartz_stairs"))));
		return parts;
	}

	/**
	 * Every copper block in all its oxidation stages, waxed and not (copper's waxable pairs), one
	 * family per run of eight; doors and trapdoors are in the redstone box.
	 */
	private List<Part> copperChests() {
		Map<String, List<Item>> families = new LinkedHashMap<>();
		List<Item> all = new ArrayList<>();
		for (var pair : net.minecraft.world.item.HoneycombItem.WAXABLES.get().entrySet()) {
			all.add(pair.getKey().asItem());
			all.add(pair.getValue().asItem());
		}
		// Registry order: the families come out as the creative tab lists them.
		all.sort(java.util.Comparator.comparingInt(BuiltInRegistries.ITEM::getId));
		for (Item item : all) {
			if (item == Items.AIR || !item.isEnabled(context.features())) {
				continue;
			}
			String family = copperFamily(id(item));
			if (family.endsWith("door")) {
				continue;
			}
			families.computeIfAbsent(family, f -> new ArrayList<>()).add(item);
		}
		for (List<Item> family : families.values()) {
			family.sort(java.util.Comparator.comparingInt(item -> copperStage(id(item))));
		}
		List<Part> chests = new ArrayList<>();
		List<ItemStack> run = new ArrayList<>();
		List<String> names = new ArrayList<>();
		for (var family : families.entrySet()) {
			if (run.size() + family.getValue().size() > KitPacker.SLOTS) {
				chests.add(new Chested("Copper: " + String.join(", ", names), run));
				run = new ArrayList<>();
				names = new ArrayList<>();
			}
			run.addAll(fulls(family.getValue()));
			names.add(family.getKey());
		}
		if (!run.isEmpty()) {
			chests.add(new Chested("Copper: " + String.join(", ", names), run));
		}
		return chests;
	}

	/** {@code waxed_weathered_cut_copper_slab} -> "cut slab", {@code oxidized_copper} -> "block". */
	static String copperFamily(String id) {
		String base = id.replaceFirst("^waxed_", "").replaceFirst("^(exposed|weathered|oxidized)_", "");
		if (base.equals("copper") || base.equals("copper_block")) {
			return "block";
		}
		String words = base.replace("copper", "").replace('_', ' ').trim().replaceAll(" +", " ");
		return words.isEmpty() ? base : words;
	}

	/** Unwaxed before waxed, each from fresh to oxidized. */
	private static int copperStage(String id) {
		int stage = id.contains("exposed") ? 1 : id.contains("weathered") ? 2 : id.contains("oxidized") ? 3 : 0;
		return (id.startsWith("waxed_") ? 4 : 0) + stage;
	}

	private List<Part> combat() {
		List<Part> parts = new ArrayList<>();
		// Enchanted weapons loose (the armour has boxes of its own), every tier unenchanted in a chest.
		for (var kind : List.of(Enchantments.SHARPNESS, Enchantments.SMITE, Enchantments.BANE_OF_ARTHROPODS)) {
			parts.add(new Loose(enchanted(one("netherite_sword"), kind.identifier().getPath(), kind, 5, Enchantments.LOOTING, 3, Enchantments.FIRE_ASPECT, 2,
				Enchantments.SWEEPING_EDGE, 3, Enchantments.KNOCKBACK, 2, Enchantments.UNBREAKING, 3, Enchantments.MENDING, 1)));
		}
		parts.add(new Loose(enchanted(one("netherite_axe"), null, Enchantments.SHARPNESS, 5, Enchantments.EFFICIENCY, 5, Enchantments.UNBREAKING, 3,
			Enchantments.MENDING, 1)));
		parts.add(new Loose(enchanted(one("netherite_spear"), null, Enchantments.LUNGE, 3, Enchantments.SHARPNESS, 5, Enchantments.UNBREAKING, 3,
			Enchantments.MENDING, 1)));
		parts.add(new Loose(enchanted(one("mace"), "density", Enchantments.DENSITY, 5, Enchantments.WIND_BURST, 3, Enchantments.FIRE_ASPECT, 2,
			Enchantments.UNBREAKING, 3, Enchantments.MENDING, 1)));
		parts.add(new Loose(enchanted(one("mace"), "breach", Enchantments.BREACH, 4, Enchantments.WIND_BURST, 3, Enchantments.UNBREAKING, 3,
			Enchantments.MENDING, 1)));
		parts.add(new Loose(enchanted(one("trident"), "loyalty", Enchantments.LOYALTY, 3, Enchantments.CHANNELING, 1, Enchantments.IMPALING, 5,
			Enchantments.UNBREAKING, 3, Enchantments.MENDING, 1)));
		parts.add(new Loose(enchanted(one("trident"), "riptide", Enchantments.RIPTIDE, 3, Enchantments.IMPALING, 5, Enchantments.UNBREAKING, 3,
			Enchantments.MENDING, 1)));
		parts.add(new Loose(enchanted(one("bow"), "infinity", Enchantments.POWER, 5, Enchantments.PUNCH, 2, Enchantments.FLAME, 1, Enchantments.INFINITY, 1,
			Enchantments.UNBREAKING, 3)));
		parts.add(new Loose(enchanted(one("bow"), "mending", Enchantments.POWER, 5, Enchantments.PUNCH, 2, Enchantments.FLAME, 1, Enchantments.MENDING, 1,
			Enchantments.UNBREAKING, 3)));
		parts.add(new Loose(enchanted(one("shield"), null, Enchantments.UNBREAKING, 3, Enchantments.MENDING, 1)));
		parts.add(new Loose(one("totem_of_undying")));
		parts.add(new Loose(full("arrow")));
		parts.add(new Loose(full("spectral_arrow")));
		parts.add(new Loose(full("wind_charge")));
		parts.add(new Loose(of("enchanted_golden_apple", 16)));
		parts.add(new Loose(full("golden_apple")));
		parts.add(new Chested("Unenchanted weapons", ones(concat(
			scan(id -> id.endsWith("_sword")), scan(id -> id.endsWith("_axe")), scan(id -> id.endsWith("_spear")),
			items(List.of("mace", "trident", "bow", "crossbow", "shield"))))));
		parts.add(new Chested("Pickaxes, shovels and hoes", ones(concat(
			scan(id -> id.endsWith("_pickaxe")),
			scan(id -> id.endsWith("_shovel")),
			scan(id -> id.endsWith("_hoe"))))));
		List<ItemStack> arrows = new ArrayList<>();
		for (Holder<Potion> potion : brewed(Items.LINGERING_POTION)) {
			if (!potion.value().getEffects().isEmpty()) {
				arrows.add(PotionContents.createItemStack(Items.TIPPED_ARROW, potion).copyWithCount(Items.TIPPED_ARROW.getDefaultMaxStackSize()));
			}
		}
		parts.add(new Bundled("Tipped arrows", arrows));
		return parts;
	}

	private static final List<String> SET = List.of("helmet", "chestplate", "leggings", "boots");

	/** A tier's four pieces ({@code golden} gives golden_helmet, ...). */
	private List<ItemStack> set(String tier) {
		List<ItemStack> pieces = new ArrayList<>();
		for (String piece : SET) {
			ItemStack stack = one(tier + "_" + piece);
			if (!stack.isEmpty()) {
				pieces.add(stack);
			}
		}
		return pieces;
	}

	/** Rows of nine: two sets of four and one more piece per row. */
	private List<Part> armorSets() {
		List<Part> parts = new ArrayList<>();
		addLoose(parts, set("netherite"), set("diamond"), List.of(one("elytra")));
		addLoose(parts, set("iron"), set("golden"), List.of(one("turtle_helmet")));
		addLoose(parts, set("chainmail"), set("leather"), List.of(one("wolf_armor")));
		return parts;
	}

	/**
	 * Netherite sets in each kind of Protection, a diamond set, and some extras. Every piece has
	 * Unbreaking III and Mending; helmets Respiration III and Aqua Affinity, leggings Swift Sneak III,
	 * boots Feather Falling IV with Depth Strider III (Soul Speed III for the nether's fire and blast
	 * sets); the Protection and Blast Protection chestplates have Thorns III.
	 */
	private List<Part> enchantedArmor() {
		List<Part> parts = new ArrayList<>();
		List<ItemStack> protection = enchantedSet("netherite", Enchantments.PROTECTION, false, true);
		List<ItemStack> fire = enchantedSet("netherite", Enchantments.FIRE_PROTECTION, true, false);
		List<ItemStack> blast = enchantedSet("netherite", Enchantments.BLAST_PROTECTION, true, true);
		List<ItemStack> projectile = enchantedSet("netherite", Enchantments.PROJECTILE_PROTECTION, false, false);
		ItemStack elytra = enchanted(one("elytra"), null, Enchantments.UNBREAKING, 3, Enchantments.MENDING, 1);
		ItemStack turtle = enchanted(one("turtle_helmet"), null, Enchantments.PROTECTION, 4, Enchantments.RESPIRATION, 3, Enchantments.AQUA_AFFINITY, 1,
			Enchantments.UNBREAKING, 3, Enchantments.MENDING, 1);
		addLoose(parts, protection, fire, List.of(elytra));
		addLoose(parts, blast, projectile, List.of(turtle));
		List<ItemStack> extras = new ArrayList<>();
		extras.add(enchanted(one("netherite_boots"), "frost_walker", Enchantments.PROTECTION, 4, Enchantments.FEATHER_FALLING, 4, Enchantments.FROST_WALKER, 2,
			Enchantments.UNBREAKING, 3, Enchantments.MENDING, 1));
		for (var book : List.of(Enchantments.MENDING, Enchantments.UNBREAKING, Enchantments.PROTECTION, Enchantments.SWIFT_SNEAK)) {
			extras.add(book(book));
		}
		addLoose(parts, enchantedSet("diamond", Enchantments.PROTECTION, false, false), extras);
		return parts;
	}

	private List<ItemStack> enchantedSet(String tier, ResourceKey<Enchantment> protection, boolean soulSpeed, boolean thorns) {
		String kind = protection.identifier().getPath();
		List<ItemStack> pieces = new ArrayList<>();
		for (ItemStack piece : set(tier)) {
			enchanted(piece, kind, protection, 4, Enchantments.UNBREAKING, 3, Enchantments.MENDING, 1);
			String id = id(piece.getItem());
			if (id.endsWith("_helmet")) {
				enchanted(piece, null, Enchantments.RESPIRATION, 3, Enchantments.AQUA_AFFINITY, 1);
			} else if (id.endsWith("_chestplate") && thorns) {
				enchanted(piece, null, Enchantments.THORNS, 3);
			} else if (id.endsWith("_leggings")) {
				enchanted(piece, null, Enchantments.SWIFT_SNEAK, 3);
			} else if (id.endsWith("_boots")) {
				enchanted(piece, null, Enchantments.FEATHER_FALLING, 4, soulSpeed ? Enchantments.SOUL_SPEED : Enchantments.DEPTH_STRIDER, 3);
			}
			pieces.add(piece);
		}
		return pieces;
	}

	/**
	 * Sets with every armour trim pattern (one each), the trim materials taken in turn, dyed leather
	 * sets, and the smithing templates.
	 */
	private List<Part> trimmedArmor() {
		List<Holder<TrimPattern>> patterns = sortedHolders(Registries.TRIM_PATTERN);
		List<Holder<TrimMaterial>> materials = sortedHolders(Registries.TRIM_MATERIAL);
		if (patterns.isEmpty() || materials.isEmpty()) {
			missing.add("armour trims (no trim patterns or materials in this world)");
		}
		int[] next = {0, 0}; // pattern, material
		java.util.function.BiFunction<List<ItemStack>, Integer, List<ItemStack>> trim = (pieces, dye) -> {
			if (patterns.isEmpty() || materials.isEmpty() || pieces.isEmpty()) {
				return pieces;
			}
			Holder<TrimPattern> pattern = patterns.get(next[0]++ % patterns.size());
			String tier = id(pieces.getFirst().getItem()).replaceFirst("_.*", "").replace("golden", "gold");
			Holder<TrimMaterial> material = materials.get(next[1]++ % materials.size());
			if (key(material).equals(tier)) { // gold on gold looks like nothing: take the next one
				material = materials.get(next[1]++ % materials.size());
			}
			for (ItemStack piece : pieces) {
				piece.set(DataComponents.TRIM, new ArmorTrim(material, pattern));
				if (dye != null) {
					piece.set(DataComponents.DYED_COLOR, new DyedItemColor(dye));
				}
			}
			return pieces;
		};
		List<Part> parts = new ArrayList<>();
		List<ItemStack> templates = fulls(concat(scan(id -> id.endsWith("_armor_trim_smithing_template")), items(List.of("netherite_upgrade_smithing_template"))));
		List<ItemStack> more = new ArrayList<>();
		for (String tier : List.of("netherite", "netherite", "diamond", "diamond", "iron", "golden")) {
			more.addAll(trim.apply(set(tier), null));
		}
		addLoose(parts, trim.apply(set("netherite"), null), trim.apply(set("diamond"), null), List.of());
		parts.add(new Bundled("Smithing templates", templates));
		addLoose(parts, trim.apply(set("iron"), null), trim.apply(set("golden"), null), List.of());
		parts.add(new Chested("More trimmed sets", more));
		addLoose(parts, trim.apply(set("copper"), null), trim.apply(set("chainmail"), null), List.of());
		List<ItemStack> leather = new ArrayList<>();
		for (DyeColor colour : List.of(DyeColor.RED, DyeColor.BLUE, DyeColor.BLACK, DyeColor.WHITE, DyeColor.GREEN, DyeColor.PURPLE)) {
			leather.addAll(trim.apply(set("leather"), colour.getTextureDiffuseColor()));
		}
		parts.add(new Chested("Dyed leather sets", leather));
		return parts;
	}

	/** Shapes for the firework stars, as the crafting recipe makes them (small ball needs no extra). */
	private static final List<FireworkExplosion.Shape> SHAPES = List.of(FireworkExplosion.Shape.SMALL_BALL, FireworkExplosion.Shape.LARGE_BALL,
		FireworkExplosion.Shape.STAR, FireworkExplosion.Shape.CREEPER, FireworkExplosion.Shape.BURST);
	private static final List<DyeColor> BRIGHT = List.of(DyeColor.RED, DyeColor.ORANGE, DyeColor.YELLOW, DyeColor.LIME, DyeColor.GREEN, DyeColor.CYAN,
		DyeColor.LIGHT_BLUE, DyeColor.BLUE, DyeColor.PURPLE, DyeColor.MAGENTA, DyeColor.PINK, DyeColor.WHITE);

	/**
	 * Rockets for the crossbow (GTA IV turns them into rocket blasts, wider with more stars): a row per
	 * flight duration, 1 to 7 stars left to right, each named with its blast radius.
	 */
	private List<Part> fireworks() {
		List<Part> parts = new ArrayList<>();
		List<List<Part>> extras = List.of(
			List.of(new Loose(enchanted(one("crossbow"), "multishot", Enchantments.MULTISHOT, 1, Enchantments.QUICK_CHARGE, 3, Enchantments.UNBREAKING, 3,
					Enchantments.MENDING, 1)),
				new Loose(enchanted(one("crossbow"), "piercing", Enchantments.PIERCING, 4, Enchantments.QUICK_CHARGE, 3, Enchantments.UNBREAKING, 3,
					Enchantments.MENDING, 1))),
			List.of(new Loose(full("gunpowder")), new Loose(full("paper"))),
			List.of(new Bundled("Firework stars: every shape", fireworkStars()),
				new Bundled("Star extras: trail, twinkle, shapes", fulls(List.of("diamond", "glowstone_dust", "fire_charge", "gold_nugget", "feather", "creeper_head")))));
		for (int flight = 1; flight <= 3; flight++) {
			for (int stars = 1; stars <= 7; stars++) {
				parts.add(new Loose(rocket(stars, flight)));
			}
			parts.addAll(extras.get(flight - 1));
		}
		return parts;
	}

	private ItemStack rocket(int stars, int flight) {
		ItemStack rocket = full("firework_rocket");
		if (rocket.isEmpty()) {
			return rocket;
		}
		List<FireworkExplosion> explosions = new ArrayList<>();
		for (int j = 0; j < stars; j++) {
			int k = stars * 3 + flight * 5 + j * 2;
			explosions.add(new FireworkExplosion(SHAPES.get((stars + flight + j) % SHAPES.size()),
				IntList.of(BRIGHT.get(k % BRIGHT.size()).getFireworkColor(), BRIGHT.get((k + 5) % BRIGHT.size()).getFireworkColor()),
				IntList.of(BRIGHT.get((k + 8) % BRIGHT.size()).getFireworkColor()), (j + flight) % 2 == 0, (j + stars) % 3 == 0));
		}
		rocket.set(DataComponents.FIREWORKS, new Fireworks(flight, explosions));
		rocket.set(DataComponents.CUSTOM_NAME, KitPacker.plain(Component.literal(rocketName(stars, flight))));
		return rocket;
	}

	/** "5 stars: 8 m blast (flight 2)": the blast GTA IV makes of it (FireworkBlast). */
	static String rocketName(int stars, int flight) {
		return stars + (stars == 1 ? " star" : " stars") + ": " + Math.round(dev.libertycraft.combat.FireworkBlast.radius(stars)) + " m blast (flight " + flight + ")";
	}

	/** One firework star of each shape, each in its own colours. */
	private List<ItemStack> fireworkStars() {
		List<ItemStack> stars = new ArrayList<>();
		for (int i = 0; i < SHAPES.size(); i++) {
			ItemStack star = full("firework_star");
			if (star.isEmpty()) {
				break;
			}
			star.set(DataComponents.FIREWORK_EXPLOSION, new FireworkExplosion(SHAPES.get(i), IntList.of(BRIGHT.get(i * 2).getFireworkColor()),
				IntList.of(BRIGHT.get((i * 2 + 7) % BRIGHT.size()).getFireworkColor()), i % 2 == 0, i % 2 == 1));
			stars.add(star);
		}
		return stars;
	}

	/** Loose parts from {@code groups} in order (rows of the box when they add up to nine). */
	@SafeVarargs
	private static void addLoose(List<Part> parts, List<ItemStack>... groups) {
		for (List<ItemStack> group : groups) {
			for (ItemStack stack : group) {
				parts.add(new Loose(stack));
			}
		}
	}

	private List<Part> food() {
		List<Part> parts = new ArrayList<>();
		parts.add(new Loose(full("golden_carrot")));
		parts.add(new Loose(full("bone_meal")));
		Set<Item> placed = new java.util.HashSet<>(List.of(Items.GOLDEN_CARROT, Items.BONE_MEAL));
		List<Item> crops = items(List.of("wheat", "carrot", "potato", "beetroot", "pumpkin", "melon", "sugar_cane", "cocoa_beans", "nether_wart",
			"bamboo", "cactus", "kelp", "sweet_berries", "glow_berries", "brown_mushroom", "red_mushroom"));
		List<Item> seeds = concat(scan(id -> id.endsWith("_seeds")), items(List.of("pitcher_pod")));
		List<Item> saplings = concat(scan(id -> id.endsWith("_sapling")), items(List.of("mangrove_propagule", "azalea", "flowering_azalea")));
		placed.addAll(crops);
		placed.addAll(seeds);
		placed.addAll(saplings);
		List<ItemStack> buckets = new ArrayList<>();
		buckets.add(full("bucket"));
		for (Item bucket : scan(id -> id.endsWith("_bucket"))) {
			buckets.add(fullOf(bucket));
			placed.add(bucket); // fish buckets count as food too
		}
		List<ItemStack> foods = new ArrayList<>();
		for (Item item : scan(item -> new ItemStack(item).has(DataComponents.FOOD), id -> true)) {
			if (placed.contains(item)) {
				continue;
			}
			if (item.getDefaultMaxStackSize() == 1) {
				buckets.add(new ItemStack(item)); // stews: no bundle takes them
			} else {
				foods.add(fullOf(item));
			}
		}
		buckets.add(one("cake"));
		parts.add(new Chested("Buckets, stews and cake", buckets));
		parts.add(new Bundled("Crops", fulls(crops)));
		parts.add(new Bundled("Seeds", fulls(seeds)));
		parts.add(new Bundled("Saplings", fulls(saplings)));
		parts.add(new Bundled("Food", foods));
		return parts;
	}

	private List<Part> potions() {
		List<Part> parts = new ArrayList<>();
		parts.add(new Loose(one("brewing_stand")));
		parts.add(new Loose(full("blaze_powder")));
		parts.add(new Loose(one("milk_bucket")));
		parts.add(new Loose(one("anvil")));
		parts.add(new Loose(one("enchanting_table")));
		parts.add(new Loose(of("bookshelf", 15))); // a level 30 enchanting table
		parts.add(new Loose(full("lapis_lazuli")));
		parts.add(new Loose(full("ender_pearl")));
		parts.add(new Loose(full("ender_eye")));
		parts.add(new Loose(full("obsidian")));
		parts.add(new Loose(one("flint_and_steel")));
		parts.add(new Loose(one("shears")));
		parts.add(new Loose(one("fishing_rod")));
		parts.add(new Loose(full("experience_bottle")));
		parts.add(new Loose(full("glass_bottle")));
		for (Item form : List.of(Items.POTION, Items.SPLASH_POTION, Items.LINGERING_POTION)) {
			List<ItemStack> stacks = new ArrayList<>();
			for (Holder<Potion> potion : brewed(form)) {
				stacks.add(PotionContents.createItemStack(form, potion));
			}
			if (stacks.isEmpty()) {
				missing.add(id(form) + " (no brewing recipe makes one)");
			}
			String label = form == Items.POTION ? "Potions" : form == Items.SPLASH_POTION ? "Splash potions" : "Lingering potions";
			parts.add(new Chested(label, stacks));
		}
		List<ItemStack> reagents = new ArrayList<>();
		for (Item reagent : context.reagents()) {
			if (!reagent.isEnabled(context.features()) || reagent == Items.BLAZE_POWDER) {
				continue;
			}
			if (reagent.getDefaultMaxStackSize() == 1) {
				parts.add(new Loose(new ItemStack(reagent))); // the turtle shell: no bundle takes it
			} else {
				reagents.add(fullOf(reagent));
			}
		}
		parts.add(new Bundled("Brewing ingredients", reagents));
		return parts;
	}

	private List<Part> netherAndEnd() {
		List<Part> parts = loose("netherite_scrap", "netherite_ingot", "ancient_debris", "netherrack", "soul_sand", "end_stone", "purpur_block",
			"shulker_shell", "dragon_breath", "blaze_rod", "blaze_powder");
		parts.add(new Loose(of("wither_skeleton_skull", 9)));
		parts.add(new Loose(of("end_crystal", 4)));
		parts.add(new Loose(one("elytra")));
		parts.add(new Loose(of("netherite_upgrade_smithing_template", 8)));
		parts.addAll(loose("quartz", "glowstone_dust", "gold_nugget", "fire_charge"));
		parts.add(new Chested("Nether blocks", fulls(List.of(
			"nether_bricks", "red_nether_bricks", "cracked_nether_bricks", "chiseled_nether_bricks", "nether_brick_fence", "nether_brick_stairs",
			"nether_brick_slab", "nether_brick_wall", "soul_soil", "basalt", "polished_basalt", "smooth_basalt", "crimson_nylium", "warped_nylium",
			"nether_wart_block", "warped_wart_block", "shroomlight", "glowstone", "magma_block", "crying_obsidian", "quartz_bricks",
			"chiseled_quartz_block", "quartz_pillar", "smooth_quartz", "nether_gold_ore", "nether_quartz_ore", "respawn_anchor"))));
		parts.add(new Chested("Blackstone and nether plants", fulls(List.of(
			"blackstone", "polished_blackstone", "polished_blackstone_bricks", "cracked_polished_blackstone_bricks", "chiseled_polished_blackstone",
			"gilded_blackstone", "blackstone_slab", "blackstone_stairs", "blackstone_wall", "polished_blackstone_slab", "polished_blackstone_stairs",
			"polished_blackstone_brick_slab", "polished_blackstone_brick_stairs", "polished_blackstone_brick_wall", "lodestone", "crimson_fungus",
			"warped_fungus", "crimson_roots", "warped_roots", "weeping_vines", "twisting_vines", "nether_sprouts", "ghast_tear", "magma_cream"))));
		parts.add(new Chested("End blocks", fulls(List.of(
			"end_stone_bricks", "end_stone_brick_slab", "end_stone_brick_stairs", "end_stone_brick_wall", "purpur_pillar", "purpur_stairs",
			"purpur_slab", "end_rod", "chorus_fruit", "popped_chorus_fruit", "chorus_flower", "dragon_head"))));
		return parts;
	}

	private List<Part> spares() {
		List<Part> parts = loose("torch", "lantern", "cobblestone", "oak_planks", "oak_log", "dirt", "ladder", "scaffolding", "cooked_beef", "bread",
			"chest", "bucket", "iron_ingot", "gold_ingot", "diamond", "emerald", "coal", "stick", "string", "arrow");
		parts.add(new Loose(of("golden_apple", 16)));
		parts.add(new Loose(one("crafting_table")));
		parts.add(new Loose(one("furnace")));
		parts.add(new Loose(one("water_bucket")));
		parts.add(new Loose(one("lava_bucket")));
		parts.add(new Loose(one("white_bed")));
		parts.add(new Loose(one("flint_and_steel")));
		return parts;
	}

	// ---- building blocks ----------------------------------------------------------------------

	private Box box(String title, DyeColor colour, List<Part> parts) {
		Item bundle = item(colour.getName() + "_bundle").orElse(Items.BUNDLE);
		KitPacker.Result packed = KitPacker.pack(title, parts, bundle, Items.CHEST);
		Item boxItem = item(colour.getName() + "_shulker_box").orElse(Items.SHULKER_BOX);
		ItemStack stack = new ItemStack(boxItem);
		stack.set(DataComponents.CONTAINER, net.minecraft.world.item.component.ItemContainerContents.fromItems(packed.slots()));
		// Bold but in the default colour: the name is also the box screen's title, on light grey, where
		// a yellow or white name would vanish. The box's own colour tells them apart.
		stack.set(DataComponents.CUSTOM_NAME, KitPacker.plain(Component.literal(title).withStyle(s -> s.withBold(true))));
		return new Box(title, colour, stack, packed);
	}

	/** A netherite (or diamond) pickaxe with Silk Touch, Unbreaking III and Efficiency V. */
	private ItemStack silkTouchPickaxe() {
		ItemStack pickaxe = one("netherite_pickaxe");
		if (pickaxe.isEmpty()) {
			pickaxe = one("diamond_pickaxe");
		}
		if (pickaxe.isEmpty()) {
			return pickaxe;
		}
		enchant(pickaxe, Enchantments.SILK_TOUCH, 1);
		enchant(pickaxe, Enchantments.UNBREAKING, 3);
		enchant(pickaxe, Enchantments.EFFICIENCY, 5);
		return pickaxe;
	}

	private void enchant(ItemStack stack, ResourceKey<Enchantment> key, int level) {
		Optional<? extends Holder<Enchantment>> holder = context.registries().lookupOrThrow(Registries.ENCHANTMENT).get(key);
		if (holder.isPresent()) {
			stack.enchant(holder.get(), level);
		} else {
			missing.add("enchantment " + key.identifier().getPath());
		}
	}

	/**
	 * {@code stack} with the enchantments given as key, level pairs; with a {@code kind} (an id such
	 * as fire_protection) it is named "Netherite Helmet (Fire Protection)" to tell it from its twins.
	 */
	@SuppressWarnings("unchecked")
	private ItemStack enchanted(ItemStack stack, @org.jspecify.annotations.Nullable String kind, Object... keysAndLevels) {
		if (stack.isEmpty()) {
			return stack;
		}
		for (int i = 0; i + 1 < keysAndLevels.length; i += 2) {
			enchant(stack, (ResourceKey<Enchantment>) keysAndLevels[i], (Integer) keysAndLevels[i + 1]);
		}
		if (kind != null) {
			String words = Character.toUpperCase(kind.charAt(0)) + kind.substring(1).replace('_', ' ');
			stack.set(DataComponents.CUSTOM_NAME, KitPacker.plain(Component.empty().append(stack.getItemName()).append(" (" + titleCase(words) + ")")));
		}
		return stack;
	}

	private static String titleCase(String words) {
		StringBuilder out = new StringBuilder();
		for (String word : words.split(" ")) {
			if (!out.isEmpty()) {
				out.append(' ');
			}
			out.append(word.equals("of") ? word : Character.toUpperCase(word.charAt(0)) + word.substring(1));
		}
		return out.toString();
	}

	/** An enchanted book with {@code key} at its highest level. */
	private ItemStack book(ResourceKey<Enchantment> key) {
		ItemStack book = one("enchanted_book");
		Optional<? extends Holder<Enchantment>> holder = context.registries().lookupOrThrow(Registries.ENCHANTMENT).get(key);
		if (book.isEmpty() || holder.isEmpty()) {
			missing.add("enchanted book " + key.identifier().getPath());
			return ItemStack.EMPTY;
		}
		var stored = new net.minecraft.world.item.enchantment.ItemEnchantments.Mutable(net.minecraft.world.item.enchantment.ItemEnchantments.EMPTY);
		stored.set(holder.get(), holder.get().value().getMaxLevel());
		book.set(DataComponents.STORED_ENCHANTMENTS, stored.toImmutable());
		return book;
	}

	/** A data-driven registry's entries, by id. */
	private <T> List<Holder<T>> sortedHolders(ResourceKey<? extends net.minecraft.core.Registry<? extends T>> registry) {
		List<Holder<T>> holders = new ArrayList<>();
		context.registries().lookupOrThrow(registry).listElements().forEach(holders::add);
		holders.sort(java.util.Comparator.comparing(StarterKit::key));
		return holders;
	}

	private static String key(Holder<?> holder) {
		return holder.unwrapKey().map(k -> k.identifier().getPath()).orElse("");
	}

	/** The potions the brewing recipes make in {@code form} (potion, splash or lingering), in registry order. */
	private List<Holder<Potion>> brewed(Item form) {
		Map<Potion, Holder<Potion>> potions = new LinkedHashMap<>();
		for (ItemStack stack : context.brewed()) {
			PotionContents contents = stack.get(DataComponents.POTION_CONTENTS);
			if (stack.is(form) && contents != null && contents.potion().isPresent()) {
				potions.putIfAbsent(contents.potion().get().value(), contents.potion().get());
			}
		}
		List<Holder<Potion>> sorted = new ArrayList<>(potions.values());
		sorted.sort(java.util.Comparator.comparingInt(h -> BuiltInRegistries.POTION.getId(h.value())));
		return sorted;
	}

	private List<Part> loose(String... ids) {
		List<Part> parts = new ArrayList<>();
		for (String id : ids) {
			parts.add(new Loose(full(id)));
		}
		return parts;
	}

	/** Every wood type's name (oak, ..., bamboo), as the game lists them. */
	private static List<String> woods() {
		return WoodType.values().map(WoodType::name).map(n -> n.contains(":") ? n.substring(n.indexOf(':') + 1) : n).toList();
	}

	/** {@code pattern} filled with every dye colour's name. */
	private static List<String> colours(String pattern) {
		List<String> ids = new ArrayList<>();
		for (DyeColor colour : DyeColor.values()) {
			ids.add(pattern.formatted(colour.getName()));
		}
		return ids;
	}

	/** The first of {@code ids} the game has; {@code what} goes to the missing list when it has none. */
	private String firstOf(String what, String... ids) {
		for (String id : ids) {
			if (BuiltInRegistries.ITEM.containsKey(Identifier.withDefaultNamespace(id))) {
				return id;
			}
		}
		missing.add(what + " (none in this version)");
		return "";
	}

	/** Enabled vanilla items whose id matches, in registry order. */
	private List<Item> scan(Predicate<String> id) {
		return scan(item -> true, id);
	}

	private List<Item> scan(Predicate<Item> item, Predicate<String> id) {
		List<Item> found = new ArrayList<>();
		for (Item candidate : BuiltInRegistries.ITEM) {
			Identifier key = BuiltInRegistries.ITEM.getKey(candidate);
			if (key.getNamespace().equals(Identifier.DEFAULT_NAMESPACE) && candidate != Items.AIR && candidate.isEnabled(context.features())
				&& item.test(candidate) && id.test(key.getPath())) {
				found.add(candidate);
			}
		}
		return found;
	}

	private Optional<Item> item(String id) {
		if (id.isEmpty()) {
			return Optional.empty();
		}
		Optional<Item> item = BuiltInRegistries.ITEM.getOptional(Identifier.withDefaultNamespace(id)).filter(i -> i != Items.AIR);
		if (item.isEmpty()) {
			missing.add(id + " (not in this version)");
		} else if (!item.get().isEnabled(context.features())) {
			missing.add(id + " (not enabled in this world)");
			return Optional.empty();
		}
		return item;
	}

	private List<Item> items(List<String> ids) {
		List<Item> found = new ArrayList<>();
		for (String id : ids) {
			item(id).ifPresent(found::add);
		}
		return found;
	}

	private ItemStack of(String id, int count) {
		return item(id).map(item -> new ItemStack(item, Math.min(count, item.getDefaultMaxStackSize()))).orElse(ItemStack.EMPTY);
	}

	private ItemStack one(String id) {
		return of(id, 1);
	}

	private ItemStack full(String id) {
		return item(id).map(StarterKit::fullOf).orElse(ItemStack.EMPTY);
	}

	private static ItemStack fullOf(Item item) {
		return new ItemStack(item, item.getDefaultMaxStackSize());
	}

	private List<ItemStack> fulls(List<?> itemsOrIds) {
		List<ItemStack> stacks = new ArrayList<>();
		for (Object o : itemsOrIds) {
			ItemStack stack = o instanceof Item item ? fullOf(item) : full((String) o);
			if (!stack.isEmpty()) {
				stacks.add(stack);
			}
		}
		return stacks;
	}

	private static List<ItemStack> ones(List<Item> items) {
		List<ItemStack> stacks = new ArrayList<>();
		for (Item item : items) {
			stacks.add(new ItemStack(item));
		}
		return stacks;
	}

	@SafeVarargs
	private static <T> List<T> concat(List<? extends T>... lists) {
		List<T> all = new ArrayList<>();
		for (List<? extends T> list : lists) {
			all.addAll(list);
		}
		return all;
	}

	private static String id(Item item) {
		return BuiltInRegistries.ITEM.getKey(item).getPath();
	}
}
