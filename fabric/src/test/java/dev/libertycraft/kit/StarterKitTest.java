package dev.libertycraft.kit;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import dev.libertycraft.kit.KitPacker.Bundled;
import dev.libertycraft.kit.KitPacker.Chested;
import dev.libertycraft.kit.KitPacker.Loose;
import dev.libertycraft.kit.KitPacker.Part;
import java.io.IOException;
import java.net.URI;
import java.nio.file.FileSystem;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.stream.Stream;
import net.minecraft.SharedConstants;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.entity.EntityEquipment;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.SpawnEggItem;
import net.minecraft.world.item.alchemy.PotionContents;
import net.minecraft.world.item.component.BundleContents;
import net.minecraft.world.item.component.ItemContainerContents;
import net.minecraft.world.item.crafting.BrewingRecipe;
import net.minecraft.world.item.enchantment.Enchantments;
import org.apache.commons.lang3.math.Fraction;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

/** The starter kit's boxes, built from the vanilla registries without a server. */
class StarterKitTest {
	private static HolderLookup.Provider registries;
	private static StarterKit.Context context;
	private static StarterKit.Kit kit;

	@BeforeAll
	static void bootstrap() throws IOException {
		if (kit != null) {
			return; // already built in this JVM
		}
		SharedConstants.tryDetectVersion();
		Bootstrap.bootStrap();
		registries = VanillaRegistries.createWorldLookup();
		// Item components are bound when a server loads its data; do what it does.
		BuiltInRegistries.DATA_COMPONENT_INITIALIZERS.build(registries).forEach(pending -> pending.apply());
		context = vanillaContext();
		kit = StarterKit.build(context);
		for (StarterKit.Box box : kit.boxes()) {
			System.out.println("box " + box.packed().summary());
		}
		System.out.println("missing: " + kit.missing());
	}

	/** The brewing recipes as the game ships them (the data files in Minecraft's jar). */
	private static StarterKit.Context vanillaContext() throws IOException {
		List<ItemStack> brewed = new ArrayList<>();
		Set<Item> reagents = new LinkedHashSet<>();
		Path jar;
		try {
			jar = Path.of(BrewingRecipe.class.getProtectionDomain().getCodeSource().getLocation().toURI());
		} catch (java.net.URISyntaxException e) {
			throw new IOException(e);
		}
		try (FileSystem fs = FileSystems.newFileSystem(URI.create("jar:" + jar.toUri()), Map.of());
		     Stream<Path> files = Files.list(fs.getPath("data/minecraft/recipe/brewing"))) {
			for (Path file : files.sorted().toList()) {
				JsonObject recipe = JsonParser.parseString(Files.readString(file)).getAsJsonObject();
				JsonObject output = recipe.getAsJsonObject("output");
				Item form = BuiltInRegistries.ITEM.getValue(Identifier.parse(output.get("id").getAsString()));
				String potion = output.getAsJsonObject("components").getAsJsonObject("minecraft:potion_contents").get("potion").getAsString();
				brewed.add(PotionContents.createItemStack(form, BuiltInRegistries.POTION.get(Identifier.parse(potion)).orElseThrow()));
				reagents.add(BuiltInRegistries.ITEM.getValue(Identifier.parse(recipe.getAsJsonObject("reagent").get("item").getAsString())));
			}
		}
		assertFalse(brewed.isEmpty(), "no brewing recipes found in " + jar);
		return new StarterKit.Context(registries, FeatureFlags.DEFAULT_FLAGS, brewed, new ArrayList<>(reagents));
	}

	@Test
	void nineBoxesThatFitAndLoseNothing() {
		assertEquals(9, kit.boxes().size());
		for (StarterKit.Box box : kit.boxes()) {
			KitPacker.Result packed = box.packed();
			assertTrue(packed.slots().size() <= KitPacker.SLOTS, box.title() + " uses " + packed.slots().size() + " slots");
			assertTrue(packed.overflow().isEmpty(), box.title() + " overflows: " + packed.overflow());
			assertTrue(packed.duplicates().isEmpty(), box.title() + " lists items twice: " + packed.duplicates());
			ItemContainerContents contents = box.stack().get(DataComponents.CONTAINER);
			assertNotNull(contents, box.title());
			assertEquals(packed.slots().size(), contents.nonEmptyItemCopyStream().count(), box.title());
			assertTrue(box.stack().has(DataComponents.CUSTOM_NAME), box.title());
		}
		// Box names tell them apart, and so do their colours.
		assertEquals(9, kit.boxes().stream().map(StarterKit.Box::title).distinct().count());
		assertEquals(9, kit.boxes().stream().map(StarterKit.Box::colour).distinct().count());
	}

