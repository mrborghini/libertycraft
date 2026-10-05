package dev.libertycraft.mixin;

import dev.libertycraft.world.HostCover;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.biome.Biome;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Where it rains in the mirror world: everywhere GTA IV's cover (HostCover) doesn't keep it off. The void
 * biome has no rain of its own, so before this nothing ever got wet there (rain put out no fires and
 * didn't stop the undead burning); now rain wets mobs and the player and puts out fires in the open, and
 * not under a GTA roof or indoors.
 */
@Mixin(Level.class)
public abstract class LevelRainMixin {
	@Shadow
	public abstract boolean isRaining();

	@Inject(method = "precipitationAt", at = @At("HEAD"), cancellable = true)
	private void libertycraft$gtaRain(BlockPos pos, CallbackInfoReturnable<Biome.Precipitation> cir) {
		Level self = (Level) (Object) this;
		if (!HostCover.applies(self)) {
			return;
		}
		cir.setReturnValue(this.isRaining() && !HostCover.covered(self, pos) ? Biome.Precipitation.RAIN : Biome.Precipitation.NONE);
	}
}
