package dev.libertycraft.combat;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Proto;
import dev.libertycraft.link.Link;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageSources;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MobCategory;
import org.jspecify.annotations.Nullable;

/**
 * Combat between the Minecraft player and GTA IV actors, server side.
 *
 * <p>Every GTA IV actor near the player gets an invisible {@link HostActorEntity} at its exact
 * position. Minecraft weapons hit those like any mob; the resulting damage is sent to GTA IV, which
 * applies it to the real actor (scaled by level) and makes it fight back. GTA IV's hits on the player
 * come back as Minecraft damage from the attacker's stand-in, so armor, shields, knockback, hurt
 * sounds and death all work the Minecraft way.
 *
 * <p>GTA IV's vehicles come as a few records along their length ({@link Proto#ACTOR_VEHICLE}); hits on
 * any of a vehicle's pieces in a tick go to GTA IV as one hit on the piece that took the most. Every
 * stand-in is solid ({@link ProxyPush}).
 */
public final class HostCombat {
	public static final ResourceKey<EntityType<?>> HOST_ACTOR_KEY =
		ResourceKey.create(Registries.ENTITY_TYPE, Identifier.fromNamespaceAndPath(LibertyCraft.MOD_ID, "host_actor"));
	public static final EntityType<HostActorEntity> HOST_ACTOR = Registry.register(
		BuiltInRegistries.ENTITY_TYPE,
		HOST_ACTOR_KEY,
		EntityType.Builder.<HostActorEntity>of(HostActorEntity::new, MobCategory.MISC)
			.sized(0.6F, 1.8F)
			.noSave()
			.noSummon()
			.noLootTable()
			.clientTrackingRange(10)
			.updateInterval(1)
			.build(HOST_ACTOR_KEY)
	);

	/** GTA IV damage is divided by this for Minecraft (a 15-damage bandit swing = 3 = 1.5 hearts). */
	public static final float HOST_TO_MC_DAMAGE = 5.0F;

	private static final Map<Integer, HostActorEntity> PROXIES = new HashMap<>();
	private static final List<Link.Actor> ACTORS = new ArrayList<>();

	private HostCombat() {
	}

	public static void init() {
		FabricDefaultAttributeRegistry.register(HOST_ACTOR, LivingEntity.createLivingAttributes());
		ServerTickEvents.END_SERVER_TICK.register(HostCombat::serverTick);
	}

	public static @Nullable HostActorEntity proxy(int formId) {
		return PROXIES.get(formId);
	}

	private static void serverTick(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (!Link.active() || players.isEmpty()) {
			removeAll();
			return;
		}
		ServerLevel level = players.getFirst().level();
		for (ServerPlayer player : players) {
			pickUpNearby(player);
		}
		if (Link.readActors(ACTORS)) {
			sync(level);
		}
		// Hits land during the tick (melee, sweeps, arrows, fire); send one combined hit per actor, and
		// one per vehicle for blows (a sweep or a blast catches several of its pieces at once). Each
		// projectile on a vehicle goes on its own: they can strike different windows.
		VEHICLE_HITS.clear();
		for (HostActorEntity proxy : PROXIES.values()) {
			float[] hit = proxy.takeHit();
			if (hit == null || !(hit[0] > 0.0F || hit[3] > 0.0F)) {
				continue;
			}
			if (proxy.isHostVehicle() && (Float.floatToRawIntBits(hit[4]) & Proto.HIT_PROJECTILE) == 0) {
				int vehicle = proxy.formId() & ~Proto.ACTOR_VEHICLE_PIECE_MASK;
				VehicleHit merged = VEHICLE_HITS.get(vehicle);
				if (merged == null) {
					VEHICLE_HITS.put(vehicle, new VehicleHit(proxy, hit));
				} else {
					merged.add(proxy, hit);
				}
				continue;
			}
			sendHit(proxy, hit);
		}
		for (VehicleHit v : VEHICLE_HITS.values()) {
			sendHit(v.piece, v.hit);
		}
	}

