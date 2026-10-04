package dev.libertycraft.kit;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Function;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.chat.Component;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.alchemy.PotionContents;
import net.minecraft.world.item.component.BundleContents;
import net.minecraft.world.item.component.ItemContainerContents;
import net.minecraft.world.item.component.ItemLore;
import org.jspecify.annotations.Nullable;

/**
 * Packs one box of the starter kit: at most 27 slots, every item in it different from the others.
 *
 * <p>An item goes in loose (a slot of its own, as given: a full stack), in a bundle (many different
 * stackable items in one slot, a share of the bundle's 64 each) or in a chest (up to 27 stacks in one
 * slot; the player places the chest to unpack it). Bundle groups first get as few bundles as they
 * need, then the box's free slots: each free slot goes to the group whose fullest bundle holds the
 * most different items, and a group that fits loose in what is left is unbundled (full stacks).
 *
 * <p>Nothing is dropped quietly: what doesn't fit (or can't go in a bundle) ends up in
 * {@link Result#overflow()}, which the caller logs.
 */
public final class KitPacker {
	public static final int SLOTS = 27;
	// A bundle holds 64 "units": one unit per item of a 64-stack, four per item of a 16-stack.
	private static final int BUNDLE_UNITS = 64;

	private KitPacker() {
	}

	/** Part of a box, in the order its slots are filled. */
	public sealed interface Part permits Loose, Bundled, Chested {
	}

	/** One slot, the stack as given. */
	public record Loose(ItemStack stack) implements Part {
	}

	/**
	 * Stackable items shared out over bundles. {@code namer} names each bundle from its items
	 * (null: the label, numbered when there are several).
	 */
	public record Bundled(String label, List<ItemStack> items, @Nullable Function<List<ItemStack>, Component> namer) implements Part {
		public Bundled(String label, List<ItemStack> items) {
			this(label, items, null);
		}
	}

	/** Items in chests of 27 stacks each (numbered when there are several). */
	public record Chested(String label, List<ItemStack> items) implements Part {
	}

	/**
	 * A packed box. {@code slots}: what goes in the box (27 at most); {@code leaves}: every item the
	 * box holds, counting what's inside its bundles and chests; {@code overflow}: what didn't fit,
	 * described; {@code duplicates}: items listed twice (kept once).
	 */
	public record Result(String box, List<ItemStack> slots, List<ItemStack> leaves, List<String> overflow, List<String> duplicates, int bundles, int chests) {
		public String summary() {
			return String.format("'%s': %d of %d slots, %d different items (%d bundles holding %d of them, %d chests), overflow: %s", box, slots.size(),
				SLOTS, leaves.size(), bundles, bundledLeaves(), chests, overflow.isEmpty() ? "none" : overflow.size() + " item(s): " + String.join(", ", overflow));
		}

		private int bundledLeaves() {
			int n = 0;
			for (ItemStack slot : slots) {
				BundleContents contents = slot.get(DataComponents.BUNDLE_CONTENTS);
				if (contents != null) {
					n += contents.size();
				}
			}
			return n;
		}
	}

	/** How one part is laid out: how many slots it takes, and whether a bundle group went loose. */
	private static final class Plan {
		final Part part;
		final List<ItemStack> items;
		int slots;
		boolean loose;

		Plan(Part part, List<ItemStack> items, int slots) {
			this.part = part;
			this.items = items;
			this.slots = slots;
		}

		/** The most different items one of this group's bundles holds. */
		int widest() {
			return (items.size() + slots - 1) / slots;
		}
	}

