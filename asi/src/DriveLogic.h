// Who drives the player this frame: Minecraft (puppet mode) or GTA IV itself (Niko mode, getting
// into / sitting in a vehicle, cutscenes). Pure logic, no IV-SDK: HostDrive.cpp feeds it the
// game's state every frame and acts on the verdict; asi/tests/drive_test.cpp tests it on Linux.
//
//  * The toggle key flips between Minecraft mode and Niko mode (plain GTA IV).
//  * The vehicle key (taken from Minecraft while puppeting) hands Niko back to GTA IV and taps
//    GTA's own enter-vehicle control, so the game picks the door or carjacks like normal. GTA acts
//    on the control's press edge (this frame above 127, the last one not) and only while the ped
//    counts as standing, which it doesn't for a moment after puppet mode lets go: a single press
//    then was lost. So the taps start once the caller says GTA reads the player's pad again and
//    Niko stands (controlReady), and repeat until GTA starts walking Niko to a door (moving) or
//    getting in. If Niko isn't getting in after kFallbackAfter, the caller enters the closest car
//    by other means once; after kGiveUpAfter without a car, Minecraft gets the player back.
//  * While GTA drives (hostDrives) Minecraft only follows; when that ends the caller does the
//    teleport handshake (resync) before Minecraft takes over again.
//  * Out of a vehicle (a bail-out from a moving car rolls and tumbles), knocked down (the caller's
//    knockdowns: a car ran the player over, a blast) or ragdolled by GTA in Minecraft mode, GTA keeps
//    the player until Niko really stands again: upright (on his feet, not ragdolled or getting up,
//    not in the air, nearly still) for kUprightSettle, and on foot for kExitSettle at least; at most
//    kRecoverCap.
#pragma once

#include <algorithm>

namespace lc::drive
{
	enum class Mode { kMinecraft, kNiko };

	// Why GTA IV drives the player (Output::why): what it animates Niko for.
	enum class Why { kNone, kNikoMode, kVehicle, kCutscene, kRagdoll };

	struct Input
	{
		float dt = 0.0f;
		bool  inGame = false;     // the player ped exists and the game isn't loading (paused is fine)
		bool  paused = false;     // pause menu: timers stand still
		bool  dead = false;
		bool  inCar = false;      // IS_CHAR_IN_ANY_CAR
		bool  gettingIn = false;  // IS_CHAR_GETTING_IN_TO_A_CAR (our press, or a mission script)
		bool  cutscene = false;
		bool  puppeting = false;  // Minecraft drives the ped right now (before this frame's decision)
		bool  controlReady = false;  // GTA reads the player's pad again and the ped stands (or waited long enough)
		bool  moving = false;     // the ped walks on its own (GTA took the press: off to a door)
		bool  ragdoll = false;    // Niko is ragdolled or getting back up
		bool  upright = true;     // Niko stands on his feet: not ragdolled, getting up or in the air, nearly still
		int   knockdowns = 0;     // the caller knocked the player over this frame (a car ran into them, a blast)
		int   toggles = 0;        // toggle-key presses since the last frame
		int   vehicleActions = 0; // vehicle-key presses since the last frame
	};

	struct Output
	{
		const char* blocker = nullptr;  // why Minecraft may not drive the player (nullptr: it may)
		bool        hostDrives = false; // kSkyHostDrives
		bool        inVehicle = false;  // kSkyInVehicle
		bool        resync = false;     // GTA just let go: teleport Minecraft to the player first
		bool        pressEnter = false; // hold GTA's enter-vehicle control this frame
		int         tapStarted = 0;     // a tap of the enter control starts this frame: its number (1, 2, ...)
		bool        accepted = false;   // GTA took a tap this frame (Niko started moving / getting in)
		bool        fallbackEnter = false;  // once per attempt: the press didn't take, enter another way
		bool        enterFailed = false;    // the attempt timed out without a car
		bool        modeChanged = false;
		Why         why = Why::kNone;      // with hostDrives: Niko mode, a vehicle (getting in, in, just out), a cutscene, knocked over
		bool        recovered = false;     // a recovery ended this frame (Niko stands, or kRecoverCap)
		bool        padLocked = false;     // GTA animates Niko getting back up (on foot): his pad is zeroed and the
		                                   // player's keys and mouse stay out of GTA (only vehicles and Niko mode
		                                   // give GTA real player input)
		bool        recoverCapped = false; // ...because kRecoverCap ran out
	};

