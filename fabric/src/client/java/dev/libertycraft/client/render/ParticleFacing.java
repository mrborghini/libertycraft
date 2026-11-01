package dev.libertycraft.client.render;

import java.util.BitSet;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.client.particle.SingleQuadParticle;
import net.minecraft.client.renderer.state.level.QuadParticleRenderState;
import org.joml.Quaternionf;
import org.jspecify.annotations.Nullable;

/**
 * Particles that face the camera, laid out in its plane for GTA IV to turn to its own camera.
 *
 * <p>Minecraft turns a particle to its camera when it extracts it: QuadParticleRenderState.add gets the
 * camera's rotation times the particle's roll. GTA IV draws that mesh with its own camera, which isn't
 * Minecraft's while GTA IV drives (a car, Niko mode) and moves on between Minecraft's frames, so the
 * particles kept facing where Minecraft's camera looked. While linked, a particle that faces the camera is
 * stored with its roll only (QuadParticleRenderStateMixin): its quad comes out in the camera's plane,
 * corners = centre + (x, y, 0), and goes to GTA IV in a billboard batch (Proto.BATCH_BILLBOARD) that GTA IV
 * turns to its camera every frame. A particle with a rotation of its own (a shriek's flat ring, a
 * vibration on its way, the trial spawner's upright ones) is left as Minecraft made it.
 */
public final class ParticleFacing {
	/** QuadParticleRenderState's particles per layer, in the order buildLayer emits them (QuadParticleRenderStateMixin). */
	public interface Holder {
		Map<SingleQuadParticle.Layer, Layer> libertycraft$facing();
	}

	/** One layer: bit i set when its particle i faces the camera (laid out in the camera's plane). */
	public static final class Layer {
		final BitSet facing = new BitSet();
		int count;

		public void add(boolean faces) {
			if (faces) {
				this.facing.set(this.count);
			}
			this.count++;
		}
	}

	// Within this of a pure roll about the camera's view axis, a rotation faces the camera.
	private static final float ROLL_ONLY = 1.0E-3F;
	private static final Quaternionf RELATIVE = new Quaternionf();

	private ParticleFacing() {
	}

	/**
	 * The particle's rotation relative to Minecraft's camera, into {@code out}, when it faces the camera
	 * (what is left is its roll); false, {@code out} untouched, when it has a rotation of its own.
	 */
	public static boolean toCameraPlane(float qx, float qy, float qz, float qw, float[] out) {
		RELATIVE.set(Minecraft.getInstance().gameRenderer.mainCamera().rotation()).conjugate().mul(qx, qy, qz, qw);
		if (Math.abs(RELATIVE.x) > ROLL_ONLY || Math.abs(RELATIVE.y) > ROLL_ONLY) {
			return false;
		}
		out[0] = RELATIVE.x;
		out[1] = RELATIVE.y;
		out[2] = RELATIVE.z;
		out[3] = RELATIVE.w;
		return true;
	}

	/** Which of the layer's particles face the camera, or null when that wasn't recorded. */
	static @Nullable BitSet of(QuadParticleRenderState particles, SingleQuadParticle.Layer layer) {
		Layer known = ((Holder) particles).libertycraft$facing().get(layer);
		return known != null ? known.facing : null;
	}
}
