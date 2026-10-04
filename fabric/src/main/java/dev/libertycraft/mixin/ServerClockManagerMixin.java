package dev.libertycraft.mixin;

import dev.libertycraft.world.HostSky;
import net.minecraft.core.Holder;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.clock.ClockTimeMarker;
import net.minecraft.world.clock.ServerClockManager;
import net.minecraft.world.clock.WorldClock;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Setting Minecraft's clock ({@code /time set}, {@code /time add}, a time marker such as day or night,
 * waking up after a night in bed) sets GTA IV's clock too (HostSky).
 */
@Mixin(ServerClockManager.class)
public abstract class ServerClockManagerMixin {
	@Inject(method = "setTotalTicks", at = @At("RETURN"))
	private void libertycraft$setGta(Holder<WorldClock> clock, long totalTicks, CallbackInfo ci) {
		HostSky.clockChanged((ServerClockManager) (Object) this, clock, "set");
	}

	@Inject(method = "addTicks", at = @At("RETURN"))
	private void libertycraft$addGta(Holder<WorldClock> clock, int ticks, CallbackInfo ci) {
		HostSky.clockChanged((ServerClockManager) (Object) this, clock, "added to");
	}

	@Inject(method = "moveToTimeMarker", at = @At("RETURN"))
	private void libertycraft$markerGta(Holder<WorldClock> clock, ResourceKey<ClockTimeMarker> marker, CallbackInfoReturnable<ServerClockManager.MoveResult> cir) {
		if (cir.getReturnValue() == ServerClockManager.MoveResult.MOVED) {
			HostSky.clockChanged((ServerClockManager) (Object) this, clock, "moved to " + marker.identifier().getPath());
		}
	}
}
