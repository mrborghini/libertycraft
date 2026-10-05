package dev.libertycraft.combat;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Proto;
import dev.libertycraft.mixin.MobTargetSelectorAccessor;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.EnumSet;
import java.util.List;
import java.util.Locale;
import java.util.Properties;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.Goal;
import net.minecraft.world.entity.ai.goal.target.TargetGoal;
import net.minecraft.world.entity.ai.targeting.TargetingConditions;
import net.minecraft.world.entity.animal.golem.IronGolem;
import net.minecraft.world.entity.animal.golem.SnowGolem;
import org.jspecify.annotations.Nullable;

/**
 * Which of GTA IV's peds Minecraft's iron and snow golems go for: {@code golemTargets} in
 * config/libertycraft.properties.
 *
 * <ul>
 * <li>{@code vanilla}: golems only attack Minecraft mobs (vanilla's hostile-mob targeting, plus players
 * who anger them); GTA IV's peds are never targets.</li>
 * <li>{@code guard} (the default; like a wolf): golems also attack the peds and police who attack the
 * player (GTA IV's "hostile to the player" in the actor table, or a ped that hurt him in Minecraft lately),
 * and the peds the player attacks.</li>
 * <li>{@code cops}: police count as monsters (golems attack them on sight), plus guard.</li>
 * <li>{@code hostile}: every GTA IV ped counts as a monster: golems attack any ped on sight.</li>
 * </ul>
 *
 * Peds on foot only, never a mission character. Vanilla stays as it is otherwise: a golem the player built
 * (PlayerCreated) never attacks players, even when hit; one from a spawn egg, a command or a village gets
 * angry at a player who hits it. A golem that hits a ped is reported like any mob (HostMobs: it is after
 * that ped), so the ped and police nearby shoot back or run, as with monsters.
 */
public final class GolemTargets {
	public enum Mode {
		VANILLA, GUARD, COPS, HOSTILE;

		static Mode parse(@Nullable String text) {
			if (text == null) {
				return GUARD;
			}
			return switch (text.trim().toLowerCase(Locale.ROOT)) {
				case "vanilla", "off", "none" -> VANILLA;
				case "cops", "police" -> COPS;
				case "hostile", "all" -> HOSTILE;
				default -> GUARD;
			};
		}
	}

	static final String CONFIG_KEY = "golemTargets";
	/** How long a ped stays an enemy of the player's golems after it hurt the player or he hurt it (ticks). */
	static final int GRUDGE_TICKS = 200;
	/** Players this near a golem count for guard. */
	static final double GUARD_RANGE = 32.0;
	private static volatile Mode mode = Mode.GUARD;
	private static int logs;

	private GolemTargets() {
	}

	public static void init() {
		ServerLifecycleEvents.SERVER_STARTED.register(server -> loadConfig());
		ServerEntityEvents.ENTITY_LOAD.register((entity, level) -> {
			if (entity instanceof IronGolem || entity instanceof SnowGolem) {
				addGoal((Mob) entity);
			}
		});
	}

	public static Mode mode() {
		return mode;
	}

	static void addGoal(Mob golem) {
		var selector = ((MobTargetSelectorAccessor) golem).libertycraft$targetSelector();
		for (var w : selector.getAvailableGoals()) {
			if (w.getGoal() instanceof GolemPedGoal) {
				return;
			}
		}
		// Beside vanilla's goal for monsters (iron golem 3, snow golem 1): a ped is one more monster.
		selector.addGoal(golem instanceof IronGolem ? 3 : 1, new GolemPedGoal(golem));
		if (logs++ < 100) {
			LibertyCraft.LOG.info("[LibertyCraft] {} at {} {} {} goes for GTA IV's peds in {} mode", golem.getType().toShortString(), String.format("%.1f", golem.getX()),
				String.format("%.1f", golem.getY()), String.format("%.1f", golem.getZ()), mode.name().toLowerCase(Locale.ROOT));
		}
	}

	/** True if {@code ped} is a golem's enemy in the current mode, with {@code players} near the golem. */
	static boolean enemy(HostActorEntity ped, List<ServerPlayer> players) {
		Mode m = mode;
		if (m == Mode.VANILLA || ped.isHostVehicle() || !PedTargets.isPed(ped)) {
			return false;
		}
		if (m == Mode.HOSTILE || m == Mode.COPS && isCop(ped)) {
			return true;
		}
		if ((ped.hostFlags() & Proto.ACTOR_HOSTILE) != 0) {
			return true; // fighting the player (GTA IV: in combat with him and hurt him lately, or police while he's wanted)
		}
		for (ServerPlayer p : players) {
			if (p.getLastHurtByMob() == ped && p.tickCount - p.getLastHurtByMobTimestamp() < GRUDGE_TICKS
				|| p.getLastHurtMob() == ped && p.tickCount - p.getLastHurtMobTimestamp() < GRUDGE_TICKS) {
				return true;
			}
		}
		return false;
	}

