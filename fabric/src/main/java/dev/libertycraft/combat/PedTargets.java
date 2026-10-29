package dev.libertycraft.combat;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.mixin.MobTargetSelectorAccessor;
import dev.libertycraft.mixin.NearestAttackableTargetGoalAccessor;
import dev.libertycraft.mixin.NearestAttackableWitchTargetGoalAccessor;
import dev.libertycraft.mixin.TargetGoalAccessor;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.EnumSet;
import java.util.List;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.LivingEntity;
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
 * Minecraft's hostile mobs hunt GTA IV's peds, and the cars people sit in, as they hunt the player:
 * whoever is nearest.
 *
 * <p>A hostile mob ({@link Enemy}: zombies, husks, drowned, skeletons, strays, spiders, creepers,
 * pillagers, vindicators, witches, ghasts, ...) picks players with a {@link NearestAttackableTargetGoal}
 * for players; every such goal makes way for a {@link NearestTargetGoal} at the same priority that picks
 * the nearest of the player and the stand-ins of living GTA IV peds on foot and of vehicles someone sits in
 * ({@link HostActorEntity}: Proto.ACTOR_OCCUPIED; not an empty car, a wreck or a body), all the same way:
 * within the mob's follow range, in sight if the mob's goal wants that, reachable if it wants that, with the
 * goal's own conditions (a drowned's, a ghast's height limit) and its own pace; spiders only in the
 * dark, a raiding witch only while its own goal lets it. A tie goes to the player. While it chases, it
 * looks again every second and turns to whoever is more than {@link #SWITCH_MARGIN} blocks nearer (a
 * vanilla goal never lets go of a player in range, and a second goal at its priority could never take
 * over). A mob that is hit turns on whoever hit it, as in vanilla (HurtByTargetGoal). From then on the mob
 * is vanilla: it paths to the ped and attacks
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
	/** How often a chasing mob looks for someone nearer (ticks). */
	static final int RETARGET_TICKS = 20;
	/** How much nearer someone must be for a chasing mob to turn to them (blocks), so it doesn't flip between two. */
	static final double SWITCH_MARGIN = 4.0;

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

	/** A ped this far away (squared) is chosen over a player this far away: only when nearer (a tie goes to the player). */
	static boolean picksPed(double pedDistanceSqr, double playerDistanceSqr) {
		return pedDistanceSqr < playerDistanceSqr;
	}

	/** A mob chasing something this far away turns to someone else this far away (blocks). */
	static boolean switchesTo(double currentDistance, double otherDistance) {
		return otherDistance + SWITCH_MARGIN < currentDistance;
	}

	static void addGoals(Mob mob) {
		GoalSelector selector = ((MobTargetSelectorAccessor) mob).libertycraft$targetSelector();
		List<WrappedGoal> players = new ArrayList<>();
		for (WrappedGoal w : selector.getAvailableGoals()) {
			if (w.getGoal() instanceof NearestTargetGoal) {
				return; // (loaded before: it has them)
			}
			if (w.getGoal() instanceof NearestAttackableTargetGoal<?> goal && huntsPlayers(goal)) {
				players.add(w);
			}
		}
		for (WrappedGoal w : players) {
			// The vanilla goal leaves the selector but lives on inside ours: its conditions and pace come
			// from it, and a witch still flips its raid switch on it (Witch.attackPlayersGoal).
			selector.removeGoal(w.getGoal());
			selector.addGoal(w.getPriority(), new NearestTargetGoal(mob, (NearestAttackableTargetGoal<?>) w.getGoal()));
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

	/** A mob's goal for players, made to pick the nearest of the player and GTA IV's peds. */
	static final class NearestTargetGoal extends TargetGoal {
		private final NearestAttackableTargetGoal<?> players;
		private final TargetingConditions conditions;
		private final int interval;
		private final boolean darkOnly;
		private @Nullable LivingEntity candidate;
		private int retargetIn;

		NearestTargetGoal(Mob mob, NearestAttackableTargetGoal<?> players) {
			super(mob, ((TargetGoalAccessor) players).libertycraft$mustSee(), ((TargetGoalAccessor) players).libertycraft$mustReach());
			NearestAttackableTargetGoalAccessor goal = (NearestAttackableTargetGoalAccessor) players;
			this.players = players;
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
			if (this.players instanceof NearestAttackableWitchTargetGoal<?> witch && !((NearestAttackableWitchTargetGoalAccessor) witch).libertycraft$canAttack()) {
				return false; // a raiding witch heals its raiders now
			}
			this.candidate = this.nearest(getServerLevel(this.mob), Double.MAX_VALUE);
			return this.candidate != null;
		}

		/**
		 * The nearest the mob can go for within {@code within} blocks: the player as vanilla's goal finds
		 * them, or a ped nearer than that.
		 */
		private @Nullable LivingEntity nearest(ServerLevel level, double within) {
			if (!(within > 0.0)) {
				return null;
			}
			double range = this.getFollowDistance();
			TargetingConditions conditions = this.conditions.range(range);
			double limitSqr = within == Double.MAX_VALUE ? Double.MAX_VALUE : within * within;
			Player player = level.getNearestPlayer(conditions, this.mob, this.mob.getX(), this.mob.getEyeY(), this.mob.getZ());
			if (player != null && this.mob.distanceToSqr(player) >= limitSqr) {
				player = null;
			}
			if (player != null) {
				limitSqr = this.mob.distanceToSqr(player); // only a nearer ped can be chosen over them
			}
			List<HostActorEntity> peds = level.getEntitiesOfClass(HostActorEntity.class, this.mob.getBoundingBox().inflate(range), PedTargets::isPed);
			peds.sort(Comparator.comparingDouble(this.mob::distanceToSqr));
			for (HostActorEntity p : peds) {
				double d = this.mob.distanceToSqr(p);
				if (d >= limitSqr) {
					break;
				}
				if (this.canAttack(p, conditions)) {
					return player == null || picksPed(d, this.mob.distanceToSqr(player)) ? p : player;
				}
			}
			return player;
		}

		@Override
		public void start() {
			LivingEntity target = this.candidate;
			this.mob.setTarget(target);
			super.start();
			this.retargetIn = RETARGET_TICKS;
			if (target instanceof HostActorEntity ped) {
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
			if (--this.retargetIn <= 0) {
				this.retargetIn = RETARGET_TICKS;
				this.retarget();
			}
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

		/** Someone clearly nearer than what the mob chases: it turns to them. */
		private void retarget() {
			LivingEntity current = this.mob.getTarget();
			if (current == null) {
				return;
			}
			double now = this.mob.distanceTo(current);
			LivingEntity nearer = this.nearest(getServerLevel(this.mob), now - SWITCH_MARGIN);
			if (nearer == null || nearer == current || !switchesTo(now, this.mob.distanceTo(nearer))) {
				return;
			}
			this.mob.setTarget(nearer);
			this.targetMob = nearer;
			if (turnLogs++ < 200) {
				LibertyCraft.LOG.info("[LibertyCraft] {} turns from {} to {}, {} and {} blocks away", this.mob.getType().toShortString(), who(current), who(nearer),
					String.format("%.1f", now), String.format("%.1f", this.mob.distanceTo(nearer)));
			}
		}

		private static String who(LivingEntity e) {
			if (e instanceof HostActorEntity ped) {
				return "GTA IV " + (ped.isHostVehicle() ? "vehicle " : "ped ") + Integer.toHexString(ped.formId()) + " (" + ped.getName().getString() + ")";
			}
			return e instanceof Player ? "the player" : e.getType().toShortString();
		}
	}

	private static int huntLogs;
	private static int turnLogs;
}