	@Test
	void everythingInABoxIsDifferent() {
		for (StarterKit.Box box : kit.boxes()) {
			List<ItemStack> leaves = leaves(box.stack());
			for (int i = 0; i < leaves.size(); i++) {
				for (int j = i + 1; j < leaves.size(); j++) {
					assertFalse(ItemStack.isSameItemSameComponents(leaves.get(i), leaves.get(j)),
						box.title() + ": " + KitPacker.describe(leaves.get(i)) + " twice");
				}
			}
			assertEquals(box.packed().leaves().size(), leaves.size(), box.title());
			// The boxes are fairly full: variety, not a few items in a big box.
			assertTrue(box.packed().slots().size() >= 17, box.title() + " only uses " + box.packed().slots().size() + " slots");
		}
	}

	@Test
	void noShulkerBoxInsideABox() {
		for (StarterKit.Box box : kit.boxes()) {
			for (ItemStack leaf : leaves(box.stack())) {
				assertFalse(leaf.getItem() instanceof net.minecraft.world.item.BlockItem block
					&& block.getBlock() instanceof net.minecraft.world.level.block.ShulkerBoxBlock, box.title() + " holds " + KitPacker.describe(leaf));
			}
		}
	}

	@Test
	void bundlesAndChestsHoldWhatTheyCan() {
		for (StarterKit.Box box : kit.boxes()) {
			for (ItemStack slot : box.packed().slots()) {
				BundleContents bundle = slot.get(DataComponents.BUNDLE_CONTENTS);
				if (bundle != null) {
					Fraction weight = bundle.weight().getOrThrow();
					assertTrue(weight.compareTo(Fraction.ONE) <= 0, box.title() + ": " + slot.getHoverName().getString() + " weighs " + weight);
					bundle.itemCopies().forEach(stack -> assertTrue(stack.getMaxStackSize() > 1, "unstackable in a bundle: " + KitPacker.describe(stack)));
				}
				ItemContainerContents chest = kitChest(slot);
				if (chest != null) {
					long n = chest.nonEmptyItemCopyStream().count();
					assertTrue(n > 0 && n <= KitPacker.SLOTS, slot.getHoverName().getString() + " holds " + n);
				}
			}
		}
	}

	@Test
	void everySpawnEggAndEveryBrewedPotion() {
		List<ItemStack> eggs = leaves(box("Spawn Eggs").stack());
		long allEggs = BuiltInRegistries.ITEM.stream().filter(item -> item instanceof SpawnEggItem).count();
		assertEquals(allEggs, eggs.stream().filter(stack -> stack.getItem() instanceof SpawnEggItem).count());
		assertTrue(eggs.stream().anyMatch(stack -> stack.is(Items.SPAWNER)));

		List<ItemStack> potions = leaves(box("Potions & Utility").stack());
		for (ItemStack brewed : context.brewed()) {
			assertTrue(potions.stream().anyMatch(stack -> ItemStack.isSameItemSameComponents(stack, brewed)), "missing " + KitPacker.describe(brewed));
		}
		// Tipped arrows of every effect (the lingering potions that have one; several recipes make some).
		long effects = context.brewed().stream().filter(stack -> stack.is(Items.LINGERING_POTION))
			.filter(stack -> stack.get(DataComponents.POTION_CONTENTS).hasEffects())
			.map(stack -> stack.get(DataComponents.POTION_CONTENTS).potion().orElseThrow().value()).distinct().count();
		long arrows = leaves(box("Combat").stack()).stream().filter(stack -> stack.is(Items.TIPPED_ARROW)).count();
		assertEquals(effects, arrows);
	}

