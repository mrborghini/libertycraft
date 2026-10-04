package dev.libertycraft.mixin;

import dev.libertycraft.world.HostPath;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.PathfinderMob;
import net.minecraft.world.entity.ai.goal.MeleeAttackGoal;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Melee mobs (zombies, husks, vindicators, spiders, creepers) on GTA IV's ground: when Minecraft's
 * pathfinder finds no way to their target (GTA IV's geometry is no blocks; HostPath makes it see most
 * of it, not all), or its path ends short of it, they walk straight at it instead of standing still,
 * as long as they see it (through GTA IV's walls they don't: MobSightMixin) and it is within 32
 * blocks. Liberty City's streets are mostly flat, and the mob still bumps into what's in the way.
 * Only in the mirror world (HostPath.applies).
 */
@Mixin(MeleeAttackGoal.class)
public abstract class MeleeAttackGoalMixin {
	@Unique
	private static final double LIBERTYCRAFT_STRAIGHT_RANGE = 32.0;

	@Shadow
	@Final
	protected PathfinderMob mob;

	@Shadow
	@Final
	private double speedModifier;

	@Unique
	private boolean libertycraft$straight(LivingEntity target) {
		return target != null && target.isAlive() && HostPath.applies(this.mob.level()) && this.mob.distanceToSqr(target) < LIBERTYCRAFT_STRAIGHT_RANGE * LIBERTYCRAFT_STRAIGHT_RANGE
			&& this.mob.getSensing().hasLineOfSight(target);
	}

	@Inject(method = "canUse", at = @At("RETURN"), cancellable = true)
	private void libertycraft$noPathStillGoes(CallbackInfoReturnable<Boolean> cir) {
		if (!cir.getReturnValue() && this.mob.getTarget() != null && this.libertycraft$straight(this.mob.getTarget())) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "canContinueToUse", at = @At("RETURN"), cancellable = true)
	private void libertycraft$noPathKeepsGoing(CallbackInfoReturnable<Boolean> cir) {
		if (!cir.getReturnValue() && this.mob.getNavigation().isDone() && this.libertycraft$straight(this.mob.getTarget())) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "tick", at = @At("TAIL"))
	private void libertycraft$walkStraight(CallbackInfo ci) {
		LivingEntity target = this.mob.getTarget();
		if (target != null && this.mob.getNavigation().isDone() && !this.mob.isWithinMeleeAttackRange(target) && this.libertycraft$straight(target)) {
			this.mob.getMoveControl().setWantedPosition(target.getX(), target.getY(), target.getZ(), this.speedModifier);
		}
	}
}
