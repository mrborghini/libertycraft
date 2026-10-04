package dev.libertycraft.client.mixin;

import dev.libertycraft.client.HostDriveClient;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * While GTA IV drives the player (a vehicle, getting back up, a cutscene, Niko mode) its own camera
 * shows the scene, and the player's body is drawn there with what it holds: Minecraft's first-person
 * hands and held items (the sword and shield at the screen's edges) stay out of the overlay. The HUD
 * (hotbar, hearts) still draws. Also while GTA IV's phone is out: it shows where the right hand is,
 * and the pickaxe looked like the phone.
 */
@Mixin(GameRenderer.class)
public abstract class GameRendererHandMixin {
	@Inject(method = "renderItemInHand", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noHandWhileGtaDrives(CallbackInfo ci) {
		if (HostDriveClient.driving() || (dev.libertycraft.client.HostClient.linked() && dev.libertycraft.client.HostClient.sky().phoneOut())) {
			ci.cancel();
		}
	}
}