	@Test
	void hotbarHasTheToolsAndTheBoxesFollow() {
		ItemStack silk = placed(0);
		assertTrue(silk.is(Items.NETHERITE_PICKAXE));
		var enchantments = registries.lookupOrThrow(net.minecraft.core.registries.Registries.ENCHANTMENT);
		assertEquals(1, silk.getEnchantments().getLevel(enchantments.getOrThrow(Enchantments.SILK_TOUCH)));
		assertEquals(3, silk.getEnchantments().getLevel(enchantments.getOrThrow(Enchantments.UNBREAKING)));
		assertEquals(5, silk.getEnchantments().getLevel(enchantments.getOrThrow(Enchantments.EFFICIENCY)));
		ItemStack plain = placed(1);
		assertTrue(plain.getItem().toString().contains("pickaxe"));
		assertTrue(plain.getEnchantments().isEmpty());
		assertTrue(kit.placements().stream().anyMatch(p -> p.slot() < 9 && p.stack().is(Items.ENDER_CHEST)));
		for (int i = 0; i < 9; i++) {
			assertSame(kit.boxes().get(i).stack(), placed(StarterKit.FIRST_BOX_SLOT + i));
		}
		// Fireworks for elytra flight fly for 3.
		assertTrue(leaves(box("Travel").stack()).stream().anyMatch(stack -> stack.is(Items.FIREWORK_ROCKET)
			&& stack.get(DataComponents.FIREWORKS).flightDuration() == 3));
	}

	@Test
	void onlyKnownGapsAreMissing() {
		// Nether woods have no boats; the command block minecart is for operators only.
		for (String missing : kit.missing()) {
			assertTrue(missing.startsWith("crimson ") || missing.startsWith("warped ") || missing.startsWith("command_block_minecart"),
				"unexpected: " + missing);
		}
	}

	@Test
	void placingFillsOnlyEmptySlots() {
		Inventory inventory = new Inventory(null, new EntityEquipment());
		ItemStack sword = new ItemStack(Items.WOODEN_SWORD);
		ItemStack dirt = new ItemStack(Items.DIRT, 5);
		inventory.setItem(0, sword);   // where the silk touch pickaxe goes
		inventory.setItem(9, dirt);    // where the first box goes
		for (int slot = 20; slot < Inventory.INVENTORY_SIZE; slot++) {
			inventory.setItem(slot, new ItemStack(Items.COBBLESTONE, 1)); // merging would top these up
		}
		KitGiver.Given given = KitGiver.place(inventory, kit.placements());
		assertSame(sword, inventory.getItem(0));
		assertEquals(Items.DIRT, inventory.getItem(9).getItem());
		assertEquals(5, inventory.getItem(9).getCount());
		for (int slot = 20; slot < Inventory.INVENTORY_SIZE; slot++) {
			assertEquals(1, inventory.getItem(slot).getCount(), "slot " + slot + " was merged into");
		}
		// 36 slots: 18 full of the player's things before, 18 free; the kit has 17 items.
		assertEquals(kit.placements().size(), given.placed().size() + given.left().size());
		assertEquals(Math.min(kit.placements().size(), Inventory.INVENTORY_SIZE - 2 - 16), given.placed().size());
		// Fill the rest: a second give places nothing and reports every item.
		for (int slot = 0; slot < Inventory.INVENTORY_SIZE; slot++) {
			if (inventory.getItem(slot).isEmpty()) {
				inventory.setItem(slot, new ItemStack(Items.STICK));
			}
		}
		KitGiver.Given again = KitGiver.place(inventory, kit.placements());
		assertTrue(again.placed().isEmpty());
		assertEquals(kit.placements().size(), again.left().size());
	}

