// Who drives the player this frame: Minecraft (puppet mode) or GTA IV itself (Niko mode, getting
// into / sitting in a vehicle, cutscenes). Pure logic, no IV-SDK: HostDrive.cpp feeds it the
// game's state every frame and acts on the verdict; asi/tests/drive_test.cpp tests it on Linux.
//
//  * The toggle key flips between Minecraft mode and Niko mode (plain GTA IV).
//  * The vehicle key (taken from Minecraft while puppeting) hands Niko back to GTA IV and presses
//    GTA's own enter-vehicle control for a moment, so the game picks the door or carjacks like
//    normal. If Niko isn't getting in after kFallbackAfter, the caller enters the closest car by
//    other means once; after kGiveUpAfter without a car, Minecraft gets the player back.
//  * While GTA drives (hostDrives) Minecraft only follows; when that ends the caller does the
//    teleport handshake (resync) before Minecraft takes over again.
#pragma once

namespace lc::drive
{
	enum class Mode { kMinecraft, kNiko };

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
		bool        fallbackEnter = false;  // once per attempt: the press didn't take, enter another way
		bool        enterFailed = false;    // the attempt timed out without a car
		bool        modeChanged = false;
	};

	class Logic
	{
	public:
		static constexpr float kPressSeconds = 0.3f;   // how long the enter control is held
		static constexpr float kFallbackAfter = 2.0f;  // no "getting in" by then (GTA walks to the door first): other means
		static constexpr float kGiveUpAfter = 4.0f;    // still on foot by then: back to Minecraft
		static constexpr float kExitSettle = 0.5f;     // on foot this long after a car before Minecraft takes over

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
		}

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
				fallbackDone_ = false;
			}
			if (entering_) {
				enterT_ += dt;
				if (a_in.inCar || !a_in.inGame || a_in.dead) {
					entering_ = false;  // in (the vehicle path takes over), or the game took the player away
				} else if (a_in.gettingIn) {
					// GTA is walking Niko to the door / pulling the driver out: let it finish.
					if (enterT_ > kGiveUpAfter + 7.0f) {
						entering_ = false;
						out.enterFailed = true;
					}
				} else if (enterT_ >= kGiveUpAfter) {
					entering_ = false;
					out.enterFailed = true;
				} else if (!a_in.puppeting) {
					// Only once puppet mode has let go: until then our own pad zeroing would eat the press.
					out.pressEnter = pressT_ < kPressSeconds;
					pressT_ += dt;
					if (pressT_ >= kFallbackAfter && !fallbackDone_) {
						fallbackDone_ = true;
						out.fallbackEnter = true;
					}
				}
			}

			if (a_in.inCar || a_in.gettingIn) {
				onFoot_ = 0.0f;
			} else {
				onFoot_ = onFoot_ < kExitSettle ? onFoot_ + dt : onFoot_;
			}

			const char* reason = mode_ == Mode::kNiko ? "Niko mode (toggle key)"
			                     : entering_          ? "entering a vehicle (vehicle key)"
			                     : a_in.gettingIn     ? "the game is putting the player in a vehicle"
			                     : a_in.inCar         ? "player in a vehicle"
			                     : onFoot_ < kExitSettle ? "just left a vehicle"
			                     : a_in.cutscene      ? "cutscene"
			                                          : nullptr;
			out.blocker = reason;
			out.hostDrives = a_in.inGame && !a_in.dead && reason != nullptr;
			out.inVehicle = out.hostDrives && a_in.inCar;
			out.resync = wasHostDrives_ && !out.hostDrives && a_in.inGame;
			wasHostDrives_ = out.hostDrives;
			return out;
		}

	private:
		Mode  mode_;
		bool  entering_ = false;
		float enterT_ = 0.0f;
		float pressT_ = 0.0f;
		bool  fallbackDone_ = false;
		bool  wasHostDrives_ = false;
		float onFoot_ = kExitSettle;  // seconds on foot since the last car (capped)
	};
}