	/** Packs {@code parts} into one box; {@code bundle} and {@code chest} are the container items to use. */
	public static Result pack(String box, List<Part> parts, Item bundle, Item chest) {
		List<String> overflow = new ArrayList<>();
		List<String> duplicates = new ArrayList<>();
		List<ItemStack> seen = new ArrayList<>();
		List<Plan> plans = new ArrayList<>();
		int used = 0;
		for (Part part : parts) {
			List<ItemStack> items = new ArrayList<>();
			List<ItemStack> given = switch (part) {
				case Loose loose -> List.of(loose.stack());
				case Bundled bundled -> bundled.items();
				case Chested chested -> chested.items();
			};
			for (ItemStack stack : given) {
				if (stack.isEmpty()) {
					continue;
				}
				if (contains(seen, stack)) {
					duplicates.add(describe(stack));
					continue;
				}
				if (part instanceof Bundled && !fitsBundle(stack)) {
					overflow.add(describe(stack) + " (can't go in a bundle)");
					continue;
				}
				seen.add(stack);
				items.add(stack);
			}
			if (items.isEmpty()) {
				continue;
			}
			int need = switch (part) {
				case Loose loose -> 1;
				case Chested chested -> (items.size() + SLOTS - 1) / SLOTS;
				case Bundled bundled -> minBundles(items, bundle);
			};
			if (used + need > SLOTS) {
				for (ItemStack stack : items) {
					overflow.add(describe(stack));
				}
				continue;
			}
			plans.add(new Plan(part, items, need));
			used += need;
		}
		spread(plans, SLOTS - used);

		List<ItemStack> slots = new ArrayList<>();
		List<ItemStack> leaves = new ArrayList<>();
		int bundles = 0;
		int chests = 0;
		for (Plan plan : plans) {
			switch (plan.part) {
				case Loose loose -> {
					slots.add(plan.items.getFirst().copy());
					leaves.add(plan.items.getFirst());
				}
				case Chested chested -> {
					for (int i = 0; i < plan.slots; i++) {
						List<ItemStack> inside = plan.items.subList(i * SLOTS, Math.min(plan.items.size(), (i + 1) * SLOTS));
						slots.add(chest(chest, numbered(chested.label(), i, plan.slots), inside));
						leaves.addAll(inside);
						chests++;
					}
				}
				case Bundled bundled -> {
					if (plan.loose) {
						for (ItemStack stack : plan.items) {
							slots.add(stack.copy());
							leaves.add(stack);
						}
						break;
					}
					List<List<ItemStack>> layout = layout(plan.items, plan.slots, bundle);
					if (layout == null) { // can't happen: minBundles found a layout with no more bundles
						throw new IllegalStateException("no bundle layout for " + bundled.label());
					}
					for (int i = 0; i < layout.size(); i++) {
						List<ItemStack> inside = layout.get(i);
						Component name = bundled.namer() != null ? bundled.namer().apply(inside) : numbered(bundled.label(), i, layout.size());
						slots.add(bundle(bundle, name, inside));
						leaves.addAll(inside);
						bundles++;
					}
				}
			}
		}
		return new Result(box, slots, leaves, overflow, duplicates, bundles, chests);
	}

	/** Hands the free slots to the bundle groups (see the class comment). */
	private static void spread(List<Plan> plans, int free) {
		while (true) {
			Plan worst = null;
			for (Plan plan : plans) {
				if (plan.part instanceof Bundled && !plan.loose && (worst == null || plan.widest() > worst.widest()
					|| plan.widest() == worst.widest() && plan.items.size() > worst.items.size())) {
					worst = plan;
				}
			}
			if (worst == null) {
				return;
			}
			int unbundle = worst.items.size() - worst.slots;
			if (unbundle <= free) {
				worst.loose = true;
				worst.slots = worst.items.size();
				free -= unbundle;
			} else if (free > 0) {
				worst.slots++;
				free--;
			} else {
				return;
			}
		}
	}

	/** The fewest bundles {@code items} fit in (one of each at least). */
	private static int minBundles(List<ItemStack> items, Item bundle) {
		for (int n = 1; n <= items.size(); n++) {
			if (layout(items, n, bundle) != null) {
				return n;
			}
		}
		return items.size(); // one item per bundle always fits (fitsBundle)
	}

