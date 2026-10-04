package dev.libertycraft.mixin;

import dev.libertycraft.world.HostDrive;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.vehicle.boat.AbstractBoat;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The boat a player rides while GTA IV drives their vehicle (HostDrive) is only a seat: nothing
 * stands on it, bumps it, gets shoved by it or targets it.
 */
@Mixin(AbstractBoat.class)
public abstract class HostMountBoatMixin {
	@Shadow
	private float outOfControlTicks;

	/**
	 * A boat under water throws its riders off after 3 s; the mount is GTA IV's seat (its car may sit
	 * in Minecraft water): it never does, or the server mounted the player again every tick and the
	 * boat flickered in and out.
	 */
	@Inject(method = "tick", at = @At("HEAD"))
	private void libertycraft$mountKeepsItsRider(CallbackInfo ci) {
		if (HostDrive.isMount((Entity) (Object) this)) {
			this.outOfControlTicks = 0.0F;
		}
	}

	@Inject(method = "canBeCollidedWith", at = @At("HEAD"), cancellable = true)
	private void libertycraft$mountIsNotSolid(Entity other, CallbackInfoReturnable<Boolean> cir) {
		if (HostDrive.isMount((Entity) (Object) this)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(method = "isPushable", at = @At("HEAD"), cancellable = true)
	private void libertycraft$mountIsNotPushable(CallbackInfoReturnable<Boolean> cir) {
		if (HostDrive.isMount((Entity) (Object) this)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(method = "isPickable", at = @At("HEAD"), cancellable = true)
	private void libertycraft$mountIsNotPickable(CallbackInfoReturnable<Boolean> cir) {
		if (HostDrive.isMount((Entity) (Object) this)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(method = "push", at = @At("HEAD"), cancellable = true)
	private void libertycraft$mountPushesNobody(Entity entity, CallbackInfo ci) {
		if (HostDrive.isMount((Entity) (Object) this)) {
			ci.cancel();
		}
	}
}
