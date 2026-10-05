package dev.libertycraft.mixin;

import dev.libertycraft.world.HostPath;
import dev.libertycraft.world.HostWater;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.level.CollisionGetter;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.PathfindingContext;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft's pathfinder sees GTA IV's walls and ground (HostPath): an empty cell GTA IV's geometry
 * fills above its middle is BLOCKED, like a block. After the level's path type cache, so geometry
 * that streams in later counts at once.
 *
 * <p>For mobs that like water (no malus for it: the drowned), an empty cell under GTA IV's water
 * surface ({@link HostWater}, around the player) is WATER, so a drowned on land finds a path into the
 * water its goal picked there. Other mobs keep seeing it as open air over nothing (GTA IV's sea bed
 * isn't in the collision Minecraft gets), so they don't walk into it.
 */
@Mixin(PathfindingContext.class)
public abstract class PathfindingContextMixin implements HostPath.Context {
	@Unique
	private boolean libertycraft$gta;
	@Unique
	private boolean libertycraft$swims;
	@Unique
	private final BlockPos.MutableBlockPos libertycraft$pos = new BlockPos.MutableBlockPos();

	@Inject(method = "<init>", at = @At("RETURN"))
	private void libertycraft$overGta(CollisionGetter level, Mob mob, CallbackInfo ci) {
		this.libertycraft$gta = HostPath.applies(mob.level());
		this.libertycraft$swims = this.libertycraft$gta && mob.getPathfindingMalus(PathType.WATER) == 0.0F;
	}

	@Override
	public boolean libertycraft$gta() {
		return this.libertycraft$gta;
	}

	@Inject(method = "getPathTypeFromState", at = @At("RETURN"), cancellable = true)
	private void libertycraft$gtaSolid(int x, int y, int z, CallbackInfoReturnable<PathType> cir) {
		if (this.libertycraft$gta && cir.getReturnValue() == PathType.OPEN) {
			if (HostPath.solid(this.libertycraft$pos.set(x, y, z))) {
				cir.setReturnValue(PathType.BLOCKED);
			} else if (this.libertycraft$swims && HostWater.openAt(this.libertycraft$pos)) {
				cir.setReturnValue(PathType.WATER);
			}
		}
	}
}
