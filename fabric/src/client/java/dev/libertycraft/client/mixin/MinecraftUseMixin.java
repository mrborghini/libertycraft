package dev.libertycraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostClip;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.MultiPlayerGameMode;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Using an item on GTA IV geometry: fire starters light the ground and walls (HostClip.forItem). */
@Mixin(Minecraft.class)
public abstract class MinecraftUseMixin {
	@WrapOperation(
		method = "startUseItem",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/client/multiplayer/MultiPlayerGameMode;useItemOn(Lnet/minecraft/client/player/LocalPlayer;Lnet/minecraft/world/InteractionHand;Lnet/minecraft/world/phys/BlockHitResult;)Lnet/minecraft/world/InteractionResult;"
		)
	)
	private InteractionResult libertycraft$useOnHost(
		MultiPlayerGameMode gameMode, LocalPlayer player, InteractionHand hand, BlockHitResult hit, Operation<InteractionResult> original
	) {
		if (dev.libertycraft.world.city.BlockyCity.isCity(player.level())) {
			return original.call(gameMode, player, hand, hit);
		}
		return original.call(gameMode, player, hand, HostClip.forItem(player.getItemInHand(hand), hit));
	}
}
