package dev.libertycraft.client.mixin;

import dev.libertycraft.client.HostDigClient;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** A dug block breaking reveals what's around it (HostDigClient). */
@Mixin(ClientLevel.class)
public abstract class ClientLevelDigMixin {
	@Inject(method = "setBlocksDirty", at = @At("HEAD"))
	private void libertycraft$blockChanged(BlockPos pos, BlockState oldState, BlockState newState, CallbackInfo ci) {
		HostDigClient.blockChanged((ClientLevel) (Object) this, pos, oldState, newState);
	}
}
