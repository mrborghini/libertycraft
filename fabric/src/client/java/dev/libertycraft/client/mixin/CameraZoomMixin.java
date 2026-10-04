package dev.libertycraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostClip;
import net.minecraft.client.Camera;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * The third-person camera (F5) pulls in against GTA IV's walls, terrain and trees as well as
 * Minecraft blocks, the way Minecraft's own camera does against blocks.
 */
@Mixin(Camera.class)
public abstract class CameraZoomMixin {
	@WrapOperation(
		method = "getMaxZoom",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private BlockHitResult libertycraft$zoomAgainstHost(Level level, ClipContext context, Operation<BlockHitResult> original) {
		if (dev.libertycraft.world.city.BlockyCity.isCity(level)) {
			return original.call(level, context);
		}
		return HostClip.refine(context.getFrom(), context.getTo(), original.call(level, context), HostClip.Use.PROJECTILE);
	}
}