	private static void sendHit(HostActorEntity proxy, float[] hit) {
		if (hit[6] > 0.0F) {
			// Where it landed, right before the hit itself (kEvHitPoint).
			Link.pushEvent(Proto.EV_HIT_POINT, proxy.formId(), hit[7], hit[8], hit[9], hit[10], Float.floatToRawIntBits(hit[11]));
		}
		Link.pushEvent(Proto.EV_HIT_ACTOR, proxy.formId(), hit[0], hit[1], hit[2], hit[3], Float.floatToRawIntBits(hit[4]), Float.floatToRawIntBits(hit[5]));
		if (hit[6] > 0.0F) {
			LibertyCraft.LOG.info("[LibertyCraft] hit {} {} for {} (knockback {}) at {} {} {} along yaw {} pitch {}{}", proxy.getName().getString(),
				Integer.toHexString(proxy.formId()), hit[0], hit[3], hit[7], hit[8], hit[9], hit[10], hit[11],
				(Float.floatToRawIntBits(hit[4]) & Proto.HIT_PROJECTILE) != 0 ? " (projectile)" : "");
		} else {
			LibertyCraft.LOG.info("[LibertyCraft] hit {} for {} (knockback {})", proxy.getName().getString(), hit[0], hit[3]);
		}
	}

	private static final Map<Integer, VehicleHit> VEHICLE_HITS = new HashMap<>();

	/** A tick's hits on one vehicle's pieces: the biggest one's damage and piece, every flag, the strongest push. */
	private static final class VehicleHit {
		HostActorEntity piece;
		final float[] hit;

		VehicleHit(HostActorEntity piece, float[] hit) {
			this.piece = piece;
			this.hit = hit.clone();
		}

		void add(HostActorEntity other, float[] h) {
			int flags = Float.floatToRawIntBits(this.hit[4]) | Float.floatToRawIntBits(h[4]);
			if (h[0] > this.hit[0]) {
				this.piece = other;
				this.hit[0] = h[0];
				this.hit[5] = h[5];
				System.arraycopy(h, 6, this.hit, 6, 6); // its hit point
			}
			if (h[3] > this.hit[3]) {
				this.hit[1] = h[1];
				this.hit[2] = h[2];
				this.hit[3] = h[3];
			}
			this.hit[4] = Float.intBitsToFloat(flags);
		}
	}

	private static void sync(ServerLevel level) {
		Map<Integer, Link.Actor> live = new HashMap<>();
		for (Link.Actor a : ACTORS) {
			// A dead ped is a ragdoll on the ground (walked over, not hit); a wreck still stands there.
			if (!a.dead() || (a.flags() & Proto.ACTOR_VEHICLE) != 0) {
				live.put(a.formId(), a);
			}
		}
		for (Iterator<Map.Entry<Integer, HostActorEntity>> it = PROXIES.entrySet().iterator(); it.hasNext(); ) {
			Map.Entry<Integer, HostActorEntity> e = it.next();
			HostActorEntity proxy = e.getValue();
			if (!live.containsKey(e.getKey()) || proxy.isRemoved() || proxy.level() != level) {
				proxy.discard();
				it.remove();
			}
		}
		int before = PROXIES.size();
		for (Link.Actor a : live.values()) {
			HostActorEntity proxy = PROXIES.get(a.formId());
			if (proxy == null) {
				proxy = new HostActorEntity(HOST_ACTOR, level);
				proxy.setFormId(a.formId());
				proxy.setHostFlags(a.flags());
				proxy.setSize(a.width(), a.height());
				proxy.snapTo(a.x(), a.y(), a.z(), a.yaw(), 0.0F);
				if (!a.name().isEmpty()) {
					proxy.setCustomName(Component.literal(a.name()));
				}
				if (!level.addFreshEntity(proxy)) {
					continue;
				}
				PROXIES.put(a.formId(), proxy);
				continue;
			}
			double ox = proxy.getX(), oz = proxy.getZ();
			proxy.setHostFlags(a.flags());
			proxy.setSize(a.width(), a.height());
			proxy.setPos(a.x(), a.y(), a.z());
			proxy.setYRot(a.yaw());
			proxy.setYHeadRot(a.yaw());
			stepOnTriggers(level, proxy);
			ProxyPush.shoveMobs(level, proxy, a.x() - ox, a.z() - oz);
		}
		if (PROXIES.size() != before && (PROXIES.size() % 5 == 0 || PROXIES.size() < 5)) {
			LibertyCraft.LOG.info("[LibertyCraft] {} GTA IV actors mirrored as hittable stand-ins", PROXIES.size());
		}
	}

