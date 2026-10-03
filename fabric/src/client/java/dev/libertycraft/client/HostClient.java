package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.client.render.WorldExporter;
import dev.libertycraft.link.Proto;
import dev.libertycraft.link.Link;
import dev.libertycraft.world.HostCollision;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.sdl.SDLVideo;

/**
 * Per-frame glue between the Minecraft client and GTA IV. Everything here runs on the render
 * thread, called from MinecraftMixin.
 */
public final class HostClient {
	private static final boolean SHOW_WINDOW = Boolean.getBoolean("libertycraft.showWindow");
	// Started by GTA IV (LibertyCraft's bundled instance passes -Dlibertycraft.startHidden=true): no window and
	// no title-screen music from the first frame, even while GTA IV is paused (Alt-Tabbed) and the
	// two haven't linked up yet. Otherwise the window only goes once GTA IV is there.
	private static final boolean START_HIDDEN = Boolean.getBoolean("libertycraft.startHidden");
	private static boolean startedHidden;

	private static final Link.SkyState sky = new Link.SkyState();
	private static final Link.McState mc = new Link.McState();
	private static volatile boolean linked;
	private static boolean tookOver;
	private static boolean windowHidden;
	private static int appliedViewportW, appliedViewportH;

	// Teleport / hold state: GTA IV decides where the player is after loads, doors and respawns.
	private static int lastTeleportSeq = -1;
	private static int teleportAck;
	private static boolean teleportPending;
	private static LocalPlayer lastPlayer;
	private static Vec3 holdPos;
	private static Vec3 unlinkedHold;
	private static long holdSince;
	private static long qpcFreq;
	private static LocalPlayer eyePlayer;
	private static float eyeSmoothed;
	private static long frameCounter;
	private static int lastPacedSeq;
	private static boolean hostStalled;
	private static int exporterErrors;

	private HostClient() {
	}

	public static boolean linked() {
		return linked;
	}

	/**
	 * True once GTA IV has connected in this session. From then on Minecraft never touches the
	 * real mouse or keyboard again (even if GTA IV closes), since its window is hidden.
	 */
	public static boolean tookOver() {
		return tookOver;
	}

	public static Link.SkyState sky() {
		return sky;
	}

	/** Start of Minecraft.runTick: pull state and input from GTA IV before anything else runs. */
	public static void beginFrame() {
		Link.poll();
		quitWithHost(Minecraft.getInstance());
		if (START_HIDDEN && !startedHidden) {
			startedHidden = true;
			Minecraft minecraft = Minecraft.getInstance();
			hideWindowOnce(minecraft);
			minecraft.options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
			minecraft.getMusicManager().stopPlaying();
		}
		boolean nowLinked = Link.active();
		if (nowLinked) {
			Link.readSkyState(sky); // on a torn read we simply keep last frame's state
			dev.libertycraft.world.HostWater.refresh();
		} else {
			dev.libertycraft.world.HostWater.clear();
		}
		if (nowLinked != linked) {
			linked = nowLinked;
			LibertyCraft.LOG.info("[LibertyCraft] GTA IV link {}", linked ? "up" : "down");
			if (linked) {
				tookOver = true;
				unlinkedHold = null;
				HostCollision.startConsumer();
				applyLinkedOptions();
			} else {
				InputBridge.releaseAll();
				LocalPlayer player = Minecraft.getInstance().player;
				unlinkedHold = player != null ? player.position() : null;
			}
		}
		if (!linked) {
			return;
		}

		Minecraft minecraft = Minecraft.getInstance();
		hideWindowOnce(minecraft);
		applyViewportSize(minecraft);
		MirrorWorld.openWhenReady(minecraft);

		if (sky.menuOpen() || sky.loading()) {
			InputBridge.releaseAll();
		}
		InputBridge.drain(minecraft);
		ProxySync.frame(minecraft);

		LocalPlayer player = minecraft.player;
		if (player == null) {
			lastPlayer = null;
			return;
		}

		// A new player object means we just joined or respawned: put it where GTA IV's player is.
		if (player != lastPlayer) {
			lastPlayer = player;
			teleportPending = true;
		}
		if (HostDriveClient.frame(minecraft, player, sky)) {
			// GTA IV drives (Niko mode, a vehicle, a cutscene): the player follows it every frame, so
			// a teleport has nothing to wait for. When GTA IV lets go it sends a fresh one.
			lastTeleportSeq = sky.teleportSeq;
			teleportAck = sky.teleportSeq;
			teleportPending = false;
			holdPos = null;
		}
		if (sky.teleportSeq != lastTeleportSeq) {
			lastTeleportSeq = sky.teleportSeq;
			teleportPending = true;
		}
		if (teleportPending && sky.inGame() && !sky.loading()) {
			requestTeleport(minecraft, sky.x, sky.y, sky.z, sky.yaw, sky.pitch);
			teleportAck = sky.teleportSeq;
			teleportPending = false;
			holdPos = new Vec3(sky.x, sky.y, sky.z);
			holdSince = 0;
		}

		// Look direction is driven by GTA IV (zero-latency camera); MC uses it for everything else.
		if (minecraft.gui.screen() == null) {
			player.setYRot(sky.yaw);
			player.setXRot(sky.pitch);
			player.yRotO = sky.yaw;
			player.xRotO = sky.pitch;
		}
	}

