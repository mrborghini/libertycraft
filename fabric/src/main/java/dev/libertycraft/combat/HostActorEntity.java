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
import org.jspecify.annotations.Nullable;

/**
 * An invisible stand-in for one GTA IV actor (a ped, or one piece of a vehicle: {@link Proto#ACTOR_VEHICLE}),
 * so Minecraft's own combat (swords, crits, sweeps, enchantments, attack cooldown, bows, tridents,
 * explosions) can target and hit GTA IV's NPCs and cars. What it receives is collected into one hit per
 * tick and forwarded to the real thing; its own health never drops.
 *
 * <p>It is solid: the local player and mobs can't walk through it (and {@link ProxyPush} puts them back
 * out when the ped or car moves into them). The server leaves players out of that: it would see the
 * stand-in a tick late and take the client's moves for moves into it.
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
	protected void actuallyHurt(ServerLevel level, DamageSource source, float dmg) {
		// Minecraft has applied everything (crit, sharpness, strength, cooldown, invulnerability
		// frames). Hand the result to GTA IV instead of lowering our own health.
		if (this.isInvulnerableTo(level, source) || dmg <= 0.0F) {
			return;
		}
		this.pendingDamage += dmg;
		if (source.getDirectEntity() instanceof Projectile) {
			this.pendingFlags |= Proto.HIT_PROJECTILE;
		}
		this.pendingWeapon = weaponClass(source);
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE)) {
			this.pendingFlags |= Proto.HIT_FIRE;
		}
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_EXPLOSION)) {
			this.pendingFlags |= Proto.HIT_EXPLOSION;
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

	/** Player.crit() was called on us this tick. */
	public void markCritical() {
		this.pendingFlags |= Proto.HIT_CRITICAL;
	}

	/** Which kind of GTA IV weapon impact this hit should look and sound like. */
	private static int weaponClass(DamageSource source) {
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

	/** Returns this tick's hit (damage, flags, push, weapon) and clears it; null if nothing hit us. */
	public float[] takeHit() {
		if (!this.hitThisTick) {
			return null;
		}
		float[] hit = { this.pendingDamage, (float) this.pushX, (float) this.pushZ, this.pushStrength, Float.intBitsToFloat(this.pendingFlags),
			Float.intBitsToFloat(this.pendingWeapon) };
		this.pendingDamage = 0.0F;
		this.pendingFlags = 0;
		this.pushX = this.pushZ = 0.0;
		this.pushStrength = 0.0F;
		this.hitThisTick = false;
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
		// Solid for whoever moves into it; on the server not for players (see the class comment).
		return this.isAlive() && !(other instanceof HostActorEntity) && (this.level().isClientSide() || !(other instanceof net.minecraft.world.entity.player.Player));
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
