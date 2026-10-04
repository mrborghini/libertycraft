package dev.libertycraft.mixin;

import dev.libertycraft.world.HostSky;
import net.minecraft.server.MinecraftServer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** {@code /weather clear, rain, thunder} set GTA IV's weather too (HostSky). */
@Mixin(MinecraftServer.class)
public abstract class MinecraftServerWeatherMixin {
	@Inject(method = "setWeatherParameters", at = @At("HEAD"))
	private void libertycraft$setGta(int clearTicks, int rainTicks, boolean raining, boolean thundering, CallbackInfo ci) {
		HostSky.weatherSet(clearTicks, rainTicks, raining, thundering, "set");
	}
}
