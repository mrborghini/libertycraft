package dev.libertycraft.client;

import dev.libertycraft.combat.HostActorEntity;
import dev.libertycraft.link.Link;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.world.entity.Entity;

/**
 * Puts the client's copies of the GTA IV actor stand-ins exactly where GTA IV has the actors this
 * frame, so the crosshair and melee reach line up with what's on screen (the server copy only
 * moves once per tick and reaches the client a tick or two later).
 */
final class ProxySync {
	private static final List<Link.Actor> ACTORS = new ArrayList<>();
	private static final Map<Integer, Link.Actor> BY_ID = new HashMap<>();

	private ProxySync() {
	}

	static void frame(Minecraft minecraft) {
		if (minecraft.level == null || !Link.readActors(ACTORS)) {
			return;
		}
		BY_ID.clear();
		for (Link.Actor a : ACTORS) {
			BY_ID.put(a.formId(), a);
		}
		for (Entity entity : minecraft.level.entitiesForRendering()) {
			if (entity instanceof HostActorEntity proxy) {
				Link.Actor a = BY_ID.get(proxy.formId());
				if (a == null) {
					continue;
				}
				proxy.setSize(a.width(), a.height());
				if (proxy.getX() != a.x() || proxy.getY() != a.y() || proxy.getZ() != a.z()) {
					proxy.setPos(a.x(), a.y(), a.z()); // (a new bounding box each time: not for the many that stand still)
				}
				proxy.xo = a.x();
				proxy.yo = a.y();
				proxy.zo = a.z();
				proxy.setYRot(a.yaw());
				proxy.yRotO = a.yaw();
			}
		}
	}
}
