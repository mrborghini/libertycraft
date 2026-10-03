package dev.libertycraft.mixin;

import dev.libertycraft.LibertyCraft;
import dev.libertycraft.combat.HostCombat;
import dev.libertycraft.combat.HostActorEntity;
import dev.libertycraft.link.Proto;
import dev.libertycraft.link.Link;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.server.level.ServerPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	/** Critical hits on a GTA IV actor are flagged so GTA IV can play them up. */
	@Inject(method = "crit", at = @At("HEAD"))
	private void libertycraft$critHost(Entity entity, CallbackInfo ci) {
		if (entity instanceof HostActorEntity proxy) {
			proxy.markCritical();
		}
	}

	/** Dying in Minecraft is dying in GTA IV: the host's through the link, a guest's through theirs. */
	@Inject(method = "die", at = @At("HEAD"))
	private void libertycraft$diesInHost(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		int attacker = HostCombat.attackerFormId(source);
		if (!dev.libertycraft.net.LcNet.isHost(self)) {
			if (net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.canSend(self, dev.libertycraft.net.LcNet.Died.TYPE)) {
				net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.send(self, new dev.libertycraft.net.LcNet.Died(attacker));
			}
			LibertyCraft.LOG.info("[LibertyCraft] guest {} died ({}); telling their GTA IV", self.getPlainTextName(), source.getMsgId());
			return;
		}
		if (Link.active()) {
			Link.pushEvent(Proto.EV_PLAYER_DIED, attacker, 0, 0, 0, 0, 0);
			LibertyCraft.LOG.info("[LibertyCraft] player died ({}); telling GTA IV", source.getMsgId());
		}
	}
}
