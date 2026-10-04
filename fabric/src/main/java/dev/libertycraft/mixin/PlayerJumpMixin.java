package dev.libertycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.LibertyCraft;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.network.ServerGamePacketListenerImpl;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;

/**
 * GTA IV puts the player where its own player is, and the client follows it (Niko mode, vehicles,
 * respawns, doors), so a move packet can carry the player hundreds of blocks at once; LibertyCraft
 * turns the "moved too quickly" check off for that. Vanilla still moves the server's player there
 * through collision: every block and entity in the box swept along the way, loading and generating
 * the chunks it crosses. After a death the server respawns the player at the world spawn (deep
 * under Liberty City) and the client then follows GTA IV's player back: that sweep held the server
 * for 4.5 s ("Can't keep up! ... 90 ticks behind"). A jump that long is a teleport: the player (or the
 * vehicle they steer) is placed there without the sweep, and no fall is counted for it.
 */
@Mixin(ServerGamePacketListenerImpl.class)
public abstract class PlayerJumpMixin {
	/** Moves longer than this (blocks) in one packet are placed, not swept through collision. */
	@Unique
	private static final double TELEPORT_DISTANCE = 32.0;

	@WrapOperation(
		method = "handlePlayerPositionChange",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/server/level/ServerPlayer;move(Lnet/minecraft/world/entity/MoverType;Lnet/minecraft/world/phys/Vec3;)V")
	)
	private void libertycraft$placeLongJumps(ServerPlayer player, MoverType type, Vec3 delta, Operation<Void> original) {
		if (delta.lengthSqr() <= TELEPORT_DISTANCE * TELEPORT_DISTANCE) {
			original.call(player, type, delta);
			return;
		}
		LibertyCraft.LOG.info("[LibertyCraft] {} jumped {} blocks in one move (GTA IV put them there): placed, not swept", player.getPlainTextName(),
			String.format("%.0f", delta.length()));
		player.setPos(player.getX() + delta.x, player.getY() + delta.y, player.getZ() + delta.z);
		player.resetFallDistance();
	}

	/** The same jump is no fall either (vanilla would add a long drop to the fall distance). */
	@WrapOperation(
		method = "handlePlayerPositionChange",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/server/level/ServerPlayer;doCheckFallDamage(DDDZ)V")
	)
	private void libertycraft$noFallForJumps(ServerPlayer player, double x, double y, double z, boolean onGround, Operation<Void> original) {
		if (x * x + y * y + z * z > TELEPORT_DISTANCE * TELEPORT_DISTANCE) {
			original.call(player, 0.0, 0.0, 0.0, onGround);
		} else {
			original.call(player, x, y, z, onGround);
		}
	}

	/** A vehicle the player steers (the mount under a GTA IV car) jumps the same way when GTA IV moves its car far. */
	@WrapOperation(
		method = "handleMoveVehicle",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/Entity;move(Lnet/minecraft/world/entity/MoverType;Lnet/minecraft/world/phys/Vec3;)V")
	)
	private void libertycraft$placeLongVehicleJumps(Entity vehicle, MoverType type, Vec3 delta, Operation<Void> original) {
		if (delta.lengthSqr() <= TELEPORT_DISTANCE * TELEPORT_DISTANCE) {
			original.call(vehicle, type, delta);
			return;
		}
		LibertyCraft.LOG.info("[LibertyCraft] {} jumped {} blocks in one move (GTA IV put it there): placed, not swept", vehicle.getPlainTextName(),
			String.format("%.0f", delta.length()));
		vehicle.setPos(vehicle.getX() + delta.x, vehicle.getY() + delta.y, vehicle.getZ() + delta.z);
		vehicle.resetFallDistance();
	}

	@WrapOperation(
		method = "handleMoveVehicle",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/Entity;doCheckFallDamage(DDDZ)V")
	)
	private void libertycraft$noFallForVehicleJumps(Entity vehicle, double x, double y, double z, boolean onGround, Operation<Void> original) {
		if (x * x + y * y + z * z > TELEPORT_DISTANCE * TELEPORT_DISTANCE) {
			original.call(vehicle, 0.0, 0.0, 0.0, onGround);
		} else {
			original.call(vehicle, x, y, z, onGround);
		}
	}
}
