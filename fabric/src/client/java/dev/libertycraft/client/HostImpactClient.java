package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Proto;
import net.minecraft.client.Minecraft;
import net.minecraft.client.particle.TerrainParticle;
import net.minecraft.client.resources.sounds.SimpleSoundInstance;
import net.minecraft.client.resources.sounds.SoundInstance;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.level.block.state.BlockState;

/**
 * One of GTA IV's bullets stopped at a block ({@link Proto#IN_BULLET_IMPACT}): a puff of the block's
 * crumbs (as when it is being mined), a little smoke and a few sparks fly off its face, and it plays
 * its hit sound. The particles reach GTA IV's frame with the rest of Minecraft's (the scene stream).
 */
public final class HostImpactClient {
	private static final int PARTICLES = 10;
	private static int logged;

	private HostImpactClient() {
	}

	static void impact(Minecraft minecraft, int face, double x, double y, double z) {
		var level = minecraft.level;
		if (level == null || minecraft.player == null || face < 0 || face >= Direction.values().length) {
			return;
		}
		Direction dir = Direction.from3DDataValue(face);
		// The block the bullet went into: just behind the face.
		BlockPos pos = BlockPos.containing(x - dir.getStepX() * 0.05, y - dir.getStepY() * 0.05, z - dir.getStepZ() * 0.05);
		BlockState state = level.getBlockState(pos);
		if (logged < 5) {
			logged++;
			LibertyCraft.LOG.info("[LibertyCraft] a GTA IV bullet hit {} at {} {} {} (face {})", state, String.format("%.2f", x), String.format("%.2f", y),
				String.format("%.2f", z), dir);
		}
		if (state.isAir()) {
			return;
		}
		var random = level.getRandom();
		for (int i = 0; i < PARTICLES; i++) {
			double px = x + dir.getStepX() * 0.05 + (random.nextDouble() - 0.5) * 0.15;
			double py = y + dir.getStepY() * 0.05 + (random.nextDouble() - 0.5) * 0.15;
			double pz = z + dir.getStepZ() * 0.05 + (random.nextDouble() - 0.5) * 0.15;
			double s = 0.08;
			minecraft.particleEngine.add(new TerrainParticle(level, px, py, pz, dir.getStepX() * s, dir.getStepY() * s + 0.03, dir.getStepZ() * s, state, pos)
				.setPower(0.3F).scale(0.8F));
		}
		double ox = x + dir.getStepX() * 0.1, oy = y + dir.getStepY() * 0.1, oz = z + dir.getStepZ() * 0.1;
		level.addParticle(ParticleTypes.SMOKE, ox, oy, oz, dir.getStepX() * 0.03, 0.02, dir.getStepZ() * 0.03);
		level.addParticle(ParticleTypes.POOF, ox, oy, oz, dir.getStepX() * 0.02, 0.01, dir.getStepZ() * 0.02);
		for (int i = 0; i < 3; i++) {
			level.addParticle(ParticleTypes.CRIT, ox, oy, oz, dir.getStepX() * 0.3 + (random.nextDouble() - 0.5) * 0.3, random.nextDouble() * 0.2,
				dir.getStepZ() * 0.3 + (random.nextDouble() - 0.5) * 0.3);
		}
		var sound = state.getSoundType();
		minecraft.getSoundManager().play(new SimpleSoundInstance(
			sound.getHitSound(), SoundSource.BLOCKS, (sound.getVolume() + 1.0F) / 4.0F, sound.getPitch() * 0.8F, SoundInstance.createUnseededRandom(), pos
		));
	}
}
