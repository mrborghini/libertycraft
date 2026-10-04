package dev.libertycraft.combat;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.mixin.MobTargetSelectorAccessor;
import dev.libertycraft.mixin.NearestAttackableTargetGoalAccessor;
import dev.libertycraft.mixin.TargetGoalAccessor;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.EnumSet;
import java.util.List;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.Goal;
import net.minecraft.world.entity.ai.goal.GoalSelector;
import net.minecraft.world.entity.ai.goal.WrappedGoal;
import net.minecraft.world.entity.ai.goal.target.NearestAttackableTargetGoal;
import net.minecraft.world.entity.ai.goal.target.NearestAttackableWitchTargetGoal;
import net.minecraft.world.entity.ai.goal.target.TargetGoal;
import net.minecraft.world.entity.ai.targeting.TargetingConditions;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.entity.monster.spider.Spider;
import net.minecraft.world.entity.player.Player;
import org.jspecify.annotations.Nullable;

/**
 * Minecraft's hostile mobs hunt GTA IV's peds, and the cars people sit in, as they hunt the player.
 *
 * <p>A hostile mob ({@link Enemy}: zombies, husks, drowned, skeletons, strays, spiders, creepers,
 * pillagers, vindicators, witches, ghasts, ...) picks players with a {@link NearestAttackableTargetGoal}
 * for players; every such goal gets a twin at the same priority that picks the stand-ins of living GTA IV
 * peds on foot and of vehicles someone sits in ({@link HostActorEntity}: Proto.ACTOR_OCCUPIED; not an
 * empty car, a wreck or a body) the same way: within the mob's follow range, in sight if the mob's goal wants that, reachable if it wants that, with the
 * goal's own conditions (a drowned's, a ghast's height limit) and its own pace; spiders only in the
 * dark, as for players. When a player is at least as near, the twin leaves the choice to the mob's own
 * goal, so the player stays a target. From then on the mob is vanilla: it paths to the ped and attacks
 * it with its own goal (blows, arrows, potions, a creeper's blast, a ghast's fireball), and its hits reach
 * GTA IV through the stand-in, flagged as a mob's (Proto.HIT_BY_MOB: no crime of the player's); a car takes
 * them as any hit on a vehicle (a wither, which targets any living thing, goes for occupied cars by itself).
 * The player's own car has no stand-in: PlayerVehicleHits turns hits on the seated player into hits on
 * it. Mobs whose player goal is anything else (endermen, who wait to be looked at; shulkers;
 * brain-driven piglins, hoglins, wardens) and neutral or friendly mobs (wolves, golems) are left as they
 * are. Peds in vehicles have no stand-in of their own: mobs go for their vehicle. Paths over GTA IV's
 * ground: HostPath; walking straight where there is none: MeleeAttackGoalMixin.
 */
public final class PedTargets {
	private PedTargets() {
	}

	public static void init() {
		ServerEntityEvents.ENTITY_LOAD.register((entity, level) -> {
			if (entity instanceof Mob mob && mob instanceof Enemy) {
				addGoals(mob);
			}
		});
	}

	/**
	 * A stand-in a mob can hunt: a living ped on foot, or a piece of a vehicle someone sits in; never a mission
	 * character or the vehicle one sits in (Proto.ACTOR_MISSION: a creeper failed a mission, zombies chased a date off).
	 */
	public static boolean isPed(HostActorEntity e) {
		return e.isAlive() && !e.isRemoved() && !e.isMission() && (e.isHostVehicle() ? e.isOccupiedVehicle() : !e.isHostCorpse());
	}

	static void addGoals(Mob mob) {
		GoalSelector selector = ((MobTargetSelectorAccessor) mob).libertycraft$targetSelector();
		List<WrappedGoal> twins = new ArrayList<>();
		for (WrappedGoal w : selector.getAvailableGoals()) {
			if (w.getGoal() instanceof PedGoal) {
				return; // (loaded before: it has them)
			}
			if (w.getGoal() instanceof NearestAttackableTargetGoal<?> goal && huntsPlayers(goal)) {
				twins.add(w);
			}
		}
		for (WrappedGoal w : twins) {
			selector.addGoal(w.getPriority(), new PedGoal(mob, (NearestAttackableTargetGoal<?>) w.getGoal()));
		}
	}

