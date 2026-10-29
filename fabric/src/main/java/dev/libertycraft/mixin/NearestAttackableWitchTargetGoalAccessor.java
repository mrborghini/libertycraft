package dev.libertycraft.mixin;

import net.minecraft.world.entity.ai.goal.target.NearestAttackableWitchTargetGoal;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Whether a raiding witch hunts players just now (Witch.aiStep sets it), for PedTargets' goal that takes its place. */
@Mixin(NearestAttackableWitchTargetGoal.class)
public interface NearestAttackableWitchTargetGoalAccessor {
	@Accessor("canAttack")
	boolean libertycraft$canAttack();
}
