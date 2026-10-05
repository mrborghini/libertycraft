package dev.libertycraft.combat;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import dev.libertycraft.world.HostDrive;
import net.fabricmc.fabric.api.entity.event.v1.ServerLivingEntityEvents;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.tags.DamageTypeTags;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.phys.Vec3;

/**
 * Mobs hit the player while GTA IV drives him: on foot (Niko mode, getting back up) GTA IV's player ped
 * takes the hit (Proto.EV_MOB_HIT_PLAYER: Niko loses health and is knocked over), seated in a
 * vehicle the vehicle does, as follows.
 *
 * <p>Mobs attack the player's own vehicle while the player sits in it. That vehicle has no stand-in (the
 * player's mount sits inside it: {@link Proto#ACTOR_PLAYER_VEHICLE}), so hostile mobs go for the player
 * as usual; a mob's blow or arrow that reaches the seated player hits the vehicle instead, through the
 * same path as hits on any vehicle's stand-in (body and engine damage, windows, fire; an arrow through
 * a window hits whoever sits behind it, in GTA IV), flagged as a mob's (no crime). A mob's blast is
 * GTA IV's own explosion at that spot already (ServerExplosionMixin): it reaches the vehicle there.
 * Runs before HostDrive's rule that the player takes no damage while GTA IV drives them.
 */
public final class PlayerVehicleHits {
	private static final Identifier EARLY = Identifier.fromNamespaceAndPath(LibertyCraft.MOD_ID, "player_vehicle_hits");
	private static int logs;

	private PlayerVehicleHits() {
	}

	public static void init() {
		ServerLivingEntityEvents.ALLOW_DAMAGE.addPhaseOrdering(EARLY, net.fabricmc.fabric.api.event.Event.DEFAULT_PHASE);
		ServerLivingEntityEvents.ALLOW_DAMAGE.register(EARLY, (entity, source, amount) -> {
			if (!(entity instanceof ServerPlayer player) || !(source.getEntity() instanceof Mob) || !Link.active()) {
				return true;
			}
			if (HostDrive.isMount(player.getVehicle())) {
				if (!source.is(DamageTypeTags.IS_EXPLOSION)) {
					hitVehicle(player, source, amount);
				}
				return false; // the vehicle takes it, not the player in it
			}
			if (HostDrive.drivenByHost(player)) {
				// GTA IV drives him on foot (Niko mode, getting back up): GTA IV's player takes it. A mob's blast
				// reaches him as GTA IV's own explosion already (kEvExplosion).
				if (!source.is(DamageTypeTags.IS_EXPLOSION)) {
					hitNiko(player, source, amount);
				}
				return false;
			}
			return true;
		});
	}

	/** A mob's blow or projectile on the player GTA IV drives on foot: GTA IV's player ped takes it (Proto.EV_MOB_HIT_PLAYER). */
	private static void hitNiko(ServerPlayer player, DamageSource source, float amount) {
		if (amount <= 0.0F || player.isCreative() || player.isSpectator()) {
			return;
		}
		Vec3 from = source.getSourcePosition() != null ? source.getSourcePosition() : player.position();
		double dx = player.getX() - from.x, dz = player.getZ() - from.z, len = Math.sqrt(dx * dx + dz * dz);
		float px = len > 1.0E-6 ? (float) (dx / len) : 0.0F, pz = len > 1.0E-6 ? (float) (dz / len) : 0.0F;
		boolean projectile = source.getDirectEntity() instanceof Projectile;
		// Minecraft's knockback: 0.4 for any hit (LivingEntity.hurtServer), and a blow adds half the mob's attack
		// knockback (Mob.doHurtTarget's causeExtraKnockback: a ravager's 1.5). GTA IV knocks him over by it as
		// a Minecraft hit knocks a ped (RagdollOnHit, HitForce).
		float strength = 0.4F;
		if (!projectile && source.getEntity() instanceof Mob mob) {
			strength += (float) (0.5 * mob.getAttributeValue(net.minecraft.world.entity.ai.attributes.Attributes.ATTACK_KNOCKBACK));
		}
		Link.pushEvent(Proto.EV_MOB_HIT_PLAYER, 0, amount, px, pz, strength, projectile ? Proto.HIT_PROJECTILE : 0, HostActorEntity.weaponClass(source));
		if (logs++ < 100) {
			LibertyCraft.LOG.info("[LibertyCraft] {} hit the player GTA IV drives for {}{}: GTA IV's player takes it", source.getEntity().getType().toShortString(),
				String.format("%.1f", amount), projectile ? " (projectile)" : "");
		}
	}

	private static void hitVehicle(ServerPlayer player, DamageSource source, float amount) {
		Vec3 from = source.getSourcePosition() != null ? source.getSourcePosition() : player.position();
		int piece = VehicleRunOver.nearestPlayerPiece(from.x, from.z);
		if (piece == 0 || amount <= 0.0F) {
			return;
		}
		double dx = player.getX() - from.x, dz = player.getZ() - from.z, len = Math.sqrt(dx * dx + dz * dz);
		float px = len > 1.0E-6 ? (float) (dx / len) : 0.0F, pz = len > 1.0E-6 ? (float) (dz / len) : 0.0F;
		int flags = Proto.HIT_BY_MOB;
		if (source.getDirectEntity() instanceof Projectile projectile) {
			flags |= Proto.HIT_PROJECTILE;
			Vec3 v = projectile.getDeltaMovement();
			if (v.lengthSqr() > 1.0E-8) {
				// Where it struck and which way it flew (kEvHitPoint): the host follows it into the car (a window, who sits behind it).
				Vec3 d = v.normalize();
				float yaw = (float) Math.toDegrees(Math.atan2(-d.x, d.z)), pitch = (float) Math.toDegrees(-Math.asin(Math.clamp(d.y, -1.0, 1.0)));
				Link.pushEvent(Proto.EV_HIT_POINT, piece, (float) projectile.getX(), (float) projectile.getY(), (float) projectile.getZ(), yaw, Float.floatToRawIntBits(pitch));
			}
		}
		if (source.is(DamageTypeTags.IS_FIRE)) {
			flags |= Proto.HIT_FIRE;
		}
		Link.pushEvent(Proto.EV_HIT_ACTOR, piece, amount, px, pz, 0.0F, flags, HostActorEntity.weaponClass(source));
		if (logs++ < 100) {
			Entity attacker = source.getEntity();
			LibertyCraft.LOG.info("[LibertyCraft] {} hit the player's vehicle (piece {}) for {}{}", attacker.getType().toShortString(), Integer.toHexString(piece),
				String.format("%.1f", amount), (flags & Proto.HIT_PROJECTILE) != 0 ? " (projectile)" : "");
		}
	}
}
