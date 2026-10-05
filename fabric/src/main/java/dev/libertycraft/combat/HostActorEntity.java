package dev.libertycraft.combat;

import dev.libertycraft.link.Proto;
import net.minecraft.network.syncher.EntityDataAccessor;
import net.minecraft.network.syncher.EntityDataSerializers;
import net.minecraft.network.syncher.SynchedEntityData;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.sounds.SoundEvent;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityDimensions;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Pose;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * An invisible stand-in for one GTA IV actor (a ped, or one piece of a vehicle: {@link Proto#ACTOR_VEHICLE}),
 * so Minecraft's own combat (swords, crits, sweeps, enchantments, attack cooldown, bows, tridents,
 * explosions) can target and hit GTA IV's NPCs and cars. What it receives is collected into one hit per
 * tick and forwarded to the real thing; its own health never drops.
 *
 * <p>A vehicle's pieces are solid: the local player and mobs can't walk through them (and
 * {@link ProxyPush} puts them back out when the car moves into them). The server leaves players out of
 * that: it would see the stand-in a tick late and take the client's moves for moves into it. A ped is
 * solid for mobs only: the player pushes it out of the way (ProxyPushClient sends kEvBump, GTA IV moves,
 * trips or knocks down the ped by the player's speed). A dead ped's corpse ({@link #isHostCorpse}) is a
 * low stand-in over the body, never solid: hits and the player push it around.
 */
public class HostActorEntity extends LivingEntity {
	private static final EntityDataAccessor<Integer> FORM_ID = SynchedEntityData.defineId(HostActorEntity.class, EntityDataSerializers.INT);
	private static final EntityDataAccessor<Float> WIDTH = SynchedEntityData.defineId(HostActorEntity.class, EntityDataSerializers.FLOAT);
	private static final EntityDataAccessor<Float> HEIGHT = SynchedEntityData.defineId(HostActorEntity.class, EntityDataSerializers.FLOAT);
	private static final EntityDataAccessor<Integer> FLAGS = SynchedEntityData.defineId(HostActorEntity.class, EntityDataSerializers.INT);

	// This tick's hit, flushed to GTA IV by HostCombat after all attacks for the tick have landed
	// (Player.attack adds its sprint/enchantment knockback after hurtServer returns).
	private float pendingDamage;
	private int pendingFlags;
	private int pendingWeapon;
	private double pushX, pushZ;
	private float pushStrength;
	private boolean hitThisTick;
	// Who dealt this tick's damage: the player (or anything else), or only Minecraft's mobs (Proto.HIT_BY_MOB).
	private boolean hitByPlayer, hitByMob;
	private static final float MOB_PLAIN_KNOCKBACK = 0.45F;
	// Vehicles: where this tick's biggest hit landed and the way it travelled (kEvHitPoint).
	private @Nullable Vec3 hitAt, hitDir;
	private float hitAtDamage;

	public HostActorEntity(EntityType<? extends HostActorEntity> type, Level level) {
		super(type, level);
		this.setNoGravity(true);
		this.noPhysics = true;
		this.setInvisible(true);
		this.setSilent(true);
	}

	public int formId() {
		return this.entityData.get(FORM_ID);
	}

	public void setFormId(int formId) {
		this.entityData.set(FORM_ID, formId);
	}

	/** The host's ActorFlags (Proto.ACTOR_*). */
	public int hostFlags() {
		return this.entityData.get(FLAGS);
	}

	public void setHostFlags(int flags) {
		if (this.entityData.get(FLAGS) != flags) {
			this.entityData.set(FLAGS, flags);
		}
	}

	/** One piece of a GTA IV vehicle rather than a ped. */
	public boolean isHostVehicle() {
		return (this.hostFlags() & Proto.ACTOR_VEHICLE) != 0;
	}

	/**
	 * A mission character (a ped a GTA IV mission script owns) or a vehicle one sits in (Proto.ACTOR_MISSION):
	 * Minecraft's mobs leave it alone (PedTargets, {@link #canBeSeenAsEnemy}) and their hits don't reach it.
	 */
	public boolean isMission() {
		return (this.hostFlags() & Proto.ACTOR_MISSION) != 0;
	}

	/** A dead ped's body on the ground (not a wreck): hit and pushed, never solid. */
	public boolean isHostCorpse() {
		return (this.hostFlags() & (Proto.ACTOR_DEAD | Proto.ACTOR_VEHICLE)) == Proto.ACTOR_DEAD;
	}

	@Override
	protected void defineSynchedData(SynchedEntityData.Builder builder) {
		super.defineSynchedData(builder);
		builder.define(FORM_ID, 0);
		builder.define(WIDTH, 0.6F);
		builder.define(HEIGHT, 1.8F);
		builder.define(FLAGS, 0);
	}

	public void setSize(float width, float height) {
		if (Math.abs(this.entityData.get(WIDTH) - width) > 0.01F || Math.abs(this.entityData.get(HEIGHT) - height) > 0.01F) {
			this.entityData.set(WIDTH, width);
			this.entityData.set(HEIGHT, height);
			this.refreshDimensions();
		}
	}

	@Override
	public void onSyncedDataUpdated(EntityDataAccessor<?> accessor) {
		super.onSyncedDataUpdated(accessor);
		if (WIDTH.equals(accessor) || HEIGHT.equals(accessor)) {
			this.refreshDimensions();
		}
	}

	@Override
	protected EntityDimensions getDefaultDimensions(Pose pose) {
		return EntityDimensions.scalable(this.entityData.get(WIDTH), this.entityData.get(HEIGHT));
	}

	@Override
	public boolean hurtServer(ServerLevel level, DamageSource source, float amount) {
		// A firework rocket's burst is GTA IV's own rocket blast there (FireworkRocketMixin), which hurts,
		// knocks over and wrecks the real peds and vehicles itself: Minecraft's firework damage (and its
		// push) on the stand-ins would hit them a second time.
		if (source.is(net.minecraft.world.damagesource.DamageTypes.FIREWORKS) && dev.libertycraft.link.Link.active()) {
			return false;
		}
		// A mob's blow, arrow or blast doesn't reach a mission's character (a creeper failed a date mission).
		if (this.isMission() && source.getEntity() instanceof net.minecraft.world.entity.Mob) {
			return false;
		}
		return super.hurtServer(level, source, amount);
	}

	@Override
	protected void actuallyHurt(ServerLevel level, DamageSource source, float dmg) {
		// Minecraft has applied everything (crit, sharpness, strength, cooldown, invulnerability
		// frames). Hand the result to GTA IV instead of lowering our own health.
		if (this.isInvulnerableTo(level, source) || dmg <= 0.0F) {
			return;
		}
		// Only what something does to it (a hit, an arrow, a blast) is GTA IV's business: the stand-in
		// sits in Minecraft's world, so its water (drowning: a car in a pool wrecked by its pieces
		// "drowning"), fire, lava, walls and falls would hurt the real ped or car twice or for nothing.
		// GTA IV handles its own world, and LibertyCraft's Hazards (host) Minecraft's fire, lava and water.
		if (source.getEntity() == null && source.getDirectEntity() == null && !source.is(net.minecraft.tags.DamageTypeTags.IS_EXPLOSION)) {
			return;
		}
		this.pendingDamage += dmg;
		if (source.getEntity() instanceof net.minecraft.world.entity.Mob) {
			this.hitByMob = true; // a zombie's blow, a skeleton's arrow, a creeper's blast (PedTargets)
		} else {
			this.hitByPlayer = true;
		}
		if (source.getDirectEntity() instanceof Projectile) {
			this.pendingFlags |= Proto.HIT_PROJECTILE;
		}
		if (PlayerVehicleHits.launches(source)) {
			this.pendingFlags |= Proto.HIT_LAUNCH; // an iron golem's blow throws the ped up
		}
		this.pendingWeapon = weaponClass(source);
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE)) {
			this.pendingFlags |= Proto.HIT_FIRE;
		}
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_EXPLOSION)) {
			this.pendingFlags |= Proto.HIT_EXPLOSION;
		}
		if (this.isHostVehicle() && dmg > this.hitAtDamage) {
			this.noteHitPoint(source, dmg);
		}
		this.hitThisTick = true;
		this.getCombatTracker().recordDamage(source, dmg);
	}

	@Override
	public void knockback(double power, double xd, double zd, DamageSource source, float damage, boolean comesFromEffect) {
		// GTA IV owns this actor's position. Remember the strongest push for GTA IV's stagger:
		// Minecraft pushes towards -(xd, zd) ((xd, zd) points from the victim to the attacker, see
		// LivingEntity.dealDefaultKnockback and causeExtraKnockback), so the push sent is the way the
		// victim should fly, away from the attacker.
		double len = Math.sqrt(xd * xd + zd * zd);
		if (len > 1e-6 && power > this.pushStrength) {
			this.pushStrength = (float) power;
			this.pushX = -xd / len;
			this.pushZ = -zd / len;
		}
		this.hitThisTick = true;
	}

	/**
	 * Where a hit on a vehicle piece landed, for GTA IV to work out what it struck (body or glass): a
	 * projectile is at the point it hit (AbstractArrow moves it there before it hurts us) and flies
	 * along its motion; a blow follows the attacker's look from the eye into this box (or, for a
	 * sweep that caught this box off the look line, toward its nearest point).
	 */
	private void noteHitPoint(DamageSource source, float dmg) {
		var box = this.getBoundingBox();
		Vec3 at = null, dir = null;
		if (source.getDirectEntity() instanceof Projectile projectile) {
			Vec3 v = projectile.getDeltaMovement();
			if (v.lengthSqr() > 1.0E-8) {
				at = projectile.position();
				dir = v.normalize();
			}
		} else if (source.getEntity() instanceof LivingEntity attacker && source.getDirectEntity() == attacker) {
			Vec3 eye = attacker.getEyePosition();
			Vec3 look = attacker.getViewVector(1.0F);
			at = box.inflate(0.01).clip(eye, eye.add(look.scale(8.0))).orElse(null);
			dir = look;
			if (at == null) {
				at = new Vec3(Math.clamp(eye.x, box.minX, box.maxX), Math.clamp(eye.y, box.minY, box.maxY), Math.clamp(eye.z, box.minZ, box.maxZ));
				Vec3 to = at.subtract(eye);
				dir = to.lengthSqr() > 1.0E-8 ? to.normalize() : look;
			}
		}
		if (at != null && dir != null) {
			this.hitAt = at;
			this.hitDir = dir;
			this.hitAtDamage = dmg;
		}
	}

	/** Player.crit() was called on us this tick. */
	public void markCritical() {
		this.pendingFlags |= Proto.HIT_CRITICAL;
	}

	/** Which kind of GTA IV weapon impact this hit should look and sound like. */
	static int weaponClass(DamageSource source) {
		if (source.getDirectEntity() instanceof net.minecraft.world.entity.projectile.arrow.ThrownTrident) {
			return Proto.WEAPON_PIERCE;
		}
		if (source.getDirectEntity() instanceof Projectile) {
			return Proto.WEAPON_ARROW;
		}
		ItemStack weapon = source.getWeaponItem();
		if (weapon == null && source.getEntity() instanceof LivingEntity attacker) {
			weapon = attacker.getMainHandItem();
		}
		if (weapon == null || weapon.isEmpty()) {
			return Proto.WEAPON_UNARMED;
		}
		if (weapon.is(ItemTags.SWORDS)) {
			return Proto.WEAPON_BLADE;
		}
		if (weapon.is(ItemTags.AXES)) {
			return Proto.WEAPON_AXE;
		}
		if (weapon.is(Items.TRIDENT)) {
			return Proto.WEAPON_PIERCE;
		}
		return Proto.WEAPON_BLUNT;
	}

	/**
	 * Returns this tick's hit and clears it; null if nothing hit us. {damage, push x, push z, push
	 * strength, flags (bits), weapon (bits), has a hit point (1/0), point x, y, z, its line's yaw, pitch}.
	 */
	public float[] takeHit() {
		if (!this.hitThisTick) {
			return null;
		}
		if (this.hitByMob && !this.hitByPlayer) {
			this.pendingFlags |= Proto.HIT_BY_MOB;
			// A mob's plain blow or arrow (knockback 0.4) doesn't knock the ped over (GTA IV would ragdoll
			// it every time, and the police could never shoot back); a heavier one (a ravager, a sprinting
			// Knockback enchantment) does, by what it has beyond that.
			this.pushStrength = Math.max(0.0F, this.pushStrength - MOB_PLAIN_KNOCKBACK);
			if ((this.pendingFlags & Proto.HIT_LAUNCH) != 0) {
				this.pushStrength = Math.max(this.pushStrength, 0.4F); // except an iron golem's: it throws the ped
			}
		}
		float[] hit = { this.pendingDamage, (float) this.pushX, (float) this.pushZ, this.pushStrength, Float.intBitsToFloat(this.pendingFlags),
			Float.intBitsToFloat(this.pendingWeapon), 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F };
		if (this.hitAt != null && this.hitDir != null) {
			hit[6] = 1.0F;
			hit[7] = (float) this.hitAt.x;
			hit[8] = (float) this.hitAt.y;
			hit[9] = (float) this.hitAt.z;
			hit[10] = (float) Math.toDegrees(Math.atan2(-this.hitDir.x, this.hitDir.z));
			hit[11] = (float) Math.toDegrees(-Math.asin(Math.clamp(this.hitDir.y, -1.0, 1.0)));
		}
		this.hitAt = this.hitDir = null;
		this.hitAtDamage = 0.0F;
		this.pendingDamage = 0.0F;
		this.pendingFlags = 0;
		this.pushX = this.pushZ = 0.0;
		this.pushStrength = 0.0F;
		this.hitThisTick = false;
		this.hitByPlayer = this.hitByMob = false;
		return hit;
	}

	@Override
	public void tick() {
		// Position and rotation come from GTA IV (HostCombat); keep hurt timers and fire ticking.
		this.baseTick();
		this.setHealth(this.getMaxHealth());
	}

	@Override
	public boolean isPushable() {
		return false;
	}

	@Override
	protected void doPush(Entity entity) {
	}

	@Override
	public boolean canBeCollidedWith(@Nullable Entity other) {
		// Vehicles are solid for whoever moves into them; on the server not for players (see the class
		// comment). Peds only for mobs: the player pushes them out of the way instead (ProxyPushClient,
		// kEvBump), and corpses are never solid.
		if (!this.isAlive() || other instanceof HostActorEntity || this.isHostCorpse()) {
			return false;
		}
		// Dropped items and experience aren't pushed around by GTA IV's peds and cars: a ped stepping
		// onto an item made it overlap a solid entity, so vanilla turned its physics off and it sank
		// through GTA's ground (which is no real block) into the void.
		if (other instanceof net.minecraft.world.entity.item.ItemEntity || other instanceof net.minecraft.world.entity.ExperienceOrb) {
			return false;
		}
		if (other instanceof net.minecraft.world.entity.player.Player) {
			return this.isHostVehicle() && this.level().isClientSide();
		}
		return true;
	}

	/**
	 * Minecraft's mobs see the ped GTA IV shows, not an invisible entity (vanilla would let them notice
	 * it only from 2 blocks away).
	 */
	@Override
	public double getVisibilityPercent(ServerLevel level, @Nullable Entity targetingEntity) {
		return 1.0;
	}

	/**
	 * Something for Minecraft's mobs to fight (PedTargets, a wither's own targeting): a living ped on foot,
	 * or a piece of a vehicle someone sits in; not a body, a wreck or an empty car.
	 */
	@Override
	public boolean canBeSeenAsEnemy() {
		int flags = this.hostFlags();
		boolean target = this.isHostVehicle() ? (flags & (Proto.ACTOR_OCCUPIED | Proto.ACTOR_DEAD)) == Proto.ACTOR_OCCUPIED : !this.isHostCorpse();
		return target && !this.isMission() && super.canBeSeenAsEnemy();
	}

	/** An occupied vehicle's piece (kActorOccupied), not a wreck. */
	public boolean isOccupiedVehicle() {
		return this.isHostVehicle() && (this.hostFlags() & (Proto.ACTOR_OCCUPIED | Proto.ACTOR_DEAD)) == Proto.ACTOR_OCCUPIED;
	}

	@Override
	public boolean shouldShowName() {
		return false;
	}

	@Override
	public boolean shouldBeSaved() {
		return false;
	}

	@Override
	protected @Nullable SoundEvent getHurtSound(DamageSource source) {
		return null; // GTA IV plays the NPC's own pain sounds
	}

	@Override
	protected @Nullable SoundEvent getDeathSound() {
		return null;
	}

	@Override
	public HumanoidArm getMainArm() {
		return HumanoidArm.RIGHT;
	}
}
