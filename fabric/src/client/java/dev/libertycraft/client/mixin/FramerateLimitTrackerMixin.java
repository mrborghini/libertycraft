package dev.libertycraft.client.mixin;

import com.mojang.blaze3d.platform.FramerateLimitTracker;
import dev.libertycraft.client.HostClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** HostClient.paceFrame() locks us to GTA IV's frame rate; don't let MC throttle on its own. */
@Mixin(FramerateLimitTracker.class)
public abstract class FramerateLimitTrackerMixin {
	@Inject(method = "getFramerateLimit", at = @At("HEAD"), cancellable = true)
	private void libertycraft$unlimited(CallbackInfoReturnable<Integer> cir) {
		if (HostClient.linked()) {
			cir.setReturnValue(260);
		}
	}
}
