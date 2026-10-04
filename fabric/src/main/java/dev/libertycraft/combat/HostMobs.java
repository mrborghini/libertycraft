package dev.libertycraft.combat;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import java.util.Comparator;
import java.util.List;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.entity.player.Player;
import org.jspecify.annotations.Nullable;

/**
 * GTA IV's peds fight back against Minecraft's mobs, Minecraft's side.
 *
 * <p>Every 5 ticks the hostile mobs near the player go to GTA IV ({@link Proto#EV_MOB}: where each
 * one is, how big, and which ped it is after), so the host can have the ped (and police nearby) shoot
 * at it or run, and can test GTA IV's bullets against the mobs' boxes. A bullet that hits one comes
 * back as {@link Proto#IN_MOB_HIT} and hurts the mob here as a shot from that ped's stand-in (so the
 * mob turns on the shooter, HurtByTargetGoal) or from the player (whose kill it is), scaled so a few
 * pistol shots kill a zombie (GTA IV's damage / {@code kMobDamageScale}).
 */
public final class HostMobs {
	/** Ticks between two reports. */
	private static final int EVERY = 5;
	/** Event ring entries the reports always leave free for hits, blasts and the rest. */
	private static final int RESERVE = 256;
	private static int hitLogs;

	private HostMobs() {
	}

	/** Server thread, every tick (HostCombat): reports the hostile mobs around {@code player}. */
	static void report(ServerLevel level, ServerPlayer player) {
		if (level.getGameTime() % EVERY != 0 || !Link.active()) {
			return;
		}
		double r = Proto.MOB_RANGE;
		List<Mob> mobs = level.getEntitiesOfClass(Mob.class, player.getBoundingBox().inflate(r), m -> m instanceof Enemy && m.isAlive() && !m.isRemoved());
		if (mobs.isEmpty()) {
			return;
		}
		mobs.sort(Comparator.comparingDouble(player::distanceToSqr));
		int sent = 0;
		for (Mob mob : mobs) {
			if (sent >= Proto.MAX_MOBS || mob.distanceToSqr(player) > r * r) {
				break;
			}
			LivingEntity target = mob.getTarget();
			int formId = target instanceof HostActorEntity ped && !ped.isRemoved() ? ped.formId() : 0;
			int width = Math.clamp(Math.round(mob.getBbWidth() * 100.0F), 1, 0xFFFF);
			int flags = width | (target instanceof Player ? Proto.MOB_AFTER_PLAYER : 0);
			if (!Link.pushEventLeaving(RESERVE, Proto.EV_MOB, formId, (float) mob.getX(), (float) mob.getY(), (float) mob.getZ(), mob.getBbHeight(), mob.getId(), flags)) {
				break;
			}
			sent++;
		}
	}

	/**
	 * Server thread: GTA IV's bullet hit the mob with entity id {@code mobId} for {@code damage}
	 * (Minecraft's), fired by the ped with actor formId {@code shooter} (0: not known,
	 * {@link Proto#MOB_HIT_BY_PLAYER}: the player) with GTA IV weapon {@code weapon}.
	 */
	public static void bulletHit(ServerPlayer player, int mobId, int shooter, float damage, int weapon) {
		ServerLevel level = player.level();
		Entity e = level.getEntity(mobId);
		if (!(e instanceof LivingEntity mob) || !mob.isAlive() || damage <= 0.0F || mob.distanceToSqr(player) > Proto.MOB_RANGE * Proto.MOB_RANGE * 1.5) {
			return;
		}
		HostActorEntity ped = shooter != 0 && shooter != Proto.MOB_HIT_BY_PLAYER ? HostCombat.proxy(shooter) : null;
		DamageSource source = source(level, player, shooter, ped);
		float before = mob.getHealth();
		mob.setInvulnerableTime(0); // every bullet is a hit of its own (a pistol fires faster than Minecraft's half-second hurt cooldown)
		boolean hurt = mob.hurtServer(level, source, damage);
		if (hitLogs++ < 200) {
			LibertyCraft.LOG.info("[LibertyCraft] GTA IV bullet (weapon {}) from {} hit {} for {}: health {} -> {}{}", weapon,
				shooter == Proto.MOB_HIT_BY_PLAYER ? "the player" : ped != null ? "ped " + Integer.toHexString(shooter) : "someone",
				mob.getType().toShortString(), String.format("%.1f", damage), String.format("%.1f", before), String.format("%.1f", mob.getHealth()),
				hurt ? mob.isDeadOrDying() ? ", killed" : "" : " (not hurt)");
		}
	}

	private static DamageSource source(ServerLevel level, ServerPlayer player, int shooter, @Nullable HostActorEntity ped) {
		var sources = level.damageSources();
		if (shooter == Proto.MOB_HIT_BY_PLAYER) {
			return sources.mobProjectile(player, player); // the player's own kill
		}
		if (ped != null) {
			return sources.mobProjectile(ped, ped); // shot by the ped: the mob turns on it
		}
		var type = level.registryAccess().lookupOrThrow(Registries.DAMAGE_TYPE).get(HostCombat.GTA_DAMAGE);
		return type.isPresent() ? new DamageSource(type.get()) : sources.generic();
	}
}