	/**
	 * GTA IV's NPCs press pressure plates and trip tripwires. Their stand-ins are placed, not moved
	 * (no physics), so Minecraft never checks what they step into; do it for those blocks here.
	 */
	private static void stepOnTriggers(ServerLevel level, HostActorEntity proxy) {
		var box = proxy.getBoundingBox().deflate(1.0E-5);
		var from = net.minecraft.core.BlockPos.containing(box.minX, box.minY, box.minZ);
		var to = net.minecraft.core.BlockPos.containing(box.maxX, box.maxY, box.maxZ);
		for (var pos : net.minecraft.core.BlockPos.betweenClosed(from, to)) {
			var state = level.getBlockState(pos);
			if (state.getBlock() instanceof net.minecraft.world.level.block.BasePressurePlateBlock
				|| state.getBlock() instanceof net.minecraft.world.level.block.TripWireBlock) {
				state.entityInside(level, pos, proxy, net.minecraft.world.entity.InsideBlockEffectApplier.NOOP, true);
			}
		}
	}

	/**
	 * Items and stuck arrows on GTA IV ground rest on its collision voxels, which on steep or rough
	 * terrain can sit a little off from where the player (on GTA IV's exact triangles) stands.
	 * Touch them over a slightly bigger area than vanilla's so walking over them picks them up.
	 * playerTouch applies all of Minecraft's own rules (pickup delay, owner, inventory space).
	 */
	private static void pickUpNearby(ServerPlayer player) {
		if (!player.isAlive() || player.isSpectator()) {
			return;
		}
		for (Entity entity : player.level().getEntities(player, player.getBoundingBox().inflate(1.25, 1.0, 1.25))) {
			if (!entity.isRemoved() && (entity instanceof net.minecraft.world.entity.item.ItemEntity
				|| entity instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow)) {
				entity.playerTouch(player);
			}
		}
	}

	private static void removeAll() {
		if (PROXIES.isEmpty()) {
			return;
		}
		PROXIES.values().forEach(Entity::discard);
		PROXIES.clear();
	}

	/**
	 * GTA IV hit the player. Runs on the server thread. {@code kind} is a Proto.HURT_* value and
	 * {@code hostDamage} is what GTA IV would have taken off the player's health.
	 */
	public static void hurtPlayer(ServerPlayer player, int kind, float hostDamage, int attackerFormId, int flags) {
		if (!player.isAlive() || hostDamage <= 0.0F) {
			return;
		}
		ServerLevel level = player.level();
		HostActorEntity attacker = PROXIES.get(attackerFormId);
		if (attacker != null && attacker.distanceToSqr(player) > 24.0 * 24.0) {
			attacker = null; // a guest's own NPC with the same form id as one of the host's
		}
		DamageSources sources = level.damageSources();
		DamageSource source = switch (kind) {
			case Proto.HURT_MELEE -> attacker != null ? sources.mobAttack(attacker) : sources.generic();
			case Proto.HURT_PROJECTILE -> attacker != null ? sources.mobProjectile(attacker, attacker) : sources.generic();
			case Proto.HURT_MAGIC -> attacker != null ? sources.indirectMagic(attacker, attacker) : sources.magic();
			default -> sources.generic();
		};
		float damage = hostDamage / HOST_TO_MC_DAMAGE;
		float healthBefore = player.getHealth();
		// (SkyCraft fed Skyrim's Block / Armor skills from here; GTA IV has no skill XP.)
		boolean hurt = player.hurtServer(level, source, damage);
		LibertyCraft.LOG.info("[LibertyCraft] GTA IV hit the player for {} ({} Minecraft): health {} -> {}{}", hostDamage, damage, healthBefore, player.getHealth(),
			hurt ? "" : " (blocked/immune)");
		if (hurt && attacker != null && (flags & Proto.HURT_POWER_ATTACK) != 0 && !player.isBlocking()) {
			// Power attacks shove harder, like a sprint hit does in Minecraft.
			player.knockback(0.5, attacker.getX() - player.getX(), attacker.getZ() - player.getZ(), source, damage);
		}
	}

	/** Form id of the GTA IV actor behind a damage source, or 0. */
	public static int attackerFormId(DamageSource source) {
		return source.getEntity() instanceof HostActorEntity proxy ? proxy.formId() : 0;
	}
}
