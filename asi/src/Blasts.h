// GTA IV's own explosions reach Minecraft (proto::kInGtaExplosion). GTA IV 1.0.8.0 adds every explosion
// (vehicles and gas pumps blowing up, grenades, rockets, molotovs, ADD_EXPLOSION) through
// CExplosionManager::AddExplosion at 0x9940D0 (cdecl, 19 arguments: the 2nd the entity that caused it,
// the 3rd its type, the 4th its size scale, the 5th a pointer to where; seen in ADD_EXPLOSION's
// native, 0xB13340). Its first instructions jump to a hook of ours (after a byte check) that notes
// each explosion for the game thread; Tick sends them to Minecraft, which hurts and knocks back its
// mobs and player with an explosion of its own there (fabric combat/GtaBlasts.java). Directed blasts
// (steam, flames, a fire hydrant's water) and ones without damage are left out, and so are the ones
// Minecraft asked for (Combat's kEvExplosion and firework blasts, made inside a FromMinecraft scope).
#pragma once

namespace lc::Combat
{
	struct Frame;
}

namespace lc::Blasts
{
	// While alive, ADD_EXPLOSION calls are Minecraft's own blasts mirrored into GTA IV: not sent back.
	struct FromMinecraft
	{
		FromMinecraft();
		~FromMinecraft();
	};

	// Game thread, once per frame (Combat::Tick, before the player's damage is bridged). Hooks GTA IV's
	// explosions the first time. Returns true if one of GTA IV's explosions sent to Minecraft this frame
	// reaches the player (a_px/y/z, GTA): in Minecraft mode Minecraft's explosion hurts him, so GTA IV's
	// own must not (Combat makes him explosion-proof for a moment).
	bool Tick(const Combat::Frame& a_frame, float a_px, float a_py, float a_pz);
}
