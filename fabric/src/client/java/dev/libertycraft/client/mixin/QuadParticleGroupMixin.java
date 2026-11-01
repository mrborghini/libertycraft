package dev.libertycraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.client.HostClient;
import net.minecraft.client.particle.QuadParticleGroup;
import net.minecraft.client.renderer.culling.Frustum;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * While linked, particles aren't culled to Minecraft's view: GTA IV draws them with its own camera, which
 * can look where Minecraft's doesn't (GTA IV drives a car, or turned between Minecraft's frames).
 * Minecraft doesn't draw its own level meanwhile (LevelRendererMixin), so its picture doesn't change.
 */
@Mixin(QuadParticleGroup.class)
public abstract class QuadParticleGroupMixin {
	@WrapOperation(
		method = "extractRenderState",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/culling/Frustum;pointInFrustum(DDD)Z")
	)
	private boolean libertycraft$allForGta(Frustum frustum, double x, double y, double z, Operation<Boolean> original) {
		return HostClient.linked() || original.call(frustum, x, y, z);
	}
}
