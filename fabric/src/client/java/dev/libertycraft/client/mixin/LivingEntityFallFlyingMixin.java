package dev.libertycraft.client.mixin;

import dev.libertycraft.client.MoverClient;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * The local player's elytra move: its motion before and after the collisions, for {@link MoverClient} (elytra
 * crashes: flying into GTA IV's walls hurts as Minecraft's do, a hard crash knocks the player over in GTA IV).
 */
@Mixin(LivingEntity.class)
public abstract class LivingEntityFallFlyingMixin {
	@Unique
	private Vec3 libertycraft$beforeFlight;

	@Inject(method = "travelFallFlying", at = @At("HEAD"))
	private void libertycraft$beforeFlight(Vec3 input, CallbackInfo ci) {
		if ((Object) this instanceof LocalPlayer player) {
			this.libertycraft$beforeFlight = player.getDeltaMovement();
			MoverClient.beforeFallFlyingMove();
		}
	}

	@Inject(method = "travelFallFlying", at = @At("TAIL"))
	private void libertycraft$afterFlight(Vec3 input, CallbackInfo ci) {
		if ((Object) this instanceof LocalPlayer player && this.libertycraft$beforeFlight != null) {
			MoverClient.fallFlyingMoved(player, this.libertycraft$beforeFlight, player.getDeltaMovement());
			this.libertycraft$beforeFlight = null;
		}
	}
}