	/** The plain goal for players (or a witch's, or a spider's dark-only one); not an enderman's or a shulker's. */
	static boolean huntsPlayers(NearestAttackableTargetGoal<?> goal) {
		Class<?> type = ((NearestAttackableTargetGoalAccessor) goal).libertycraft$targetType();
		if (type != Player.class && type != ServerPlayer.class) {
			return false;
		}
		Class<?> c = goal.getClass();
		return c == NearestAttackableTargetGoal.class || c == NearestAttackableWitchTargetGoal.class || c.getEnclosingClass() == Spider.class;
	}

	/** The twin of a mob's goal for players, for GTA IV's peds. */
	static final class PedGoal extends TargetGoal {
		private final TargetingConditions conditions;
		private final int interval;
		private final boolean darkOnly;
		private @Nullable HostActorEntity candidate;

		PedGoal(Mob mob, NearestAttackableTargetGoal<?> players) {
			super(mob, ((TargetGoalAccessor) players).libertycraft$mustSee(), ((TargetGoalAccessor) players).libertycraft$mustReach());
			NearestAttackableTargetGoalAccessor goal = (NearestAttackableTargetGoalAccessor) players;
			this.conditions = goal.libertycraft$targetConditions().copy();
			this.interval = goal.libertycraft$randomInterval();
			this.darkOnly = players.getClass().getEnclosingClass() == Spider.class;
			this.setFlags(EnumSet.of(Goal.Flag.TARGET));
		}

		@Override
		public boolean canUse() {
			if (this.interval > 0 && this.mob.getRandom().nextInt(this.interval) != 0) {
				return false;
			}
			if (this.darkOnly && this.mob.getLightLevelDependentMagicValue() >= 0.5F) {
				return false;
			}
			ServerLevel level = getServerLevel(this.mob);
			double range = this.getFollowDistance();
			TargetingConditions conditions = this.conditions.range(range);
			List<HostActorEntity> peds = level.getEntitiesOfClass(HostActorEntity.class, this.mob.getBoundingBox().inflate(range), PedTargets::isPed);
			if (peds.isEmpty()) {
				return false;
			}
			peds.sort(Comparator.comparingDouble(this.mob::distanceToSqr));
			HostActorEntity ped = null;
			for (HostActorEntity p : peds) {
				if (this.canAttack(p, conditions)) {
					ped = p;
					break;
				}
			}
			if (ped == null) {
				return false;
			}
			Player player = level.getNearestPlayer(conditions, this.mob, this.mob.getX(), this.mob.getEyeY(), this.mob.getZ());
			if (player != null && this.mob.distanceToSqr(player) <= this.mob.distanceToSqr(ped)) {
				return false; // the player is as near: the mob's own goal picks them
			}
			this.candidate = ped;
			return true;
		}

		@Override
		public void start() {
			HostActorEntity ped = this.candidate;
			this.mob.setTarget(ped);
			super.start();
			if (ped != null) {
				LibertyCraft.LOG.info("[LibertyCraft] {} hunts GTA IV {} {} ({}), {} blocks away", this.mob.getType().toShortString(), ped.isHostVehicle() ? "vehicle" : "ped",
					Integer.toHexString(ped.formId()), ped.getName().getString(), String.format("%.1f", this.mob.distanceTo(ped)));
			}
		}

		@Override
		public void stop() {
			super.stop();
			this.candidate = null;
		}

		@Override
		public void tick() {
			// Now and then, how the hunt goes (where the mob is, where its path leads).
			if (this.mob.tickCount % 80 != 0 || huntLogs >= 120 || !(this.mob.getTarget() instanceof HostActorEntity ped)) {
				return;
			}
			huntLogs++;
			var path = this.mob.getNavigation().getPath();
			var end = path != null ? path.getEndNode() : null;
			LibertyCraft.LOG.info("[LibertyCraft] {} hunting {}: at {} {} {}{}, it is at {} {} {} ({} blocks), path {}", this.mob.getType().toShortString(),
				Integer.toHexString(ped.formId()), String.format("%.1f", this.mob.getX()), String.format("%.1f", this.mob.getY()), String.format("%.1f", this.mob.getZ()),
				this.mob.onGround() ? "" : " (in the air)", String.format("%.1f", ped.getX()), String.format("%.1f", ped.getY()), String.format("%.1f", ped.getZ()),
				String.format("%.1f", this.mob.distanceTo(ped)),
				path == null ? "none" : path.getNodeCount() + " nodes to " + (end == null ? "?" : end.x + " " + end.y + " " + end.z) + (path.canReach() ? "" : " (can't reach)")
					+ ", at node " + path.getNextNodeIndex());
		}
	}

	private static int huntLogs;
}