	// Minecraft is started with GTA IV (the ASI plugin launches it), so it goes when that GTA IV has
	// closed for good: saved and shut down the normal way. -Dlibertycraft.quitWithHost=false keeps it
	// running instead (development: restarting GTA IV without restarting Minecraft).
	private static final boolean QUIT_WITH_HOST = Boolean.parseBoolean(System.getProperty("libertycraft.quitWithHost", "true"));
	private static long hostGoneSince;
	private static long nextHostCheck;
	// Started hidden by GTA IV but never connected: nobody can see or use this Minecraft, and it
	// would stop the next GTA IV from starting a fresh one ("already running"). It goes after this.
	private static final long NEVER_CONNECTED_QUIT_MS = 10 * 60 * 1000;
	private static final long STARTED_AT = System.currentTimeMillis();
	private static boolean gaveUpWaiting;
	// The bridge file outlives GTA IV, so at startup its header may still name an earlier, closed
	// GTA IV (pid set, heartbeat stale). That one must not count as "GTA IV has closed": only a host
	// we have seen alive in this run can close on us (seen: Minecraft started before GTA IV quit
	// at once on such a stale header).
	private static boolean hostSeenAlive;

	private static void quitWithHost(Minecraft minecraft) {
		int pid = Link.hostPid();
		long now = System.currentTimeMillis();
		if (QUIT_WITH_HOST && START_HIDDEN && (pid == 0 || !hostSeenAlive) && !tookOver && !gaveUpWaiting && now - STARTED_AT > NEVER_CONNECTED_QUIT_MS) {
			gaveUpWaiting = true;
			LibertyCraft.LOG.warn("[LibertyCraft] started hidden but GTA IV never connected in {} minutes; quitting", NEVER_CONNECTED_QUIT_MS / 60000);
			minecraft.stop();
			return;
		}
		if (!QUIT_WITH_HOST || pid == 0 || now < nextHostCheck) {
			return;
		}
		nextHostCheck = now + 1000;
		// Process check on Windows, heartbeat age (8 s) under Wine: see Link.hostAlive.
		if (Link.hostAlive()) {
			hostSeenAlive = true;
			hostGoneSince = 0;
			return;
		}
		if (!hostSeenAlive) {
			return;   // a stale header from an earlier GTA IV: keep waiting for a live one
		}
		if (hostGoneSince == 0) {
			hostGoneSince = now;
		} else if (now - hostGoneSince > 5000) {
			LibertyCraft.LOG.info("[LibertyCraft] GTA IV (pid {}) has closed; saving and quitting", pid);
			minecraft.stop();
		}
	}

	/** Called at the end of every client tick. */
	public static void clientTick(Minecraft minecraft) {
		MirrorWorld.tick(minecraft);
		DiscordPresence.tick(minecraft);
		HostDigClient.tick(minecraft);
		freezeWhileUnlinked(minecraft);
		HostDriveClient.tick(minecraft);
		if (!HostDriveClient.driving()) {
			holdUntilReady(minecraft);
		}
		publishTick(minecraft);
		DevAutorun.tick(minecraft, linked);
	}

