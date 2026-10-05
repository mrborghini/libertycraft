package dev.libertycraft.mixin;

import dev.libertycraft.world.MobHold;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** A mob over ground GTA IV hasn't described yet doesn't move: it would fall into the void (MobHold). */
@Mixin(LivingEntity.class)
public abstract class MobHoldMixin {
	@Inject(method = "travel", at = @At("HEAD"), cancellable = true)
	private void libertycraft$holdOverUnknown(Vec3 input, CallbackInfo ci) {
		if ((Object) this instanceof Mob mob && !mob.level().isClientSide() && MobHold.holds(mob)) {
			mob.setDeltaMovement(Vec3.ZERO);
			mob.resetFallDistance();
			ci.cancel();
		}
	}
}
