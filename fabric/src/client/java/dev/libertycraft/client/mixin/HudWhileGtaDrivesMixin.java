package dev.libertycraft.client.mixin;

import com.llamalad7.mixinextras.injector.ModifyExpressionValue;
import com.llamalad7.mixinextras.injector.WrapWithCondition;
import dev.libertycraft.client.HostDriveClient;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.gui.Font;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.Hud;
import net.minecraft.client.gui.contextualbar.ContextualBar;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * While GTA IV drives the player (a vehicle, getting in, getting back up after a bail-out or a
 * knockdown) GTA's camera and aim are in charge: Minecraft's HUD keeps only the hotbar (with the
 * selected item and the offhand slot), the hearts and the food bar. Gone meanwhile: the crosshair
 * and both attack indicators, armour, the experience bar and level (and the locator and jump
 * bars in their place), air bubbles, the mount's hearts (the food bar shows instead), the action
 * bar message ("Press Left Shift to dismount"), titles, the held item's name, status effects, boss
 * bars and the camera overlays (pumpkin, vignette, portal, powder snow). Chat, the scoreboard,
 * subtitles and toasts stay. On foot in Minecraft mode nothing changes.
 */
@Mixin(Hud.class)
public abstract class HudWhileGtaDrivesMixin {
	private static boolean libertycraft$minimal() {
		return HostDriveClient.driving();
	}

	@Inject(method = "extractCrosshair", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noCrosshair(GuiGraphicsExtractor graphics, DeltaTracker deltaTracker, CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	@Inject(method = "extractCameraOverlays", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noCameraOverlays(GuiGraphicsExtractor graphics, DeltaTracker deltaTracker, CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	@Inject(method = "extractEffects", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noEffects(GuiGraphicsExtractor graphics, DeltaTracker deltaTracker, CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	@Inject(method = "extractBossOverlay", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noBossBars(GuiGraphicsExtractor graphics, DeltaTracker deltaTracker, CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	@Inject(method = "extractOverlayMessage", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noActionBar(GuiGraphicsExtractor graphics, DeltaTracker deltaTracker, CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	@Inject(method = "extractTitle", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noTitle(GuiGraphicsExtractor graphics, DeltaTracker deltaTracker, CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	@Inject(method = "extractSelectedItemName", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noItemName(GuiGraphicsExtractor graphics, CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	@Inject(method = "extractArmor", at = @At("HEAD"), cancellable = true)
	private static void libertycraft$noArmor(GuiGraphicsExtractor graphics, Player player, int yLineBase, int numHealthRows, int healthRowHeight, int xLeft,
		CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	@Inject(method = "extractAirBubbles", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noAir(GuiGraphicsExtractor graphics, Player player, int vehicleHearts, int yLineAir, int xRight, CallbackInfo ci) {
		if (libertycraft$minimal()) {
			ci.cancel();
		}
	}

	/** No mount hearts (GTA's vehicle carries the player); with none the food bar shows in their place. */
	@Inject(method = "getPlayerVehicleWithHealth", at = @At("HEAD"), cancellable = true)
	private void libertycraft$noMountHealth(CallbackInfoReturnable<LivingEntity> cir) {
		if (libertycraft$minimal()) {
			cir.setReturnValue(null);
		}
	}

	/** The experience, locator or jump bar behind the hotbar. */
	@WrapWithCondition(
		method = "extractHotbarAndDecorations",
		at = @At(value = "INVOKE",
			target = "Lnet/minecraft/client/gui/contextualbar/ContextualBar;extractBackground(Lnet/minecraft/client/gui/GuiGraphicsExtractor;Lnet/minecraft/client/DeltaTracker;)V")
	)
	private boolean libertycraft$noContextualBarBackground(ContextualBar bar, GuiGraphicsExtractor graphics, DeltaTracker deltaTracker) {
		return !libertycraft$minimal();
	}

	@WrapWithCondition(
		method = "extractHotbarAndDecorations",
		at = @At(value = "INVOKE",
			target = "Lnet/minecraft/client/gui/contextualbar/ContextualBar;extractRenderState(Lnet/minecraft/client/gui/GuiGraphicsExtractor;Lnet/minecraft/client/DeltaTracker;)V")
	)
	private boolean libertycraft$noContextualBar(ContextualBar bar, GuiGraphicsExtractor graphics, DeltaTracker deltaTracker) {
		return !libertycraft$minimal();
	}

	@WrapWithCondition(
		method = "extractHotbarAndDecorations",
		at = @At(value = "INVOKE",
			target = "Lnet/minecraft/client/gui/contextualbar/ContextualBar;extractExperienceLevel(Lnet/minecraft/client/gui/GuiGraphicsExtractor;Lnet/minecraft/client/gui/Font;I)V")
	)
	private boolean libertycraft$noExperienceLevel(GuiGraphicsExtractor graphics, Font font, int experienceLevel) {
		return !libertycraft$minimal();
	}

	/** The attack indicator beside the hotbar (AttackIndicator=hotbar) draws only below full strength. */
	@ModifyExpressionValue(
		method = "extractItemHotbar",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/player/LocalPlayer;getAttackStrengthScale(F)F")
	)
	private float libertycraft$noHotbarAttackIndicator(float strength) {
		return libertycraft$minimal() ? 1.0F : strength;
	}
}