	/**
	 * GTA IV went quiet (a long loading screen, a stall, or it closed). Its collision around the
	 * player may be about to change (interior doors), so keep the player exactly where they were
	 * instead of letting them fall; GTA IV puts them where they belong when it's back.
	 */
	private static void freezeWhileUnlinked(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (linked || !tookOver || player == null) {
			return;
		}
		if (unlinkedHold == null) {
			unlinkedHold = player.position();
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(unlinkedHold.x, unlinkedHold.y, unlinkedHold.z);
		player.xo = unlinkedHold.x;
		player.yo = unlinkedHold.y;
		player.zo = unlinkedHold.z;
		player.resetFallDistance();
	}

	/**
	 * Hands GTA IV the raw physics tick (previous + latest feet, smoothed eye height, walk bob) with a
	 * QueryPerformanceCounter timestamp. GTA IV interpolates between them on its own frame clock,
	 * exactly like Minecraft's renderer does with partial ticks.
	 */
	private static void publishTick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (qpcFreq == 0) {
			qpcFreq = Link.qpcFrequency();
		}
		float tickMs = minecraft.level != null ? minecraft.level.tickRateManager().millisecondsPerTick() : 50.0F;
		// The tick really "happened" partial ticks ago (DeltaTracker keeps the remainder).
		float remainder = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
		mc.tickQpc = Link.qpc() - (long) (remainder * tickMs * qpcFreq / 1000.0);
		mc.tickMs = tickMs;
		mc.prevX = player.xo;
		mc.prevY = player.yo;
		mc.prevZ = player.zo;
		mc.curX = player.getX();
		mc.curY = player.getY();
		mc.curZ = player.getZ();
		// Same smoothing as Camera.tick(): eye height eases halfway toward the target each tick.
		if (player != eyePlayer) {
			eyePlayer = player;
			eyeSmoothed = player.getEyeHeight();
		}
		mc.eyeHeightO = eyeSmoothed;
		eyeSmoothed += (player.getEyeHeight() - eyeSmoothed) * 0.5F;
		mc.eyeHeightT = eyeSmoothed;
		boolean bob = minecraft.options.bobView().get();
		var avatar = player.avatarState();
		mc.walkDistO = bob ? avatar.getInterpolatedWalkDistance(0.0F) : 0.0F;
		mc.walkDist = bob ? avatar.getInterpolatedWalkDistance(1.0F) : 0.0F;
		mc.bobO = bob ? avatar.getInterpolatedBob(0.0F) : 0.0F;
		mc.bob = bob ? avatar.getInterpolatedBob(1.0F) : 0.0F;
		Link.writeMcState(mc);
	}

