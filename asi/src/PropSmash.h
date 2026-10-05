// The Minecraft player at speed against GTA IV's world (drive/PropHit.h has the maths):
//  - elytra crashes (kEvImpact): flying into a wall or onto the ground hard knocks the player over
//    (HostDrive::KnockDown: a ragdoll along the crash; Minecraft hurts its player itself);
//  - props (kEvMover): sprinting, in elytra flight or on a Minecraft mount, running into GTA IV's
//    street furniture (traffic lights, lamp posts, signs, bins, hydrants, fences: Collision's tracked
//    objects) fast enough knocks it over the way a car does. A little ahead of the contact the prop
//    leaves Minecraft's collision (so Minecraft doesn't stop the mover at it); at the contact GTA IV's
//    physics takes it (dynamic, sent off along the mover's way, tipping over) and Minecraft slows the
//    mover by the momentum it lost (kInPropHit). Too slow, the prop stays and Minecraft stops the mover.
//
// Game thread (Combat's event drain, Game::Tick).
#pragma once

#include "libertycraft_protocol.h"

namespace lc::PropSmash
{
	// Combat::Tick's event drain: kEvImpact and kEvMover.
	void OnEvent(const ::libertycraft::proto::McEvent& a_ev);
	// Game::Tick, every frame. a_puppeting: Minecraft drives the player (Minecraft mode, on foot).
	void Tick(int a_ped, bool a_puppeting, bool a_paused, float a_dt);
	// Game::OnIngameStartup.
	void OnIngameStartup();
}
