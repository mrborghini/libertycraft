package dev.libertycraft.mixin;

import dev.libertycraft.world.HostPath;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.PathfindingContext;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * A cell with GTA IV's ground low in it (or on top of the cell below) is one to walk in (HostPath),
 * as the cell above a slab is for vanilla. (A cell whose ground sits high is BLOCKED, and vanilla
 * makes the one above it walkable: PathfindingContextMixin.)
 */
@Mixin(WalkNodeEvaluator.class)
public abstract class WalkNodeEvaluatorMixin {
	@Inject(
		method = "getPathTypeStatic(Lnet/minecraft/world/level/pathfinder/PathfindingContext;Lnet/minecraft/core/BlockPos$MutableBlockPos;)Lnet/minecraft/world/level/pathfinder/PathType;",
		at = @At("RETURN"),
		cancellable = true
	)
	private static void libertycraft$gtaFloor(PathfindingContext context, BlockPos.MutableBlockPos pos, CallbackInfoReturnable<PathType> cir) {
		if (cir.getReturnValue() == PathType.OPEN && ((HostPath.Context) context).libertycraft$gta() && HostPath.floor(pos)) {
			cir.setReturnValue(WalkNodeEvaluator.checkNeighbourBlocks(context, pos.getX(), pos.getY(), pos.getZ(), PathType.WALKABLE));
		}
	}
}