	/**
	 * {@code items} split into {@code n} runs as even as possible, each run's items sharing a bundle's
	 * 64 units equally (at least one each, at most the count asked for). Null when a run doesn't fit.
	 */
	private static @Nullable List<List<ItemStack>> layout(List<ItemStack> items, int n, Item bundle) {
		List<List<ItemStack>> runs = new ArrayList<>();
		int from = 0;
		for (int i = 0; i < n; i++) {
			int size = items.size() / n + (i < items.size() % n ? 1 : 0);
			List<ItemStack> run = new ArrayList<>();
			int share = BUNDLE_UNITS / Math.max(1, size);
			int total = 0;
			for (ItemStack stack : items.subList(from, from + size)) {
				int units = units(stack);
				int count = Math.max(1, Math.min(stack.getCount(), share / units));
				run.add(stack.copyWithCount(count));
				total += count * units;
			}
			// Items whose single unit is more than the share (a 16-stack among many 64-stacks) can push
			// the run over: take back from the biggest counts.
			while (total > BUNDLE_UNITS) {
				ItemStack biggest = null;
				for (ItemStack stack : run) {
					if (stack.getCount() > 1 && (biggest == null || stack.getCount() * units(stack) > biggest.getCount() * units(biggest))) {
						biggest = stack;
					}
				}
				if (biggest == null) {
					return null;
				}
				biggest.shrink(1);
				total -= units(biggest);
			}
			if (!fill(new ItemStack(bundle), run)) {
				return null;
			}
			runs.add(run);
			from += size;
		}
		return runs;
	}

	/** Bundle units one item of {@code stack} takes. */
	private static int units(ItemStack stack) {
		return (BUNDLE_UNITS + stack.getMaxStackSize() - 1) / stack.getMaxStackSize();
	}

	private static boolean fitsBundle(ItemStack stack) {
		return stack.getMaxStackSize() > 1 && BundleContents.canItemBeInBundle(stack);
	}

	/** Puts {@code items} into {@code bundle}; false when the bundle doesn't take them all. */
	private static boolean fill(ItemStack bundle, List<ItemStack> items) {
		BundleContents.Mutable contents = new BundleContents.Mutable();
		// A bundle shows its last insertion first: insert backwards so it reads in list order.
		for (int i = items.size() - 1; i >= 0; i--) {
			ItemStack copy = items.get(i).copy();
			int want = copy.getCount();
			if (contents.tryInsert(copy) != want) {
				return false;
			}
		}
		bundle.set(DataComponents.BUNDLE_CONTENTS, contents.toImmutable());
		return true;
	}

	private static ItemStack bundle(Item item, Component name, List<ItemStack> items) {
		ItemStack bundle = new ItemStack(item);
		if (!fill(bundle, items)) {
			throw new IllegalStateException("bundle overfilled: " + name.getString());
		}
		bundle.set(DataComponents.CUSTOM_NAME, plain(name));
		return bundle;
	}

	private static ItemStack chest(Item item, Component name, List<ItemStack> items) {
		ItemStack chest = new ItemStack(item);
		List<ItemStack> copies = new ArrayList<>();
		for (ItemStack stack : items) {
			copies.add(stack.copy());
		}
		chest.set(DataComponents.CONTAINER, ItemContainerContents.fromItems(copies));
		chest.set(DataComponents.CUSTOM_NAME, plain(name));
		chest.set(DataComponents.LORE, new ItemLore(List.of(Component.literal("Place it to unpack").withStyle(s -> s.withItalic(false).withColor(0xAAAAAA)))));
		return chest;
	}

	/** "Label", or "Label (2/3)" when the part takes several slots. */
	private static Component numbered(String label, int i, int of) {
		return Component.literal(of > 1 ? label + " (" + (i + 1) + "/" + of + ")" : label);
	}

	/** Custom names are italic unless told otherwise. */
	static Component plain(Component name) {
		return name.copy().withStyle(s -> s.withItalic(false));
	}

	private static boolean contains(List<ItemStack> stacks, ItemStack stack) {
		for (ItemStack other : stacks) {
			if (ItemStack.isSameItemSameComponents(other, stack)) {
				return true;
			}
		}
		return false;
	}

	/** {@code minecraft:potion[strong_healing] x1} style, for logs. */
	public static String describe(ItemStack stack) {
		String id = BuiltInRegistries.ITEM.getKey(stack.getItem()).getPath();
		PotionContents potion = stack.get(DataComponents.POTION_CONTENTS);
		if (potion != null && potion.potion().isPresent()) {
			id += "[" + potion.potion().get().unwrapKey().map(k -> k.identifier().getPath()).orElse("?") + "]";
		}
		return id + " x" + stack.getCount();
	}
}
