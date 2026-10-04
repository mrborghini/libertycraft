package dev.libertycraft.mixin;

import dev.libertycraft.world.HostPath;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.ai.navigation.PathNavigation;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * A spot on GTA IV's ground (HostPath) is somewhere a mob can go: vanilla only takes a cell over a
 * real block, and the mirror world has none, so the random spots of its idle and fleeing goals were
 * never found. An untamed horse with a rider bucks and tames through such a goal
 * (RunAroundLikeCrazyGoal): it never started, so horses could never be tamed in Liberty City.
 */
@Mixin(PathNavigation.class)
public abstract class PathNavigationMixin {
	@Shadow
	@Final
	protected Level level;

	@Inject(method = "isStableDestination", at = @At("HEAD"), cancellable = true)
	private void libertycraft$gtaGroundIsStable(BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
		if (HostPath.applies(this.level) && this.level.getBlockState(pos.below()).isAir() && HostPath.floor(pos)) {
			cir.setReturnValue(true);
		}
	}
}
