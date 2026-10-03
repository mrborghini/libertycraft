package dev.libertycraft.client;

import dev.libertycraft.combat.HostCombat;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.rendering.v1.EntityRendererRegistry;
import net.minecraft.client.renderer.entity.NoopRenderer;

public final class LibertyCraftClient implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		dev.libertycraft.link.Link.announceRunning();
		DiscordPresence.start();
		DestructionToggle.register();
		// Multiplayer without editing files: the host opens their world to LAN (O, Open to LAN) and
		// e4mc gives them a link; friends type /join <link> in chat, and /leave to come back.
		net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback.EVENT.register((dispatcher, context) -> {
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("join")
				.then(net.fabricmc.fabric.api.client.command.v2.ClientCommands.argument("link", com.mojang.brigadier.arguments.StringArgumentType.greedyString())
					.executes(c -> {
						String link = com.mojang.brigadier.arguments.StringArgumentType.getString(c, "link");
						c.getSource().sendFeedback(net.minecraft.network.chat.Component.literal("Joining " + link.trim() + "..."));
						// After the chat screen has closed: this leaves the current world.
						net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.joinFriend(net.minecraft.client.Minecraft.getInstance(), link));
						return 1;
					})));
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("leave").executes(c -> {
				net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.leaveFriend(net.minecraft.client.Minecraft.getInstance()));
				return 1;
			}));
		});
		ClientTickEvents.END_CLIENT_TICK.register(HostClient::clientTick);
		// GTA IV's peds and cars that moved into the player put it back out (and cars shove it).
		ClientTickEvents.END_CLIENT_TICK.register(ProxyPushClient::tick);
		// Multiplayer testing on one PC: LIBERTYCRAFT_LAN_PORT opens the world to LAN on that port as soon
		// as it's loaded, and LIBERTYCRAFT_LAN_OFFLINE lets offline (dev) clients join it.
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents.JOIN.register((handler, sender, minecraft) -> {
			String port = System.getenv("LIBERTYCRAFT_LAN_PORT");
			var server = minecraft.getSingleplayerServer();
			if (port == null || port.isBlank() || server == null || server.isPublished()) {
				return;
			}
			minecraft.execute(() -> {
				if (System.getenv("LIBERTYCRAFT_LAN_OFFLINE") != null) {
					server.setUsesAuthentication(false);
				}
				boolean ok = server.publishServer(net.minecraft.server.MinecraftServer.MultiplayerScope.LAN, false, Integer.parseInt(port.trim()));
				dev.libertycraft.LibertyCraft.LOG.info("[LibertyCraft] world opened to LAN on port {} ({}{})", port.trim(), ok ? "ok" : "FAILED",
					System.getenv("LIBERTYCRAFT_LAN_OFFLINE") != null ? ", offline logins allowed" : "");
			});
		});
		// A guest in a friend's world: dying there kills this player's own GTA IV character.
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.registerGlobalReceiver(dev.libertycraft.net.LcNet.Died.TYPE, (payload, context) -> {
			if (dev.libertycraft.link.Link.active()) {
				dev.libertycraft.link.Link.pushEvent(dev.libertycraft.link.Proto.EV_PLAYER_DIED, payload.attackerFormId(), 0, 0, 0, 0, 0);
			}
		});
		// GTA IV draws the real NPC; its Minecraft stand-in is only a hitbox.
		EntityRendererRegistry.register(HostCombat.HOST_ACTOR, NoopRenderer::new);
		// Players (client-side movement AND the integrated server's re-check of it) use the smooth
		// triangle collider, never GTA IV's voxels; otherwise the server sees the smooth position
		// dip into a voxel and teleports the player back every few ticks.
		// GTA IV's vehicle mounts (HostDrive) are only seats: they never collide with GTA IV's voxels
		// either, so the server takes the client's moves of them as they come.
		dev.libertycraft.world.HostCollision.setSmoothCollider(e -> (e instanceof net.minecraft.world.entity.player.Player && HostClient.linked())
			|| dev.libertycraft.world.HostDrive.isMount(e));
	}
}
