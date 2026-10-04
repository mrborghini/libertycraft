package dev.libertycraft.client.mixin;

import com.llamalad7.mixinextras.injector.ModifyExpressionValue;
import dev.libertycraft.client.GtaMenuPause;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Minecraft pauses while GTA IV's pause menu is open (GtaMenuPause): runTick's pause test (singleplayer, a
 * pausing screen, not open to friends) counts GTA IV's menu as a pausing screen.
 */
@Mixin(Minecraft.class)
public abstract class GtaMenuPauseMixin {
	@ModifyExpressionValue(method = "runTick", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/gui/Gui;isPausing()Z"))
	private boolean libertycraft$gtaMenuPauses(boolean pausing) {
		return pausing || GtaMenuPause.active();
	}
}
