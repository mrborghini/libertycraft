package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostCover;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.ai.goal.FleeSunGoal;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * A burning skeleton (or stray, or bogged) with nothing to attack looks for shade under GTA IV's cover as
 * vanilla's look for a roof: a spot under an el-train track or a balcony, a doorway (HostCover). Vanilla's
 * priorities stay, so a mob with a target keeps fighting.
 */
@Mixin(FleeSunGoal.class)
public abstract class FleeSunCoverMixin {
	@WrapOperation(method = "canUse", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;canSeeSky(Lnet/minecraft/core/BlockPos;)Z"))
	private boolean libertycraft$gtaCover(Level level, BlockPos pos, Operation<Boolean> original) {
		return HostCover.seesSky(level, pos, original.call(level, pos));
	}

	@WrapOperation(method = "getHidePos", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;canSeeSky(Lnet/minecraft/core/BlockPos;)Z"))
	private boolean libertycraft$gtaShade(Level level, BlockPos pos, Operation<Boolean> original) {
		return HostCover.seesSkyForShade(level, pos, original.call(level, pos));
	}
}
