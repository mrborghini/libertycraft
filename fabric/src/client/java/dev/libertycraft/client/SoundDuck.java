package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import net.minecraft.client.Minecraft;
import net.minecraft.sounds.SoundSource;

/**
 * Minecraft's sounds duck while a phone call is going on in GTA IV (SkyState's kSkyPhoneCall), so the call
 * can be heard over the mobs, the rain and the music: every sound plays at {@link #DUCKED} of its volume,
 * faded over {@link #FADE_SECONDS} each way. A gain of our own on top of the options' volumes
 * (SoundEngineDuckMixin), so the player's volume settings are never touched. (Cutscenes and mission cameras
 * pause Minecraft and its sounds altogether: GtaMenuPause.)
 */
public final class SoundDuck {
	/** The share of its volume each Minecraft sound keeps during a call. */
	public static final float DUCKED = 0.25F;
	private static final float FADE_SECONDS = 0.3F;
	private static float gain = 1.0F;
	private static long lastNanos;
	private static boolean ducking;

	private SoundDuck() {
	}

	/** The current extra gain on every Minecraft sound (1: none). */
	public static float gain() {
		return gain;
	}

	/** Every frame from HostClient.beginFrame, after the SkyState read. */
	static void frame(Minecraft minecraft, boolean linked, Link.SkyState sky) {
		long now = System.nanoTime();
		float dt = lastNanos == 0 ? 0.0F : Math.min((now - lastNanos) / 1.0e9F, 0.1F);
		lastNanos = now;
		boolean want = linked && sky.phoneCall();
		if (want != ducking) {
			ducking = want;
			LibertyCraft.LOG.info("[LibertyCraft] {}", want ? "a phone call in GTA IV: Minecraft's sounds duck" : "the phone call is over: Minecraft's sounds come back");
		}
		float target = want ? DUCKED : 1.0F;
		if (gain == target) {
			return;
		}
		float step = (1.0F - DUCKED) * dt / FADE_SECONDS;
		gain = gain < target ? Math.min(target, gain + step) : Math.max(target, gain - step);
		for (SoundSource source : SoundSource.values()) {
			minecraft.getSoundManager().refreshCategoryVolume(source);
		}
	}
}
