package dev.libertycraft.mixin;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.combat.FireworkBlast;
import dev.libertycraft.combat.HostActorEntity;
import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import net.minecraft.core.component.DataComponents;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.FireworkRocketEntity;
import net.minecraft.world.item.component.Fireworks;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * A firework rocket with stars is GTA IV's RPG: its burst becomes GTA IV's rocket blast where it burst
 * (kEvExplosion with Proto.EXPLOSION_FIREWORK). Every burst goes through {@code explode}: at the end of
 * its flight, or early when it strikes a creature, a stand-in or a surface (Minecraft's or GTA IV's:
 * ProjectileMixin), from a crossbow, a hand launch, a dispenser or an elytra boost. A rocket that struck
 * something bursts where it struck it (it has moved on through it by then); a struck GTA IV actor's
 * stand-in is named in the event (formId), so the host knows a vehicle took the rocket itself.
 */
@Mixin(FireworkRocketEntity.class)
public abstract class FireworkRocketMixin {
	@Unique
	private @Nullable Vec3 libertycraft$struckAt;

	@Unique
	private int libertycraft$struck;

	@Inject(method = "onHitEntity", at = @At("HEAD"))
	private void libertycraft$noteEntityHit(EntityHitResult hit, CallbackInfo ci) {
		this.libertycraft$struckAt = hit.getLocation();
		this.libertycraft$struck = hit.getEntity() instanceof HostActorEntity proxy ? proxy.formId() : 0;
	}

	@Inject(method = "onHitBlock", at = @At("HEAD"))
	private void libertycraft$noteBlockHit(BlockHitResult hit, CallbackInfo ci) {
		this.libertycraft$struckAt = hit.getLocation();
		this.libertycraft$struck = 0;
	}

	@Inject(method = "explode", at = @At("HEAD"))
	private void libertycraft$tellHost(ServerLevel level, CallbackInfo ci) {
		if (!Link.active()) {
			return;
		}
		FireworkRocketEntity self = (FireworkRocketEntity) (Object) this;
		Fireworks fireworks = self.getItem().get(DataComponents.FIREWORKS);
		int stars = fireworks != null ? fireworks.explosions().size() : 0;
		float radius = FireworkBlast.radius(stars);
		if (radius <= 0.0F) {
			return; // no stars: vanilla makes no burst either
		}
		Vec3 at = this.libertycraft$struckAt != null ? this.libertycraft$struckAt : self.position();
		int flags = Proto.EXPLOSION_FIREWORK | (self.getOwner() instanceof Player ? Proto.EXPLOSION_BY_PLAYER : 0);
		Link.pushEvent(Proto.EV_EXPLOSION, this.libertycraft$struck, (float) at.x, (float) at.y, (float) at.z, radius, flags, stars);
		LibertyCraft.LOG.info("[LibertyCraft] firework burst ({} star(s), blast radius {}) at {} {} {}{}{}", stars, radius, String.format("%.1f", at.x),
			String.format("%.1f", at.y), String.format("%.1f", at.z), this.libertycraft$struck != 0 ? " on stand-in " + Integer.toHexString(this.libertycraft$struck) : "",
			(flags & Proto.EXPLOSION_BY_PLAYER) != 0 ? ", the player's" : "");
	}
}
