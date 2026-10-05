package dev.libertycraft.combat;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Proto;
import dev.libertycraft.world.HostDrive;
import java.util.Optional;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.network.protocol.game.ClientboundExplodePacket;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.util.random.WeightedList;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Explosion;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * GTA IV's own explosions in Minecraft (Proto.IN_GTA_EXPLOSION: cars and gas pumps blowing up, grenades,
 * rockets, molotovs). Each becomes a Minecraft explosion at the same spot that hurts and knocks back
 * Minecraft's mobs, items and player as one of Minecraft's own of that size would: GTA IV's radius is how
 * far it hurts, so the Minecraft explosion's own radius is half of it (Minecraft's hurt out to twice
 * theirs). A fire blast (a molotov, a burning car) also sets what it reaches alight. It breaks no
 * blocks and makes no sound (GTA IV's blast is the one you see and hear), and it is never sent back to
 * GTA IV (ServerExplosionMixin). GTA IV's own stand-ins are left out (GTA IV's blast hit the real peds and
 * cars), and so is the player while GTA IV drives him (Niko mode, a vehicle, getting back up): GTA IV's
 * blast hurts him then. In Minecraft mode Minecraft's explosion is the one that hurts and throws him, and
 * the host drops GTA IV's damage to the player ped (Blasts, Combat's explosion-proofing). A blast the
 * player set off (his grenade, his rocket, a car he blew up) is his kill.
 */
public final class GtaBlasts {
	private static boolean mirroring;
	private static int logs;

	private GtaBlasts() {
	}

	/** A GTA IV explosion is being made in Minecraft right now (ServerExplosionMixin leaves it alone). */
	public static boolean mirroring() {
		return mirroring;
	}

	/** Who a GTA IV blast leaves alone: the stand-ins, the player's mount, a player GTA IV drives or who just respawned. */
	static boolean spared(Entity e) {
		return e instanceof HostActorEntity || HostDrive.isMount(e)
			|| (e instanceof ServerPlayer p && (HostDrive.drivenByHost(p) || p.tickCount < HostCombat.RESPAWN_GRACE_TICKS));
	}

	private static final ExplosionDamageCalculator CALCULATOR = new ExplosionDamageCalculator() {
		@Override
		public boolean shouldDamageEntity(Explosion explosion, Entity entity) {
			return !spared(entity) && super.shouldDamageEntity(explosion, entity);
		}

		@Override
		public float getKnockbackMultiplier(Entity entity) {
			return spared(entity) ? 0.0F : super.getKnockbackMultiplier(entity);
		}
	};

	/** Server thread: one of GTA IV's explosions at (x, y, z) (MC), {@code code} as Proto.IN_GTA_EXPLOSION's. */
	public static void explode(ServerPlayer player, double x, double y, double z, int code) {
		ServerLevel level = player.level();
		Vec3 center = new Vec3(x, y, z);
		if (player.distanceToSqr(center) > 160.0 * 160.0) {
			return;
		}
		int type = code & Proto.GTA_BLAST_TYPE_MASK;
		float gtaRadius = ((code >>> Proto.GTA_BLAST_RADIUS_SHIFT) & 0x7F) * 0.5F;
		boolean byPlayer = (code & Proto.GTA_BLAST_BY_PLAYER) != 0, fire = (code & Proto.GTA_BLAST_FIRE) != 0;
		float radius = Math.clamp(gtaRadius * 0.5F, 0.5F, 10.0F);
		DamageSource source = level.damageSources().explosion(byPlayer ? player : null, byPlayer ? player : null);
		ServerExplosion explosion = new ServerExplosion(level, null, source, CALCULATOR, center, radius, false, Explosion.BlockInteraction.KEEP);
		int burning = 0;
		// (What it does, for the log: the health of everything within its reach before and after.)
		var reach = new AABB(center, center).inflate(radius * 2.0 + 1.0);
		var before = new java.util.IdentityHashMap<LivingEntity, Float>();
		for (LivingEntity e : level.getEntitiesOfClass(LivingEntity.class, reach, e -> e.isAlive() && !(e instanceof HostActorEntity))) {
			before.put(e, e.getHealth());
		}
		mirroring = true;
		try {
			explosion.explode();
		} finally {
			mirroring = false;
		}
		StringBuilder hurt = new StringBuilder();
		before.forEach((e, h) -> {
			if (e.getHealth() < h || !e.isAlive()) {
				hurt.append(hurt.isEmpty() ? "" : ", ").append(e.getType().toShortString()).append(' ').append(String.format("%.0f", h)).append("->")
					.append(e.isAlive() ? String.format("%.0f", e.getHealth()) : "dead");
			}
		});
		if (fire) {
			for (LivingEntity e : level.getEntitiesOfClass(LivingEntity.class, new AABB(center, center).inflate(gtaRadius), e -> e.isAlive() && !spared(e))) {
				if (e.distanceToSqr(center) <= gtaRadius * gtaRadius && !(e instanceof Player p && p.isCreative())) {
					e.igniteForSeconds(fire && gtaRadius < 6.0F ? 6.0F : 4.0F);
					burning++;
				}
			}
		}
		// The players it threw: their own clients move them (no sound or particles: GTA IV's blast is the one).
		explosion.getHitPlayers().forEach((p, knockback) -> {
			if (p instanceof ServerPlayer sp) {
				sp.connection.send(new ClientboundExplodePacket(center, radius, 0, Optional.of(knockback), ParticleTypes.SMOKE, SoundEvents.GENERIC_EXPLODE,
					WeightedList.of(), false));
			}
		});
		if (logs++ < 300) {
			LibertyCraft.LOG.info("[LibertyCraft] GTA IV explosion type {} (GTA radius {} m{}{}) at {} {} {}: Minecraft explosion of radius {}{}{}; hurt: {}", type,
				gtaRadius, byPlayer ? ", the player's" : "", fire ? ", fire" : "", String.format("%.1f", x), String.format("%.1f", y), String.format("%.1f", z),
				String.format("%.1f", radius), explosion.getHitPlayers().isEmpty() ? "" : ", threw the player", burning > 0 ? ", " + burning + " set alight" : "",
				hurt.isEmpty() ? "nothing" : hurt);
		}
	}
}
