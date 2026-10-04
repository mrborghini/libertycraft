package dev.libertycraft.mixin;

import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.GoalSelector;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** A mob's target goals, for PedTargets (Minecraft's hostile mobs hunt GTA IV's peds too). */
@Mixin(Mob.class)
public interface MobTargetSelectorAccessor {
	@Accessor("targetSelector")
	GoalSelector libertycraft$targetSelector();
}