	/** Freeze the player until GTA IV's collision around them has arrived. */
	private static void holdUntilReady(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (!sky.inGame() || sky.loading()) {
			// GTA IV is on its main menu or a loading screen: park the player where they are.
			if (holdPos == null) {
				holdPos = player.position();
			}
			teleportPending = true;
			holdSince = 0; // the timeouts below count time in game only
		}
		if (holdPos == null) {
			holdSince = 0;
			return;
		}
		if (holdSince == 0) {
			holdSince = System.currentTimeMillis();
		}
		int bx = (int) Math.floor(holdPos.x), by = (int) Math.floor(holdPos.y), bz = (int) Math.floor(holdPos.z);
		// GTA IV announces a new collision epoch (world change, reload) in the same SkyState as the
		// teleport, but its kColClear and the new regions come later through the collision ring:
		// until they have, what we hold is the old world's (seen: released at once on the old
		// regions, then the clear took the floor away and the player fell through it).
		boolean current = sky.collisionEpoch == 0 || HostCollision.epoch() == sky.collisionEpoch;
		boolean known = current && HostCollision.isKnown(bx, by - 1, bz) && HostCollision.isKnown(bx, by, bz)
			&& HostCollision.isKnown(bx, by - HostCollision.REGION_SIZE, bz);
		long held = System.currentTimeMillis() - holdSince;
		// Release once there is actual ground below (or after a timeout, e.g. when mid-air on purpose;
		// or, as a last resort, when the regions never come).
		boolean ready = (known && (HostCollision.hasSolidBelow(bx, by, bz, 12) || held > 6000)) || held > 30000;
		if (ready && sky.inGame() && !sky.loading()) {
			if (!known) {
				LibertyCraft.LOG.warn("[LibertyCraft] GTA IV's collision under the player never arrived (epoch {} of {}); releasing them anyway",
					HostCollision.epoch(), sky.collisionEpoch);
			}
			// GTA IV's feet can sit a fraction of a voxel inside our ground layer. Minecraft's
			// collision never pushes you out of a shape, so you'd drop through: lift out first.
			Vec3 safe = liftOutOfGeometry(player, holdPos);
			if (safe.y != holdPos.y) {
				player.setPos(safe.x, safe.y, safe.z);
				player.yo = safe.y;
				LibertyCraft.LOG.info("[LibertyCraft] lifted player {} blocks out of the ground", String.format("%.3f", safe.y - holdPos.y));
			}
			holdPos = null;
			return;
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(holdPos.x, holdPos.y, holdPos.z);
		player.xo = holdPos.x;
		player.yo = holdPos.y;
		player.zo = holdPos.z;
		player.resetFallDistance();
	}

	private static Vec3 liftOutOfGeometry(LocalPlayer player, Vec3 pos) {
		// Stand on the exact GTA IV ground if it is slightly above the feet. GTA IV reports the feet
		// to within centimetres (unlike Skyrim, hence SkyCraft's 2.5 blocks), and collision v2 sends
		// ceilings and awnings too, so only walkable ground up to one block above the feet counts.
		double ground = HostCollider.walkableGroundAt(pos.x, pos.y, pos.z, 1.0);
		return !Double.isNaN(ground) && ground > pos.y ? new Vec3(pos.x, ground, pos.z) : pos;
	}

	private static void requestTeleport(Minecraft minecraft, double x, double y, double z, float yaw, float pitch) {
		LocalPlayer player = minecraft.player;
		player.setPos(x, y, z);
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		var server = minecraft.getSingleplayerServer();
		if (server != null) {
			var uuid = player.getUUID();
			server.execute(() -> {
				ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
				if (sp != null) {
					sp.teleportTo(x, y, z);
					sp.setYRot(yaw);
					sp.setXRot(pitch);
					sp.resetFallDistance();
				}
			});
		}
		LibertyCraft.LOG.info("[LibertyCraft] teleported to {} {} {}", x, y, z);
	}

	/** After GameRenderer.render(): report the player to GTA IV and ship the overlay frame. */
	public static void afterRender() {
		if (!linked) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		int flags = 0;
		if (player != null && minecraft.level != null) {
			float partial = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
			Vec3 feet = player.getPosition(partial);
			Camera camera = minecraft.gameRenderer.mainCamera();
			flags |= Proto.MC_IN_WORLD;
			if (player.onGround()) {
				flags |= Proto.MC_ON_GROUND;
			}
			if (player.isShiftKeyDown()) {
				flags |= Proto.MC_SNEAKING;
			}
			if (player.isSprinting()) {
				flags |= Proto.MC_SPRINTING;
			}
			if (player.isDeadOrDying()) {
				flags |= Proto.MC_DEAD;
			}
			if (player.isSwimming()) {
				flags |= Proto.MC_SWIMMING;
			}
			if (player.getAbilities().flying) {
				flags |= Proto.MC_FLYING;
			}
			mc.x = feet.x;
			mc.y = feet.y;
			mc.z = feet.z;
			mc.yaw = player.getYRot();
			mc.pitch = player.getXRot();
			// The eye, not the camera: in third person Minecraft's camera sits behind or in front.
			Vec3 eye = camera.isDetached() ? player.getEyePosition(partial) : camera.position();
			mc.eyeHeight = (float) (eye.y - feet.y);
			mc.eyeX = eye.x;
			mc.eyeY = eye.y;
			mc.eyeZ = eye.z;
			mc.fov = camera.getFov();
			// Minecraft's F5 camera: GTA IV puts its camera where Minecraft's would be.
			mc.cameraMode = minecraft.options.getCameraType().ordinal();
			mc.cameraDistance = camera.isDetached() ? (float) camera.position().distanceTo(player.getEyePosition(partial)) : 0.0F;
			// Walk bob, exactly what GameRenderer.bobView() uses this frame.
			var entityState = minecraft.gameRenderer.gameRenderState().levelRenderState.cameraRenderState.entityRenderState;
			boolean bob = minecraft.options.bobView().get() && entityState.isPlayer;
			mc.bobPhase = bob ? entityState.backwardsInterpolatedWalkDistance : 0.0F;
			mc.bobAmount = bob ? entityState.bob : 0.0F;
		}
		if (minecraft.gui.screen() != null) {
			flags |= Proto.MC_SCREEN_OPEN;
		}
		mc.flags = flags;
		mc.sensitivity = minecraft.options.sensitivity().get().floatValue();
		mc.teleportAck = holdPos == null ? teleportAck : teleportAck - 1; // not "arrived" until we are released
		mc.guiScale = minecraft.getWindow().getGuiScale();
		mc.frameCounter = ++frameCounter;
		Link.writeMcState(mc);

		if ((flags & Proto.MC_IN_WORLD) != 0) {
			try {
				WorldExporter.frame(minecraft, minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false));
			} catch (RuntimeException e) {
				if (exporterErrors++ < 5) {
					LibertyCraft.LOG.error("[LibertyCraft] world export failed", e);
				}
			}
			FrameExporter.capture(minecraft);
		}
	}

