package dev.libertycraft.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.libertycraft.client.HostClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The MC window is hidden while linked; GTA IV has the real focus, so pretend we do too. */
@Mixin(Window.class)
public abstract class WindowMixin {
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	private void libertycraft$focused(CallbackInfoReturnable<Boolean> cir) {
		if (HostClient.tookOver()) {
			// Focused while GTA IV is connected; if GTA IV goes away, act unfocused so MC
			// never tries to grab the (hidden) mouse.
			cir.setReturnValue(HostClient.linked());
		}
	}

	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void libertycraft$notIconified(CallbackInfoReturnable<Boolean> cir) {
		if (HostClient.linked()) {
			cir.setReturnValue(false);
		}
	}
}
