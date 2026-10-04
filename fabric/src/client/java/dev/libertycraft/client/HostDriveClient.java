package dev.libertycraft.client;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.link.Link;
import dev.libertycraft.net.LcNet;
import dev.libertycraft.world.HostDrive;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.Vec3;

/**
 * GTA IV drives the player (kSkyHostDrives: Niko mode, a vehicle, a cutscene): the client side.
 * The player doesn't move on its own; every frame it is put where GTA IV has Niko (or, in a vehicle,
 * on its mount at the seat, which the server spawns), and every tick the server hears about it
 * (LcNet.Drive). Teleports are acknowledged as they come (HostClient). Render thread only.
 */
public final class HostDriveClient {
	private static boolean driving;
	private static boolean inVehicle;
	private static int ticks;

	private HostDriveClient() {
	}

	public static boolean driving() {
		return driving;
	}

	/** On GTA IV's vehicle mount: the body is shown even in first person (AvatarExporter). */
	public static boolean riding(LocalPlayer player) {
		return driving && inVehicle && player.getVehicle() != null;
	}

	/** Start of every frame, after the SkyState read. True while GTA IV drives: the player only follows. */
	public static boolean frame(Minecraft minecraft, LocalPlayer player, Link.SkyState sky) {
		boolean drives = sky.hostDrives() && !sky.loading();
		if (drives != driving) {
			driving = drives;
			ticks = 0;
			if (drives) {
				LibertyCraft.LOG.info("[LibertyCraft] GTA IV drives the player{}: following it", sky.inVehicle() ? " (in a vehicle)" : "");
				InputBridge.releaseAll();
				if (minecraft.gui.screen() != null) {
					player.closeContainer();
				}
			} else {
				LibertyCraft.LOG.info("[LibertyCraft] GTA IV let go of the player");
				if (player.getVehicle() != null) {
					// Off the mount right away, or riding it would pull the player back from GTA IV's
					// teleport (which comes this same frame) until the server's dismount arrives.
					player.removeVehicle();
				}
				send(sky, 0); // the server drops the mount and puts the player at sky pos
				HostDrive.clientMount = null;
				inVehicle = false;
			}
		}
		if (!drives) {
			return false;
		}
		if (sky.inVehicle() != inVehicle) {
			inVehicle = sky.inVehicle();
			LibertyCraft.LOG.info("[LibertyCraft] {} a vehicle", inVehicle ? "in" : "out of");
		}
		follow(player, sky);
		return true;
	}

	/** End of every client tick: hold the pose against the tick's own movement and tell the server. */
	public static void tick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!driving || player == null) {
			return;
		}
		Link.SkyState sky = HostClient.sky();
		if (!HostClient.linked()) {
			// GTA IV is gone: nobody drives. HostClient holds the player where they are.
			driving = inVehicle = false;
			HostDrive.clientMount = null;
			send(sky, 0);
			LibertyCraft.LOG.info("[LibertyCraft] GTA IV link lost while it drove the player; letting go");
			return;
		}
		follow(player, sky);
		send(sky, HostDrive.FLAG_DRIVES | (inVehicle ? HostDrive.FLAG_IN_VEHICLE : 0));
		if (++ticks % 100 == 0) {
			Entity vehicle = player.getVehicle();
			LibertyCraft.LOG.info("[LibertyCraft] following GTA IV at {} {} {} yaw {}, riding {}", fmt(sky.x), fmt(sky.y), fmt(sky.z), fmt(sky.yaw),
				vehicle == null ? "nothing" : vehicle.getType().toShortString() + " at " + fmt(vehicle.getX()) + " " + fmt(vehicle.getY()) + " " + fmt(vehicle.getZ()));
		}
	}

	private static void follow(LocalPlayer player, Link.SkyState sky) {
		Entity vehicle = player.getVehicle();
		if (inVehicle && vehicle != null) {
			// Our copy of a boat (or a saddled horse) is the one that counts: the server takes its moves.
			HostDrive.clientMount = vehicle;
			HostDrive.place(vehicle, player, sky.x, sky.y, sky.z, sky.yaw);
			vehicle.setOldPosAndRot(); // rendered where GTA IV's seat is now, not a tick behind
		} else {
			// (Still in GTA IV's vehicle but off the mount for a moment: it stays our mount, so it stays hidden.)
			if (!inVehicle) {
				HostDrive.clientMount = null;
			}
			if (vehicle == null) {
				player.setPos(sky.x, sky.y, sky.z);
			}
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		player.setOldPosAndRot();
	}

	private static void send(Link.SkyState sky, int flags) {
		if (ClientPlayNetworking.canSend(LcNet.Drive.TYPE)) {
			ClientPlayNetworking.send(new LcNet.Drive(sky.x, sky.y, sky.z, sky.yaw, flags));
		}
	}

	private static String fmt(double v) {
		return String.format(java.util.Locale.ROOT, "%.2f", v);
	}
}
