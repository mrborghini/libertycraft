package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostCover;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** The undead don't burn under GTA IV's roofs, bridges and el-train tracks, nor in its interiors (HostCover). */
@Mixin(Mob.class)
public abstract class SunCoverMixin {
	@WrapOperation(method = "isSunBurnTick", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;canSeeSky(Lnet/minecraft/core/BlockPos;)Z"))
	private boolean libertycraft$gtaCover(Level level, BlockPos pos, Operation<Boolean> original) {
		return HostCover.seesSky(level, pos, original.call(level, pos));
	}
}
