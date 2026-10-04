// The blocky city (kMcBlockyCity): Minecraft's player went through a nether portal into
// libertycraft:blocky_city, a block copy of Liberty City at the same coordinates that Minecraft
// streams like any other blocks. Everything stays in GTA IV and goes on as usual (puppet mode,
// teleports, combat, vehicles); only GTA's own map geometry is hidden meanwhile, so the blocks are
// the city: every building (map sections, terrain, interiors and their LODs) and every loose object
// (street furniture, props, doors; not weapons, pickups or what peds hold) gets its visible flag
// (CEntity +0x24 bit 5) and its cast-shadows flag (+0x28 0x80) cleared, every frame for what
// streams in, and put back when the player returns. Sky, water, peds, vehicles and the HUD stay.
// GTA's collision stays too: it is the same city, so its peds and cars walk and drive on it (the
// city's own blocks don't reach NpcBlocks / Hazards: the mod leaves them out of kRenSolids and
// kRenLiquids).
//
// Game thread (Game::Tick).
#pragma once

namespace lc::BlockyCity
{
	// Every frame: a_inCity = McState has kMcBlockyCity (and Minecraft is alive, the game isn't loading).
	void Tick(bool a_inCity, float a_dt);
	// A save is loading: the entities are going away (nothing to put back).
	void OnIngameStartup();
	// GTA's map is hidden for the blocky city.
	bool Active();
}
