package dev.libertycraft.mixin;

import dev.libertycraft.world.HostPath;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.navigation.GroundPathNavigation;
import net.minecraft.world.entity.ai.navigation.PathNavigation;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.chunk.LevelChunk;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * A path to something standing on GTA IV's ground ends there (HostPath), not at the top of the void
 * world: vanilla looks down the column for a block to stand on, finds none, and aims at the build
 * limit instead.
 */
@Mixin(GroundPathNavigation.class)
public abstract class GroundPathNavigationMixin extends PathNavigation {
	private GroundPathNavigationMixin(Mob mob, Level level) {
		super(mob, level);
	}

	@Inject(method = "findSurfacePosition", at = @At("HEAD"), cancellable = true)
	private void libertycraft$gtaSurface(LevelChunk chunk, BlockPos pos, int reachRange, CallbackInfoReturnable<BlockPos> cir) {
		if (!chunk.getBlockState(pos).isAir() || !HostPath.applies(this.level)) {
			return;
		}
		BlockPos ground = HostPath.standable(pos, 4, 8);
		if (ground != null) {
			cir.setReturnValue(ground);
		}
	}
}
