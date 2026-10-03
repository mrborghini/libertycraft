package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.link.Proto;
import dev.libertycraft.link.Link;
import dev.libertycraft.world.HostDigBlast;
import java.util.List;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Explosion;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft explosions in Liberty City: they blow GTA IV's ground and rock apart like blocks
 * (HostDigBlast), and GTA IV feels them (loose objects are thrown and people knocked away).
 */
@Mixin(ServerExplosion.class)
public abstract class ServerExplosionMixin {
	@Unique
	private @Nullable HostDigBlast libertycraft$blast;

	@Inject(method = "explode", at = @At("HEAD"))
	private void libertycraft$begin(CallbackInfoReturnable<Integer> cir) {
		this.libertycraft$blast = HostDigBlast.begin((ServerExplosion) (Object) this);
	}

	@WrapOperation(
		method = "calculateExplodedPositions",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/ExplosionDamageCalculator;getBlockExplosionResistance(Lnet/minecraft/world/level/Explosion;Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/material/FluidState;)Ljava/util/Optional;"
		)
	)
	private Optional<Float> libertycraft$hostResists(
		ExplosionDamageCalculator calculator, Explosion explosion, BlockGetter level, BlockPos pos, BlockState block, FluidState fluid, Operation<Optional<Float>> original
	) {
		Optional<Float> vanilla = original.call(calculator, explosion, level, pos, block, fluid);
		return this.libertycraft$blast != null ? this.libertycraft$blast.resistance(pos, vanilla) : vanilla;
	}

	@ModifyVariable(method = "explode", at = @At("STORE"), ordinal = 0)
	private List<BlockPos> libertycraft$hostBreaks(List<BlockPos> targets) {
		if (this.libertycraft$blast != null) {
			ServerExplosion self = (ServerExplosion) (Object) this;
			this.libertycraft$blast.materialize(targets, self.getBlockInteraction() != Explosion.BlockInteraction.KEEP
				&& self.getBlockInteraction() != Explosion.BlockInteraction.TRIGGER_BLOCK);
		}
		return targets;
	}

	@Inject(method = "explode", at = @At("RETURN"))
	private void libertycraft$tellHost(CallbackInfoReturnable<Integer> cir) {
		if (this.libertycraft$blast != null) {
			this.libertycraft$blast.finish();
			this.libertycraft$blast = null;
		}
		if (!Link.active()) {
			return;
		}
		ServerExplosion self = (ServerExplosion) (Object) this;
		var center = self.center();
		Link.pushEvent(Proto.EV_EXPLOSION, 0, (float) center.x, (float) center.y, (float) center.z, self.radius(), 0);
	}
}
