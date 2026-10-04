package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostClip;
import dev.libertycraft.world.city.BlockyCity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Mobs don't see through GTA IV's walls: their line of sight (targeting players and peds, ranged
 * attacks) meets GTA IV's collision as well as Minecraft's blocks, as arrows do (HostClip).
 */
@Mixin(LivingEntity.class)
public abstract class MobSightMixin {
	@WrapOperation(
		method = "hasLineOfSight(Lnet/minecraft/world/entity/Entity;Lnet/minecraft/world/level/ClipContext$Block;Lnet/minecraft/world/level/ClipContext$Fluid;D)Z",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private BlockHitResult libertycraft$seeGta(Level level, ClipContext context, Operation<BlockHitResult> original) {
		BlockHitResult vanilla = original.call(level, context);
		if (!((Object) this instanceof Mob) || level.isClientSide() || BlockyCity.isCity(level)) {
			return vanilla;
		}
		return HostClip.refine(context.getFrom(), context.getTo(), vanilla, HostClip.Use.PROJECTILE);
	}
}
