package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostPath;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Knockback on the player starts from what the client says he is doing, not from the server's own copy of
 * his motion. The server simulates the player's movement too (vanilla's AUTHORITATIVE_SIDE_AND_SERVER), but
 * he collides with GTA IV's ground on the client only (its exact triangles): on the server he keeps
 * falling between the client's position updates, so his motion there sits near terminal speed (about 3.5
 * blocks a tick down) and his onGround is mostly false. Vanilla's knockback keeps that downward motion when
 * he isn't on the ground and adds the push to it: every blow then pushed him only sideways, and an iron
 * golem's launch (0.4 up on top of the knockback) was swallowed. Now the knockback takes the client's last
 * movement, and he is on the ground when the client says so (no vertical movement or a step down).
 */
@Mixin(LivingEntity.class)
public abstract class PlayerKnockbackMixin {
	@Inject(method = "knockback(DDDLnet/minecraft/world/damagesource/DamageSource;FZ)V", at = @At("HEAD"))
	private void libertycraft$fromClientMotion(double power, double xd, double zd, DamageSource source, float damage, boolean comesFromEffect, CallbackInfo ci) {
		if ((Object) this instanceof ServerPlayer player && HostPath.applies(player.level())) {
			Vec3 known = player.getKnownMovement();
			player.setDeltaMovement(known.x, Math.max(known.y, -0.5), known.z);
		}
	}

	@WrapOperation(method = "knockback(DDDLnet/minecraft/world/damagesource/DamageSource;FZ)V", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/LivingEntity;onGround()Z"))
	private boolean libertycraft$clientOnGround(LivingEntity self, Operation<Boolean> original) {
		if (self instanceof ServerPlayer player && HostPath.applies(player.level())) {
			double dy = player.getKnownMovement().y;
			return original.call(self) || (dy <= 0.0 && dy > -0.2);
		}
		return original.call(self);
	}
}
