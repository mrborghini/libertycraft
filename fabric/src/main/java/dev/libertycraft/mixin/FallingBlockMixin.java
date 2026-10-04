package dev.libertycraft.mixin;

import dev.libertycraft.world.city.BlockyCity;
import dev.libertycraft.world.HostCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.FallingBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Sand and gravel rest on GTA IV ground instead of falling forever through it. */
@Mixin(FallingBlock.class)
public abstract class FallingBlockMixin {
	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void libertycraft$restOnHost(BlockState state, ServerLevel level, BlockPos pos, RandomSource random, CallbackInfo ci) {
		if (!BlockyCity.isCity(level) && HostCollision.supportsFromBelow(pos)) {
			ci.cancel();
		}
	}
}
