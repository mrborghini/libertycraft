package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.city.BlockyCity;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.portal.PortalForcer;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Where no spot for a new portal is found, vanilla builds one on an obsidian platform at least at
 * y 70. Liberty City's streets are a few metres above the sea: between the mirror world and the
 * blocky city the platform goes where the player is instead (in the mirror world, which has no
 * blocks of its own, that is always the case: on GTA IV's ground under the player).
 */
@Mixin(PortalForcer.class)
public abstract class CityPortalForcerMixin {
	@Shadow
	@Final
	private ServerLevel level;

	@WrapOperation(method = "createPortal", at = @At(value = "INVOKE", target = "Ljava/lang/Math;max(II)I"))
	private int libertycraft$noFloorAt70(int lowest, int seventy, Operation<Integer> original) {
		return BlockyCity.isPortalPair(this.level) ? lowest : original.call(lowest, seventy);
	}
}
