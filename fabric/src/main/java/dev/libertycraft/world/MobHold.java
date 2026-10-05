package dev.libertycraft.world;

import dev.libertycraft.LibertyCraft;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.navigation.FlyingPathNavigation;
import net.minecraft.world.level.Level;

/**
 * Mobs never fall into what GTA IV hasn't described. The mirror world is a void under GTA IV's collision
 * (HostCollision), which GTA IV streams in regions around the player: three regions below the player's
 * feet and two above, out to where it forgets them. A mob over a column it hasn't described yet (far
 * below a player on a roof, the sea bed under deep water, a column just after a world change cleared the
 * collision) would fall or sink through the void and die there. Such a mob is held where it is, as if
 * frozen, until GTA IV has sent the regions under it: then it lands on whatever is there.
 *
 * <p>Only mobs that fall: not flying ones (no gravity, a flying navigation; phantoms and ghasts fly with
 * their own travel). A mob standing on GTA IV's ground or on a Minecraft block is never held.
 */
public final class MobHold {
	/** How far below the feet the column must be described (a fall covers at most about 4 blocks a tick). */
	static final int LOOK_DOWN = 5;
	private static int logs;
	private static long held;

	private MobHold() {
	}

	/** True if this mob must stay where it is this tick (LivingEntity.travel). Server side. */
	public static boolean holds(Mob mob) {
		Level level = mob.level();
		if (!HostPath.applies(level) || mob.isNoGravity() || mob.isPassenger() || mob.getNavigation() instanceof FlyingPathNavigation) {
			return false;
		}
		if (mob.onGround()) {
			return false; // on GTA IV's ground (described) or a Minecraft block
		}
		if (!groundUnknown(level, mob.blockPosition())) {
			return false;
		}
		held++;
		if (logs < 20 && (held == 1 || held % 2000 == 0)) {
			logs++;
			LibertyCraft.LOG.info("[LibertyCraft] {} held at {} {} {}: GTA IV hasn't described the ground under it yet ({} mob ticks held so far)",
				mob.getType().toShortString(), String.format("%.1f", mob.getX()), String.format("%.1f", mob.getY()), String.format("%.1f", mob.getZ()), held);
		}
		return true;
	}

	/**
	 * Walking down from the feet (LOOK_DOWN blocks): a region GTA IV hasn't sent comes before any of its
	 * geometry, and no Minecraft block is there to land on. Known open air further down is a real drop.
	 */
	static boolean groundUnknown(Level level, BlockPos feet) {
		BlockPos.MutableBlockPos pos = feet.mutable();
		boolean unknown = false;
		for (int dy = 0; dy <= LOOK_DOWN; dy++) {
			pos.setY(feet.getY() - dy);
			if (!level.getBlockState(pos).isAir()) {
				return false; // Minecraft's own blocks (or water) hold it
			}
			if (!HostCollision.isKnown(pos.getX(), pos.getY(), pos.getZ())) {
				unknown = true;
			} else if (!unknown && HostCollision.hasGeometry(pos)) {
				return false;
			}
		}
		return unknown;
	}
}
