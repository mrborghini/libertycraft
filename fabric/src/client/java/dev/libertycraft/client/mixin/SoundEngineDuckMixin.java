package dev.libertycraft.client.mixin;

import com.llamalad7.mixinextras.injector.ModifyReturnValue;
import dev.libertycraft.client.SoundDuck;
import net.minecraft.client.sounds.SoundEngine;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Every sound's volume times SoundDuck's gain: Minecraft's sounds duck during GTA IV's phone calls. */
@Mixin(SoundEngine.class)
public abstract class SoundEngineDuckMixin {
	@ModifyReturnValue(method = "calculateVolume(FLnet/minecraft/sounds/SoundSource;)F", at = @At("RETURN"))
	private float libertycraft$duck(float volume) {
		return volume * SoundDuck.gain();
	}
}
