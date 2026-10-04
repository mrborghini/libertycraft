package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.llamalad7.mixinextras.sugar.Local;
import dev.libertycraft.world.city.BlockyCity;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.NetherPortalBlock;
import net.minecraft.world.level.border.WorldBorder;
import net.minecraft.world.level.portal.PortalForcer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * A nether portal in GTA IV's mirror world leads to the blocky city and one there leads back
 * (BlockyCity): the destination level is swapped, everything else is vanilla (both dimensions have a
 * coordinate scale of 1, so it's the same X/Z). The pair looks for the other side's portal within 16
 * blocks, like the nether does, instead of the overworld's 128: a portal built elsewhere in the city
 * gets its own one at its own spot in GTA IV.
 */
@Mixin(NetherPortalBlock.class)
public abstract class CityPortalMixin {
	@WrapOperation(
		method = "getPortalDestination",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/server/MinecraftServer;getLevel(Lnet/minecraft/resources/ResourceKey;)Lnet/minecraft/server/level/ServerLevel;")
	)
	private ServerLevel libertycraft$toTheCity(MinecraftServer server, ResourceKey<Level> key, Operation<ServerLevel> original, @Local(argsOnly = true) ServerLevel from) {
		ServerLevel city = BlockyCity.portalTarget(server, from);
		return city != null ? city : original.call(server, key);
	}

	@WrapOperation(
		method = "getExitPortal",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/portal/PortalForcer;findClosestPortalPosition(Lnet/minecraft/core/BlockPos;ZLnet/minecraft/world/level/border/WorldBorder;)Ljava/util/Optional;"
		)
	)
	private Optional<BlockPos> libertycraft$nearbyOnly(PortalForcer forcer, BlockPos pos, boolean toNether, WorldBorder border, Operation<Optional<BlockPos>> original,
		@Local(argsOnly = true) ServerLevel destination) {
		return original.call(forcer, pos, toNether || BlockyCity.isPortalPair(destination), border);
	}
}
