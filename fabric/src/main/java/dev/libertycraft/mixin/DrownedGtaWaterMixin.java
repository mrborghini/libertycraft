package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostWater;
import dev.libertycraft.world.city.BlockyCity;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * A drowned on land by day looks for water to go to (vanilla's DrownedGoToWaterGoal: water blocks within
 * 10 blocks); GTA IV's rivers, harbour and sea count (HostWater: where GTA IV has told Minecraft about its
 * water, the columns around the player), so it heads for them, swims and hunts there as in Minecraft.
 * Only water open up to its surface: not the cells under a beach or a quay that GTA IV's water plane
 * runs under (HostWater.openAt).
 */
@Mixin(targets = "net.minecraft.world.entity.monster.zombie.Drowned$DrownedGoToWaterGoal")
public abstract class DrownedGtaWaterMixin {
	@WrapOperation(method = "getWaterPos", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;getBlockState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/block/state/BlockState;"))
	private BlockState libertycraft$gtaWater(Level level, BlockPos pos, Operation<BlockState> original) {
		BlockState state = original.call(level, pos);
		if (state.isAir() && HostWater.active() && !BlockyCity.isCity(level) && HostWater.openAt(pos)) {
			return Blocks.WATER.defaultBlockState();
		}
		return state;
	}
}
