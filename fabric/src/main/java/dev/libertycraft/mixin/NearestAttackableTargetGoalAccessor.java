package dev.libertycraft.mixin;

import net.minecraft.world.entity.ai.goal.target.NearestAttackableTargetGoal;
import net.minecraft.world.entity.ai.targeting.TargetingConditions;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** How a mob picks players as targets, for PedTargets to pick GTA IV's peds the same way. */
@Mixin(NearestAttackableTargetGoal.class)
public interface NearestAttackableTargetGoalAccessor {
	@Accessor("targetType")
	Class<?> libertycraft$targetType();

	@Accessor("randomInterval")
	int libertycraft$randomInterval();

	@Accessor("targetConditions")
	TargetingConditions libertycraft$targetConditions();
}