	/** The host names a ped's stand-in after its type (PedTypeName): police are "Cop". */
	static boolean isCop(HostActorEntity ped) {
		return "Cop".equals(ped.getName().getString());
	}

	/** A golem's target goal for GTA IV's peds, as vanilla's for monsters: in sight, the nearest, every half second or so. */
	static final class GolemPedGoal extends TargetGoal {
		private static final int INTERVAL = 10;
		private final TargetingConditions conditions = TargetingConditions.forCombat();
		private @Nullable HostActorEntity candidate;

		GolemPedGoal(Mob golem) {
			super(golem, true, false);
			this.setFlags(EnumSet.of(Goal.Flag.TARGET));
		}

		@Override
		public boolean canUse() {
			if (mode == Mode.VANILLA || this.mob.getRandom().nextInt(INTERVAL) != 0) {
				return false;
			}
			ServerLevel level = getServerLevel(this.mob);
			double range = this.getFollowDistance();
			List<HostActorEntity> peds = level.getEntitiesOfClass(HostActorEntity.class, this.mob.getBoundingBox().inflate(range), PedTargets::isPed);
			if (peds.isEmpty()) {
				return false;
			}
			List<ServerPlayer> players = new ArrayList<>();
			for (ServerPlayer p : level.players()) {
				if (p.isAlive() && p.distanceToSqr(this.mob) < GUARD_RANGE * GUARD_RANGE) {
					players.add(p);
				}
			}
			peds.sort(Comparator.comparingDouble(this.mob::distanceToSqr));
			TargetingConditions near = this.conditions.range(range);
			for (HostActorEntity ped : peds) {
				if (enemy(ped, players) && this.canAttack(ped, near)) {
					this.candidate = ped;
					return true;
				}
			}
			return false;
		}

		@Override
		public void start() {
			HostActorEntity ped = this.candidate;
			this.mob.setTarget(ped);
			super.start();
			if (ped != null && logs++ < 100) {
				LibertyCraft.LOG.info("[LibertyCraft] {} goes for GTA IV {} {} ({} mode), {} blocks away", this.mob.getType().toShortString(), ped.getName().getString(),
					Integer.toHexString(ped.formId()), mode.name().toLowerCase(Locale.ROOT), String.format("%.1f", this.mob.distanceTo(ped)));
			}
		}

		@Override
		public boolean canContinueToUse() {
			LivingEntity target = this.mob.getTarget();
			if (target instanceof HostActorEntity ped && (mode == Mode.VANILLA || !PedTargets.isPed(ped))) {
				return false;
			}
			return super.canContinueToUse();
		}

		@Override
		public void stop() {
			super.stop();
			this.candidate = null;
		}
	}

	// ---- config/libertycraft.properties: golemTargets=vanilla|guard|cops|hostile --------------------------
	private static void loadConfig() {
		Path file = FabricLoader.getInstance().getConfigDir().resolve("libertycraft.properties");
		Properties props = new Properties();
		try {
			if (Files.exists(file)) {
				try (var in = Files.newBufferedReader(file)) {
					props.load(in);
				}
			}
			String value = props.getProperty(CONFIG_KEY);
			if (value == null) {
				List<String> lines = Files.exists(file) ? new ArrayList<>(Files.readAllLines(file)) : new ArrayList<>(List.of("# LibertyCraft"));
				lines.add("# Which GTA IV peds iron and snow golems attack: guard (the default, like a wolf: the peds and police who attack you,");
				lines.add("# and the peds you attack), cops (police on sight, plus guard), hostile (any ped on sight) or vanilla (golems only attack");
				lines.add("# Minecraft mobs, plus players who anger them; GTA IV's peds are never targets).");
				lines.add(CONFIG_KEY + "=guard");
				Files.createDirectories(file.getParent());
				Files.write(file, lines);
			}
			mode = Mode.parse(value);
		} catch (Exception e) {
			mode = Mode.GUARD;
			LibertyCraft.LOG.warn("[LibertyCraft] couldn't read {} ({}); golems guard the player", file, e.toString());
		}
		LibertyCraft.LOG.info("[LibertyCraft] golems and GTA IV's peds: {} ({}={})", switch (mode) {
			case VANILLA -> "golems only attack Minecraft mobs (and players who anger them), never GTA IV's peds";
			case GUARD -> "golems attack the peds and police who attack the player, and the peds he attacks";
			case COPS -> "golems attack police on sight, and guard the player";
			case HOSTILE -> "golems attack any ped on sight";
		}, CONFIG_KEY, mode.name().toLowerCase(Locale.ROOT));
	}
}