	/** End of the frame: render at most once per GTA IV frame instead of spinning freely. */
	public static void paceFrame() {
		if (!linked) {
			return;
		}
		if (hostStalled && (Link.skyStateSeq() >>> 1) == lastPacedSeq) {
			return; // GTA IV is paused (menu / alt-tab): don't block every frame waiting for it
		}
		hostStalled = false;
		long deadline = System.nanoTime() + 25_000_000L;
		// SkyState.seq advances by 2 per GTA IV frame (odd while writing).
		while ((Link.skyStateSeq() >>> 1) == lastPacedSeq && System.nanoTime() < deadline) {
			Thread.onSpinWait();
			if (deadline - System.nanoTime() > 2_000_000L) {
				Thread.yield();
			}
		}
		int seqNow = Link.skyStateSeq() >>> 1;
		hostStalled = seqNow == lastPacedSeq;
		lastPacedSeq = seqNow;
	}

	private static void applyLinkedOptions() {
		Minecraft minecraft = Minecraft.getInstance();
		var options = minecraft.options;
		options.pauseOnLostFocus = false;
		options.vignette().set(false);
		options.enableVsync().set(false);
		options.framerateLimit().set(260);
		// Minecraft doesn't draw the world itself; these only decide how far out placed blocks,
		// arrows and GTA IV NPC stand-ins stay loaded and simulated.
		options.renderDistance().set(8);
		options.simulationDistance().set(8);
		options.autoJump().set(false);
		options.onboardAccessibility = false;
		if (options.tutorialStep != net.minecraft.client.tutorial.TutorialSteps.NONE) {
			minecraft.getTutorial().setStep(net.minecraft.client.tutorial.TutorialSteps.NONE);
		}
		options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
		options.save();
	}

	private static void hideWindowOnce(Minecraft minecraft) {
		if (windowHidden || SHOW_WINDOW) {
			return;
		}
		windowHidden = true;
		SDLVideo.SDL_HideWindow(minecraft.getWindow().handle());
		LibertyCraft.LOG.info("[LibertyCraft] game window hidden (run with -Dlibertycraft.showWindow=true to keep it)");
	}

	private static void applyViewportSize(Minecraft minecraft) {
		int w = Math.min(sky.viewportW, Proto.MAX_OVERLAY_W);
		int h = Math.min(sky.viewportH, Proto.MAX_OVERLAY_H);
		if (w <= 0 || h <= 0 || (w == appliedViewportW && h == appliedViewportH)) {
			return;
		}
		appliedViewportW = w;
		appliedViewportH = h;
		minecraft.getWindow().setWindowed(w, h);
		LibertyCraft.LOG.info("[LibertyCraft] sizing overlay to GTA IV viewport {}x{}", w, h);
	}
}
