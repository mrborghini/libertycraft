package dev.libertycraft.mixin;

import net.minecraft.world.entity.ai.goal.target.TargetGoal;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** A target goal's line-of-sight and reach rules, for PedTargets. */
@Mixin(TargetGoal.class)
public interface TargetGoalAccessor {
	@Accessor("mustSee")
	boolean libertycraft$mustSee();

	@Accessor("mustReach")
	boolean libertycraft$mustReach();
}
