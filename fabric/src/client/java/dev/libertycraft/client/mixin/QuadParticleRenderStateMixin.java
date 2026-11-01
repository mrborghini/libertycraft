package dev.libertycraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapmethod.WrapMethod;
import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import dev.libertycraft.client.HostClient;
import dev.libertycraft.client.render.ParticleFacing;
import java.util.HashMap;
import java.util.Map;
import net.minecraft.client.particle.SingleQuadParticle;
import net.minecraft.client.renderer.state.level.QuadParticleRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * While linked, particles that face the camera are kept with their roll only, so GTA IV can turn them to
 * its own camera (ParticleFacing); which ones do is noted per layer, in the order buildLayer emits them.
 */
@Mixin(QuadParticleRenderState.class)
public abstract class QuadParticleRenderStateMixin implements ParticleFacing.Holder {
	@Unique
	private final Map<SingleQuadParticle.Layer, ParticleFacing.Layer> libertycraft$facing = new HashMap<>();
	@Unique
	private final float[] libertycraft$rotation = new float[4];

	@Override
	public Map<SingleQuadParticle.Layer, ParticleFacing.Layer> libertycraft$facing() {
		return this.libertycraft$facing;
	}

	@WrapMethod(method = "add")
	private void libertycraft$cameraPlane(SingleQuadParticle.Layer layer, float x, float y, float z, float qx, float qy, float qz, float qw, float size,
		float u0, float u1, float v0, float v1, int color, int light, Operation<Void> original) {
		boolean faces = HostClient.linked() && ParticleFacing.toCameraPlane(qx, qy, qz, qw, this.libertycraft$rotation);
		this.libertycraft$facing.computeIfAbsent(layer, l -> new ParticleFacing.Layer()).add(faces);
		if (faces) {
			float[] q = this.libertycraft$rotation;
			original.call(layer, x, y, z, q[0], q[1], q[2], q[3], size, u0, u1, v0, v1, color, light);
		} else {
			original.call(layer, x, y, z, qx, qy, qz, qw, size, u0, u1, v0, v1, color, light);
		}
	}

	@Inject(method = "clear", at = @At("HEAD"))
	private void libertycraft$clearFacing(CallbackInfo ci) {
		this.libertycraft$facing.clear();
	}
}