	@Test
	void packerReportsWhatDoesNotFit() {
		List<Part> parts = new ArrayList<>();
		List<ItemStack> given = new ArrayList<>();
		// 30 loose different items: 3 too many.
		List<Item> blocks = BuiltInRegistries.ITEM.stream().filter(item -> item.getDefaultMaxStackSize() == 64 && item != Items.AIR).limit(30).toList();
		for (Item item : blocks) {
			ItemStack stack = new ItemStack(item, 64);
			parts.add(new Loose(stack));
			given.add(stack);
		}
		// Unstackables can't go in a bundle.
		ItemStack sword = new ItemStack(Items.IRON_SWORD);
		parts.add(new Bundled("Swords", List.of(sword)));
		given.add(sword);
		ItemStack chest = new ItemStack(Items.DIAMOND_SWORD);
		parts.add(new Chested("More", List.of(chest)));
		given.add(chest);
		KitPacker.Result result = KitPacker.pack("test", parts, Items.BUNDLE, Items.CHEST);
		assertEquals(KitPacker.SLOTS, result.slots().size());
		assertEquals(given.size(), result.leaves().size() + result.overflow().size(), "every item is either packed or reported");
		assertTrue(result.overflow().stream().anyMatch(line -> line.contains("iron_sword") && line.contains("bundle")));
		assertTrue(result.overflow().stream().anyMatch(line -> line.contains("diamond_sword")));
	}

	@Test
	void bundleGroupsShareTheFreeSlots() {
		// 2 loose items, 40 kinds in one group and 5 in another: the 5 go loose (full stacks) and the 40
		// share the other 20 slots, two kinds and 32 of each per bundle.
		List<ItemStack> many = BuiltInRegistries.ITEM.stream().filter(item -> item.getDefaultMaxStackSize() == 64 && item != Items.AIR).skip(100).limit(40)
			.map(item -> new ItemStack(item, 64)).toList();
		List<ItemStack> few = BuiltInRegistries.ITEM.stream().filter(item -> item.getDefaultMaxStackSize() == 64 && item != Items.AIR).skip(200).limit(5)
			.map(item -> new ItemStack(item, 64)).toList();
		List<Part> parts = List.of(new Loose(new ItemStack(Items.STONE, 64)), new Loose(new ItemStack(Items.DIRT, 64)), new Bundled("Many", many),
			new Bundled("Few", few));
		KitPacker.Result result = KitPacker.pack("test", parts, Items.BUNDLE, Items.CHEST);
		assertTrue(result.overflow().isEmpty());
		assertEquals(KitPacker.SLOTS, result.slots().size());
		assertEquals(2 + 40 + 5, result.leaves().size());
		for (ItemStack leaf : result.leaves()) {
			assertTrue(leaf.getCount() >= 16, KitPacker.describe(leaf) + " got a thin share");
		}
	}

	@Test
	void copperFamilies() {
		assertEquals("block", StarterKit.copperFamily("copper_block"));
		assertEquals("block", StarterKit.copperFamily("waxed_oxidized_copper"));
		assertEquals("cut slab", StarterKit.copperFamily("waxed_weathered_cut_copper_slab"));
		assertEquals("lightning rod", StarterKit.copperFamily("exposed_lightning_rod"));
		assertEquals("golem statue", StarterKit.copperFamily("copper_golem_statue"));
	}

	private static StarterKit.Box box(String title) {
		return kit.boxes().stream().filter(b -> b.title().equals(title)).findFirst().orElseThrow();
	}

	private static ItemStack placed(int slot) {
		return kit.placements().stream().filter(p -> p.slot() == slot).findFirst().orElseThrow().stack();
	}

	/** The contents of one of the kit's chests (named; a plain chest item has an empty container too). */
	private static ItemContainerContents kitChest(ItemStack slot) {
		return slot.is(Items.CHEST) && slot.has(DataComponents.CUSTOM_NAME) ? slot.get(DataComponents.CONTAINER) : null;
	}

	/** Every item a box holds, including what's in its bundles and chests. */
	private static List<ItemStack> leaves(ItemStack box) {
		List<ItemStack> leaves = new ArrayList<>();
		box.get(DataComponents.CONTAINER).nonEmptyItemCopyStream().forEach(slot -> {
			BundleContents bundle = slot.get(DataComponents.BUNDLE_CONTENTS);
			ItemContainerContents chest = kitChest(slot);
			if (bundle != null) {
				bundle.itemCopies().forEach(leaves::add);
			} else if (chest != null) {
				chest.nonEmptyItemCopyStream().forEach(leaves::add);
			} else {
				leaves.add(slot);
			}
		});
		return leaves;
	}
}
