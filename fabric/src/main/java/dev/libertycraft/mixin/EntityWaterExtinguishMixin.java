package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostWater;
import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Touching GTA IV's water puts a fire out at once, as Minecraft water does. Minecraft's own water does it
 * through the water block's inside effect (EXTINGUISH), which GTA IV's water (air to Minecraft, see
 * {@link HostWater}) never has; its touching-water state (EntityFluidInteractionMixin) does count it, so
 * an entity touching water is put out with the rain's check, extinguish sound included.
 */
@Mixin(Entity.class)
public abstract class EntityWaterExtinguishMixin {
	@WrapOperation(method = "applyEffectsFromBlocks(Ljava/util/List;)V", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/Entity;isInRain()Z"))
	private boolean libertycraft$putOutInHostWater(Entity self, Operation<Boolean> original) {
		return original.call(self) || HostWater.active() && self.isInWater();
	}
}