	class Logic
	{
	public:
		static constexpr float kTapSeconds = 0.1f;     // how long one tap holds the enter control (and at least kTapFrames)
		static constexpr int   kTapFrames = 2;
		static constexpr float kTapDelay = 0.05f;      // control back this long before the first tap
		static constexpr float kTapInterval = 0.45f;   // from one tap's start to the next, while GTA shows no reaction
		static constexpr int   kMaxTaps = 3;
		static constexpr float kFallbackAfter = 2.0f;  // no "getting in" by then (GTA walks to the door first): other means
		static constexpr float kGiveUpAfter = 4.0f;    // still on foot by then: back to Minecraft
		static constexpr float kExitSettle = 0.5f;     // on foot this long after a car before Minecraft takes over
		static constexpr float kStalledAfter = 1.0f;   // GTA took a tap but Niko stands still this long: not progressing
		static constexpr float kUprightSettle = 0.4f;  // after a vehicle or a fall: upright this long before Minecraft takes over
		static constexpr float kRecoverCap = 8.0f;     // ...but at most this long

		explicit Logic(bool a_startInMinecraft = true) :
			mode_(a_startInMinecraft ? Mode::kMinecraft : Mode::kNiko)
		{}

		Mode mode() const { return mode_; }
		bool entering() const { return entering_; }

		// Forgets an attempt in progress (a save is loading). The mode stays.
		void Reset()
		{
			entering_ = false;
			wasHostDrives_ = false;
			onFoot_ = kExitSettle;
			recovering_ = false;
		}

		bool recovering() const { return recovering_; }
		Why  recoveringFrom() const { return recoverWhy_; }  // with recovering(): a vehicle or a knockdown

