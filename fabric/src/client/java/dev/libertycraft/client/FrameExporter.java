package dev.libertycraft.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import dev.libertycraft.link.OverlayShipper;
import dev.libertycraft.link.Proto;
import java.lang.foreign.MemorySegment;
import net.minecraft.client.Minecraft;

/**
 * Copies Minecraft's main render target (hand + HUD + screens on a transparent background) back
 * from the GPU and publishes it to GTA IV through the overlay triple buffer.
 *
 * The copy is asynchronous: a frame is captured into one of a few staging buffers and shipped
 * once the GPU says the copy finished, typically a frame later.
 *
 * <p>Only what changed is shipped (OverlayShipper: dirty tiles, unchanged frames skipped).
 */
public final class FrameExporter {
	private static final int STAGING = 3;
	private static final int FREE = 0;
	private static final int PENDING = 1;
	private static final int READY = 2;

	private static final Staging[] staging = new Staging[STAGING];
	private static long nextFrameId = 1;
	private static boolean loggedFormat;

	private static final OverlayShipper SHIPPER = new OverlayShipper();
	private static final OverlayShipper.Target LINK = new OverlayShipper.Target() {
		@Override
		public MemorySegment memory() {
			return Link.segment();
		}

		@Override
		public int backSlot() {
			return Link.overlayBackSlot();
		}

		@Override
		public long backSlotOffset() {
			return Link.overlayBackSlotOffset();
		}

		@Override
		public boolean middleUnread() {
			return Link.overlayMiddleUnread();
		}

		@Override
		public void publish(int w, int h, long frameId, long baseFrameId, int[] tiles) {
			Link.publishOverlay(w, h, true, frameId, baseFrameId, tiles);
		}
	};

	private static final class Staging {
		GpuBuffer buffer;
		int width;
		int height;
		volatile int state = FREE;
		long frameId;
	}

	private FrameExporter() {
	}

	public static void capture(Minecraft minecraft) {
		shipReadyFrames();

		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		if (color == null) {
			return;
		}
		int width = target.width;
		int height = target.height;
		if (width > Proto.MAX_OVERLAY_W || height > Proto.MAX_OVERLAY_H) {
			return;
		}
		if (!loggedFormat) {
			loggedFormat = true;
			LibertyCraft.LOG.info("[LibertyCraft] overlay capture {}x{} format {}", width, height, color.getFormat());
		}

		Staging slot = null;
		for (Staging s : staging) {
			if (s != null && s.state == FREE) {
				slot = s;
				break;
			}
		}
		if (slot == null) {
			for (int i = 0; i < STAGING; i++) {
				if (staging[i] == null) {
					staging[i] = slot = new Staging();
					break;
				}
			}
		}
		if (slot == null) {
			return; // all staging buffers still in flight; skip this frame
		}

		long bytes = (long) width * height * 4L;
		if (slot.buffer == null || slot.width != width || slot.height != height) {
			if (slot.buffer != null) {
				slot.buffer.close();
			}
			slot.buffer = RenderSystem.getDevice().createBuffer(() -> "LibertyCraft overlay readback", 9, bytes);
			slot.width = width;
			slot.height = height;
		}
		final Staging captured = slot;
		captured.state = PENDING;
		captured.frameId = nextFrameId++;
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(color, captured.buffer, 0L, () -> captured.state = READY, 0);
	}

	/** Maps the newest finished readback and copies it into shared memory. */
	private static void shipReadyFrames() {
		Staging newest = null;
		for (Staging s : staging) {
			if (s != null && s.state == READY && (newest == null || s.frameId > newest.frameId)) {
				newest = s;
			}
		}
		if (newest == null) {
			return;
		}
		MemorySegment shm = Link.segment();
		if (shm != null) {
			try (GpuBufferSlice.MappedView view = newest.buffer.map(true, false)) {
				if (SHIPPER.ship(MemorySegment.ofBuffer(view.data()), newest.width, newest.height, newest.frameId, Link.generation(), LINK)
					&& SHIPPER.shipped % 3000 == 0) {
					LibertyCraft.LOG.info("[LibertyCraft] overlay: {} frames shipped ({} tiles of 256 on average), {} unchanged ones skipped", SHIPPER.shipped,
						String.format("%.1f", (double) SHIPPER.tilesShipped / SHIPPER.shipped), SHIPPER.skipped);
				}
			}
		}
		// Anything older than what we just shipped is useless now.
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.frameId <= newest.frameId) {
				s.state = FREE;
			}
		}
	}
}
