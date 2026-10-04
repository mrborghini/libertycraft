package dev.libertycraft.mixin;

import dev.libertycraft.world.city.BlockyCity;
import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.libertycraft.world.HostClip;
import net.minecraft.world.item.Item;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Buckets, boats and other "use on what I'm looking at" items see GTA IV surfaces. */
@Mixin(Item.class)
public abstract class ItemMixin {
	@WrapOperation(
		method = "getPlayerPOVHitResult",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private static BlockHitResult libertycraft$povHost(Level level, ClipContext context, Operation<BlockHitResult> original) {
		// These items act on hitPos.relative(face): report the cell behind the one a block placed there
		// would go into, so a bucket pours into that cell (sitting on the ground like a placed block
		// does) instead of a block higher where the ground is low in its cell.
		if (BlockyCity.isCity(level)) {
			return original.call(level, context);
		}
		BlockHitResult hit = HostClip.refine(context.getFrom(), context.getTo(), original.call(level, context), HostClip.Use.PICK);
		return hit instanceof HostClip.HostHitResult ? HostClip.behindFace(hit, false) : hit;
	}
}