		Output Step(const Input& a_in)
		{
			Output out;
			const float dt = a_in.paused ? 0.0f : a_in.dt;
			if (a_in.toggles % 2 != 0) {
				mode_ = mode_ == Mode::kMinecraft ? Mode::kNiko : Mode::kMinecraft;
				out.modeChanged = true;
				entering_ = false;
			}

			if (a_in.vehicleActions > 0 && mode_ == Mode::kMinecraft && a_in.inGame && !a_in.dead && !a_in.inCar && !entering_) {
				entering_ = true;
				enterT_ = 0.0f;
				pressT_ = 0.0f;
				readyT_ = 0.0f;
				taps_ = 0;
				tapLeftT_ = 0.0f;
				tapLeftFrames_ = 0;
				upFrames_ = kTapFrames;
				accepted_ = false;
				stillT_ = 0.0f;
				fallbackDone_ = false;
				fallbackAt_ = 0.0f;
			}
			if (entering_) {
				enterT_ += dt;
				if (a_in.inCar || !a_in.inGame || a_in.dead) {
					entering_ = false;  // in (the vehicle path takes over), or the game took the player away
				} else if (a_in.gettingIn) {
					// GTA is walking Niko to the door / pulling the driver out: let it finish.
					out.accepted = !accepted_;
					accepted_ = true;
					if (enterT_ > kGiveUpAfter + 7.0f) {
						entering_ = false;
						out.enterFailed = true;
					}
				} else if (enterT_ >= (Progressing() ? kGiveUpAfter + 7.0f : std::max(kGiveUpAfter, fallbackAt_ + 1.0f))) {
					entering_ = false;
					out.enterFailed = true;
				} else if (!a_in.puppeting && !a_in.paused) {
					// Only once puppet mode has let go (GTA ignores the pad while it's off).
					if (a_in.moving && !accepted_ && taps_ > 0) {
						accepted_ = true;
						out.accepted = true;
					}
					stillT_ = accepted_ && !a_in.moving ? stillT_ + dt : 0.0f;
					readyT_ = a_in.controlReady ? readyT_ + dt : 0.0f;
					if (tapLeftT_ > 0.0f || tapLeftFrames_ > 0) {
						out.pressEnter = true;  // a tap in progress
						tapLeftT_ -= dt;
						--tapLeftFrames_;
						upFrames_ = 0;
					} else if (!accepted_ && taps_ < kMaxTaps && a_in.controlReady && readyT_ >= kTapDelay + taps_ * kTapInterval
							   && upFrames_ >= kTapFrames) {
						// (released for a few frames first: the next press must be an edge again)
						++taps_;
						out.tapStarted = taps_;
						out.pressEnter = true;
						tapLeftT_ = kTapSeconds - dt;
						tapLeftFrames_ = kTapFrames - 1;
						upFrames_ = 0;
					} else {
						++upFrames_;
					}
					pressT_ += dt;
					if (pressT_ >= kFallbackAfter && !fallbackDone_ && !Progressing()) {
						fallbackDone_ = true;
						fallbackAt_ = enterT_;  // (it gets a second before the attempt gives up)
						out.fallbackEnter = true;
					}
				}
			}

			if (a_in.inCar || a_in.gettingIn) {
				onFoot_ = 0.0f;
			} else {
				onFoot_ = onFoot_ < kExitSettle ? onFoot_ + dt : onFoot_;
			}

			// Getting back up: after a vehicle, a knockdown, or GTA ragdolling Niko in Minecraft mode.
			if (!a_in.inGame || a_in.dead) {
				recovering_ = false;
			} else if (a_in.inCar || a_in.gettingIn) {
				StartRecovery(Why::kVehicle);
			} else if (mode_ == Mode::kMinecraft && (a_in.knockdowns > 0 || (a_in.ragdoll && !recovering_ && !entering_))) {
				StartRecovery(recovering_ ? recoverWhy_ : Why::kRagdoll);
			}
			if (recovering_ && !a_in.inCar && !a_in.gettingIn) {
				recoverT_ += dt;
				uprightT_ = a_in.upright && !a_in.ragdoll ? uprightT_ + dt : 0.0f;
				const bool stands = onFoot_ >= kExitSettle && uprightT_ >= kUprightSettle;
				if (stands || recoverT_ >= kRecoverCap || mode_ == Mode::kNiko) {
					recovering_ = false;
					out.recovered = true;
					out.recoverCapped = !stands && mode_ != Mode::kNiko;
				}
			}

			const char* reason = mode_ == Mode::kNiko ? "Niko mode (toggle key)"
			                     : entering_          ? "entering a vehicle (vehicle key)"
			                     : a_in.gettingIn     ? "the game is putting the player in a vehicle"
			                     : a_in.inCar         ? "player in a vehicle"
			                     : recovering_        ? (recoverWhy_ == Why::kVehicle ? "just left a vehicle (until Niko stands)" : "knocked over (until Niko is back up)")
			                     : a_in.cutscene      ? "cutscene"
			                                          : nullptr;
			out.blocker = reason;
			out.hostDrives = a_in.inGame && !a_in.dead && reason != nullptr;
			out.padLocked = out.hostDrives && recovering_ && mode_ == Mode::kMinecraft && !entering_ && !a_in.inCar && !a_in.gettingIn;
			out.why = !out.hostDrives                                    ? Why::kNone
			          : mode_ == Mode::kNiko                             ? Why::kNikoMode
			          : (entering_ || a_in.gettingIn || a_in.inCar)    ? Why::kVehicle
			          : recovering_                                      ? recoverWhy_
			                                                             : Why::kCutscene;
			out.inVehicle = out.hostDrives && a_in.inCar;
			out.resync = wasHostDrives_ && !out.hostDrives && a_in.inGame;
			wasHostDrives_ = out.hostDrives;
			return out;
		}

	private:
		// GTA took a tap and is still walking Niko to a door: no fallback, more time.
		bool Progressing() const { return accepted_ && stillT_ < kStalledAfter; }

		void StartRecovery(Why a_why)
		{
			recovering_ = true;
			recoverWhy_ = a_why;
			recoverT_ = 0.0f;
			uprightT_ = 0.0f;
		}

		Mode  mode_;
		bool  entering_ = false;
		float enterT_ = 0.0f;
		float pressT_ = 0.0f;   // seconds since puppet mode let go
		float readyT_ = 0.0f;   // seconds GTA has been reading the player's pad again
		int   taps_ = 0;
		float tapLeftT_ = 0.0f;  // the tap in progress: seconds and frames still to hold
		int   tapLeftFrames_ = 0;
		int   upFrames_ = 0;      // frames since the last tap ended
		bool  accepted_ = false;  // GTA reacted to a tap
		float stillT_ = 0.0f;     // seconds Niko has stood still since (GTA gave up on the door?)
		bool  fallbackDone_ = false;
		float fallbackAt_ = 0.0f;
		bool  wasHostDrives_ = false;
		float onFoot_ = kExitSettle;  // seconds on foot since the last car (capped)
		bool  recovering_ = false;    // GTA keeps the player until Niko stands again
		Why   recoverWhy_ = Why::kVehicle;
		float recoverT_ = 0.0f;       // seconds of this recovery (on foot)
		float uprightT_ = 0.0f;       // seconds Niko has been upright in a row
	};
}
