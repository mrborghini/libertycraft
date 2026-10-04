package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostSky;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.gamerules.GameRule;
import net.minecraft.world.level.gamerules.GameRules;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * While Minecraft's weather follows GTA IV's (HostSky), its own weather cycle stands still (as with the
 * advance_weather game rule off, without touching the world's rules); rain and thunder still fade in
 * and out. Sleeping through a rainy night clears GTA IV's weather as it clears Minecraft's.
 */
@Mixin(ServerLevel.class)
public abstract class ServerLevelWeatherMixin {
	@WrapOperation(
		method = "advanceWeatherCycle",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/gamerules/GameRules;get(Lnet/minecraft/world/level/gamerules/GameRule;)Ljava/lang/Object;")
	)
	private Object libertycraft$gtaWeather(GameRules rules, GameRule<?> rule, Operation<Object> original) {
		Object value = original.call(rules, rule);
		return rule == GameRules.ADVANCE_WEATHER && HostSky.followsGta() ? Boolean.FALSE : value;
	}

	@Inject(method = "resetWeatherCycle", at = @At("HEAD"))
	private void libertycraft$sleptThrough(CallbackInfo ci) {
		HostSky.weatherSet(12000, 0, false, false, "cleared by sleeping");
	}
}
