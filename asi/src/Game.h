// The per-frame hub (port of SkyCraft's Game): GTA state -> SkyState, McState -> puppeted player
// ped and camera, the teleport handshake, mouse look, collision scheduling.
//
// Threads: Tick() runs in processScriptsEvent (game thread, natives allowed), Camera() in
// processCameraEvent (game thread, no script context: memory writes only). Input's window
// procedure and Render's draw callback read Shared() from their own threads.
#pragma once

#include <atomic>
#include <cstdint>

namespace lc::Game
{
	struct Shared
	{
		std::atomic<bool> linkReady{ false };
		std::atomic<bool> puppeting{ false };     // Minecraft drives the player: route input to it
		std::atomic<bool> mcScreenOpen{ false };  // a Minecraft GUI screen is open (cursor + text input)
		std::atomic<bool> gtaMenuOpen{ true };    // pause menu / loading / fading: input belongs to GTA
		std::atomic<int>  viewportW{ 0 };         // back buffer size, captured by Render
		std::atomic<int>  viewportH{ 0 };
		std::atomic<int>  cursorX{ 0 };           // Minecraft's GUI cursor (overlay pixels)
		std::atomic<int>  cursorY{ 0 };
		std::atomic<float> sensitivity{ 0.5f };   // Minecraft's mouse sensitivity option
	};

	Shared& State();

	void Tick();               // processScriptsEvent
	void Camera();             // processCameraEvent
	void OnIngameStartup();    // ingameStartupEvent: a save / new game / episode is about to load
	// Pushes kInHurt (Combat calls it). a_damage is "host damage": Minecraft takes a_damage / 5.
	void ReportHurt(std::uint16_t a_kind, float a_damage, std::uint32_t a_attacker, std::uint32_t a_flags);
}
