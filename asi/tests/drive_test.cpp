// DriveLogic.h: who drives the player (Minecraft or GTA IV) through toggles, the vehicle key,
// vehicles entered by scripts, and the hand-back after leaving a car.
#include "DriveLogic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

	using lc::drive::Input;
	using lc::drive::Logic;
	using lc::drive::Mode;
	using lc::drive::Why;
	using lc::drive::Output;

	constexpr float kDt = 1.0f / 60.0f;

	Input OnFoot(bool a_puppeting)
	{
		Input in;
		in.dt = kDt;
		in.inGame = true;
		in.puppeting = a_puppeting;
		return in;
	}

	// Runs frames with `in` for a_seconds; returns the last output, counting presses/fallbacks.
	struct Run
	{
		Output last;
		int    pressFrames = 0, fallbacks = 0, failed = 0, resyncs = 0, taps = 0, edges = 0, accepted = 0;
		bool   wasPressed = false;
	};
	Run Frames(Logic& a_logic, Input a_in, float a_seconds)
	{
		Run r;
		for (float t = 0.0f; t < a_seconds; t += a_in.dt) {
			r.last = a_logic.Step(a_in);
			r.pressFrames += r.last.pressEnter;
			r.edges += r.last.pressEnter && !r.wasPressed;  // what GTA sees: a press edge
			r.wasPressed = r.last.pressEnter;
			r.taps += r.last.tapStarted > 0;
			r.accepted += r.last.accepted;
			r.fallbacks += r.last.fallbackEnter;
			r.failed += r.last.enterFailed;
			r.resyncs += r.last.resync;
		}
		return r;
	}

	// The vehicle key while puppeting; returns the input for the frames after it (puppet let go,
	// GTA reading the player's pad again).
	Input PressVehicleKey(Logic& a_logic)
	{
		auto in = OnFoot(true);
		in.vehicleActions = 1;
		a_logic.Step(in);
		in.vehicleActions = 0;
		in.puppeting = false;
		in.controlReady = true;
		return in;
	}

	void TestDefaultMinecraftMode()
	{
		Logic logic(true);
		auto  out = logic.Step(OnFoot(false));
		CHECK(logic.mode() == Mode::kMinecraft);
		CHECK(out.blocker == nullptr);
		CHECK(!out.hostDrives && !out.inVehicle && !out.resync);
	}

	void TestToggle()
	{
		Logic logic(true);
		auto  in = OnFoot(true);
		in.toggles = 1;
		auto out = logic.Step(in);
		CHECK(out.modeChanged);
		CHECK(logic.mode() == Mode::kNiko);
		CHECK(out.blocker != nullptr && std::strstr(out.blocker, "Niko"));
		CHECK(out.hostDrives && !out.inVehicle);
		// Two presses in one frame cancel out.
		in.toggles = 2;
		out = logic.Step(in);
		CHECK(!out.modeChanged && logic.mode() == Mode::kNiko);
		// Back: GTA lets go, so a teleport handshake first.
		in.toggles = 1;
		in.puppeting = false;
		out = logic.Step(in);
		CHECK(logic.mode() == Mode::kMinecraft);
		CHECK(!out.hostDrives && out.blocker == nullptr && out.resync);
		in.toggles = 0;
		out = logic.Step(in);
		CHECK(!out.resync);  // only once
	}

	void TestStartsInNikoMode()
	{
		Logic logic(false);
		auto  out = logic.Step(OnFoot(false));
		CHECK(logic.mode() == Mode::kNiko && out.hostDrives);
	}

	void TestVehicleKeyEntersCar()
	{
		Logic logic(true);
		auto  in = OnFoot(true);
		in.vehicleActions = 1;
		auto out = logic.Step(in);
		CHECK(logic.entering());
		CHECK(out.blocker != nullptr && out.hostDrives);  // Game leaves puppet mode now
		CHECK(!out.pressEnter);                           // ...but our pad zeroing is still on this frame
		in.vehicleActions = 0;
		in.puppeting = false;
		in.controlReady = true;
		// GTA takes the first tap: Niko walks to the door.
		auto r = Frames(logic, in, 0.2f);
		CHECK(r.taps == 1 && r.edges == 1 && !r.last.pressEnter);
		in.moving = true;
		r = Frames(logic, in, 1.0f);
		CHECK(r.accepted == 1 && r.taps == 0 && r.pressFrames == 0);  // no more taps once Niko is on his way
		in.moving = false;
		in.gettingIn = true;
		r = Frames(logic, in, Logic::kGiveUpAfter + 1.0f);  // longer than the give-up time: getting in is progress
		CHECK(r.failed == 0 && r.fallbacks == 0);
		CHECK(r.last.hostDrives && !r.last.inVehicle);
		in.gettingIn = false;
		in.inCar = true;
		out = logic.Step(in);
		CHECK(!logic.entering());
		CHECK(out.hostDrives && out.inVehicle && !out.resync);
	}

	void TestTapsRepeatUntilGtaReacts()
	{
		Logic logic(true);
		auto  in = PressVehicleKey(logic);
		// GTA ignores the taps: kMaxTaps separate press edges, each held about kTapSeconds.
		auto r = Frames(logic, in, Logic::kFallbackAfter - 0.1f);
		CHECK(r.taps == Logic::kMaxTaps && r.edges == Logic::kMaxTaps);
		const int perTap = static_cast<int>(Logic::kTapSeconds / kDt + 0.5f);
		CHECK(r.pressFrames >= Logic::kMaxTaps * (perTap - 1) && r.pressFrames <= Logic::kMaxTaps * (perTap + 1));
		CHECK(r.accepted == 0 && r.fallbacks == 0);
		r = Frames(logic, in, 0.2f);
		CHECK(r.fallbacks == 1 && r.taps == 0);
	}

	void TestWalkToDoorSkipsFallbackUntilStalled()
	{
		Logic logic(true);
		auto  in = PressVehicleKey(logic);
		auto  r = Frames(logic, in, 0.2f);
		CHECK(r.taps == 1);
		in.moving = true;  // GTA walks Niko to a far door: past the fallback and give-up times
		r = Frames(logic, in, Logic::kGiveUpAfter + 1.0f);
		CHECK(r.accepted == 1 && r.fallbacks == 0 && r.failed == 0 && r.taps == 0 && logic.entering());
		in.moving = false;  // GTA gave up on the door
		r = Frames(logic, in, Logic::kStalledAfter + 0.1f);
		CHECK(r.fallbacks == 1 && r.failed == 0);  // the other means get a second
		r = Frames(logic, in, 1.0f);
		CHECK(r.failed == 1 && !logic.entering());
	}

	void TestTapsWaitForControl()
	{
		Logic logic(true);
		auto  in = PressVehicleKey(logic);
		in.controlReady = false;  // GTA still reads the empty pad (player control not back yet)
		auto r = Frames(logic, in, 0.5f);
		CHECK(r.pressFrames == 0 && r.taps == 0);
		in.controlReady = true;
		const int readyFrames = static_cast<int>(Logic::kTapDelay / kDt + 0.999f);  // the first tap's frame
		for (int k = 1; k < readyFrames; ++k) {
			CHECK(logic.Step(in).tapStarted == 0);
		}
		const auto out = logic.Step(in);
		CHECK(out.tapStarted == 1 && out.pressEnter);
	}

	void TestShortTapAtLowFrameRate()
	{
		Logic logic(true);
		auto  in = PressVehicleKey(logic);
		in.dt = 0.2f;  // 5 fps: one frame is longer than a tap, the press still lasts kTapFrames frames
		auto out = logic.Step(in);
		CHECK(out.tapStarted == 1 && out.pressEnter);
		out = logic.Step(in);
		CHECK(out.pressEnter && out.tapStarted == 0);
		// The next tap is due right away at 5 fps, but GTA needs released frames between taps.
		for (int k = 0; k < Logic::kTapFrames; ++k) {
			CHECK(!logic.Step(in).pressEnter);
		}
		out = logic.Step(in);
		CHECK(out.tapStarted == 2 && out.pressEnter);
	}

	void TestNoTapsWhilePaused()
	{
		Logic logic(true);
		auto  in = PressVehicleKey(logic);
		in.paused = true;
		const auto r = Frames(logic, in, 1.0f);
		CHECK(r.pressFrames == 0);
	}

	void TestMovingBeforeAnyTapIsNotAcceptance()
	{
		Logic logic(true);
		auto  in = PressVehicleKey(logic);
		in.moving = true;  // e.g. still sliding when puppet mode let go: not GTA taking a press
		const auto r = Frames(logic, in, 0.3f);
		CHECK(r.taps >= 1);
	}

	void TestEnterFallbackThenGiveUp()
	{
		Logic logic(true);
		auto  in = OnFoot(true);
		in.vehicleActions = 1;
		logic.Step(in);
		in.vehicleActions = 0;
		in.puppeting = false;
		in.controlReady = true;
		auto r = Frames(logic, in, Logic::kFallbackAfter + 0.2f);
		CHECK(r.fallbacks == 1);
		r = Frames(logic, in, Logic::kGiveUpAfter - Logic::kFallbackAfter);
		CHECK(r.fallbacks == 0);   // only once
		CHECK(r.failed == 1);      // nothing after kGiveUpAfter
		CHECK(!logic.entering());
		CHECK(!r.last.hostDrives && r.last.blocker == nullptr);
		CHECK(r.resyncs == 1);     // Minecraft gets the player back through a teleport
	}

	void TestVehicleKeyIgnoredInNikoModeOrInCar()
	{
		Logic logic(false);
		auto  in = OnFoot(false);
		in.vehicleActions = 1;
		logic.Step(in);
		CHECK(!logic.entering());
		Logic mc(true);
		in.inCar = true;
		mc.Step(in);
		CHECK(!mc.entering());
	}

	void TestScriptPutsPlayerInCar()
	{
		Logic logic(true);
		auto  in = OnFoot(true);
		in.gettingIn = true;  // a mission script tasks Niko into a car
		auto out = logic.Step(in);
		CHECK(out.hostDrives && out.blocker && std::strstr(out.blocker, "putting"));
		in.gettingIn = false;
		in.inCar = true;
		in.puppeting = false;
		out = logic.Step(in);
		CHECK(out.inVehicle);
	}

	void TestExitSettlesBeforeHandBack()
	{
		Logic logic(true);
		Input in = OnFoot(false);
		in.inCar = true;
		auto r = Frames(logic, in, 1.0f);
		CHECK(r.last.inVehicle);
		in.inCar = false;  // GTA's F: out of the car
		auto out = logic.Step(in);
		CHECK(out.hostDrives && !out.inVehicle && !out.resync);
		r = Frames(logic, in, Logic::kExitSettle + 0.1f);
		CHECK(r.resyncs == 1);
		CHECK(!r.last.hostDrives && r.last.blocker == nullptr);
	}

	void TestInCarInNikoModeThenToggle()
	{
		Logic logic(true);
		Input in = OnFoot(false);
		in.inCar = true;
		in.toggles = 1;
		auto out = logic.Step(in);
		CHECK(logic.mode() == Mode::kNiko && out.inVehicle);
		CHECK(out.why == Why::kNikoMode);
		in.toggles = 1;  // back to Minecraft mode while still driving: GTA keeps the wheel
		out = logic.Step(in);
		CHECK(logic.mode() == Mode::kMinecraft && out.inVehicle && !out.resync);
		CHECK(out.why == Why::kVehicle);
		in.toggles = 0;
		in.inCar = false;  // just out: still the vehicle's, until on foot for kExitSettle
		out = logic.Step(in);
		CHECK(out.hostDrives && out.why == Why::kVehicle);
	}

	void TestCutsceneAndLoading()
	{
		Logic logic(true);
		Input in = OnFoot(true);
		in.cutscene = true;
		auto out = logic.Step(in);
		CHECK(out.hostDrives && std::strstr(out.blocker, "cutscene"));
		CHECK(out.why == Why::kCutscene);
		in.inGame = false;  // loading: nobody follows, no resync (Game teleports after loads anyway)
		out = logic.Step(in);
		CHECK(!out.hostDrives && !out.resync);
		CHECK(out.why == Why::kNone);
		in.inGame = true;
		in.cutscene = false;
		out = logic.Step(in);
		CHECK(!out.hostDrives && !out.resync);
	}

	void TestPausedTimersStandStill()
	{
		Logic logic(true);
		auto  in = OnFoot(true);
		in.vehicleActions = 1;
		logic.Step(in);
		in.vehicleActions = 0;
		in.puppeting = false;
		in.paused = true;
		auto r = Frames(logic, in, 5.0f);
		CHECK(r.failed == 0 && logic.entering());
		CHECK(r.last.hostDrives);
	}

	void TestDeathDropsAttempt()
	{
		Logic logic(true);
		auto  in = OnFoot(true);
		in.vehicleActions = 1;
		logic.Step(in);
		in.dead = true;
		auto out = logic.Step(in);
		CHECK(!logic.entering() && !out.hostDrives);
	}

	// Bailing out of a moving car: Niko rolls and tumbles (ragdoll), gets up; GTA keeps him until he
	// has stood still for kUprightSettle, however long the tumble takes.
	void TestBailOutWaitsUntilStanding()
	{
		Logic logic(true);
		Input in = OnFoot(false);
		in.inCar = true;
		logic.Step(in);
		in.inCar = false;
		in.upright = false;
		in.ragdoll = true;
		auto r = Frames(logic, in, 3.0f);  // tumbling, far past kExitSettle
		CHECK(r.last.hostDrives && r.last.why == Why::kVehicle && r.resyncs == 0);
		CHECK(r.last.padLocked);  // GTA animates him: the player's input stays out of GTA
		CHECK(std::strstr(r.last.blocker, "until Niko stands"));
		in.ragdoll = false;  // getting up: not ragdolled any more, not upright yet either
		r = Frames(logic, in, 1.0f);
		CHECK(r.last.hostDrives && r.resyncs == 0);
		in.upright = true;
		r = Frames(logic, in, Logic::kUprightSettle - 0.1f);
		CHECK(r.last.hostDrives && r.resyncs == 0);
		r = Frames(logic, in, 0.2f);
		CHECK(!r.last.hostDrives && r.resyncs == 1 && !logic.recovering() && !r.last.padLocked);
		// An upright moment in the middle of a tumble doesn't count: it has to last.
		Logic l2(true);
		in = OnFoot(false);
		in.inCar = true;
		l2.Step(in);
		in.inCar = false;
		for (int i = 0; i < 10; ++i) {
			in.upright = (i % 2) == 0;
			r = Frames(l2, in, 0.3f);
			CHECK(r.last.hostDrives);
		}
	}

	// A car runs the puppeted player over: GTA takes him (kRagdoll) until he is back on his feet.
	void TestKnockdownUntilBackUp()
	{
		Logic logic(true);
		Input in = OnFoot(true);
		CHECK(!logic.Step(in).hostDrives);
		in.knockdowns = 1;
		auto out = logic.Step(in);
		CHECK(out.hostDrives && out.why == Why::kRagdoll && std::strstr(out.blocker, "knocked over"));
		CHECK(out.padLocked);
		in.knockdowns = 0;
		in.puppeting = false;
		in.ragdoll = true;
		in.upright = false;
		auto r = Frames(logic, in, 2.0f);
		CHECK(r.last.hostDrives && r.last.why == Why::kRagdoll);
		in.ragdoll = false;
		in.upright = true;
		r = Frames(logic, in, Logic::kUprightSettle + 0.05f);
		CHECK(!r.last.hostDrives && r.resyncs == 1);
		// Never stands up (stuck somewhere): kRecoverCap hands him back anyway.
		in.knockdowns = 1;
		in.upright = false;
		out = logic.Step(in);
		in.knockdowns = 0;
		r = Frames(logic, in, Logic::kRecoverCap + 0.1f);
		CHECK(!r.last.hostDrives && r.resyncs == 1);
		// GTA ragdolls Niko by itself in Minecraft mode (a blast): the same.
		Logic l3(true);
		in = OnFoot(true);
		in.ragdoll = true;
		in.upright = false;
		out = l3.Step(in);
		CHECK(out.hostDrives && out.why == Why::kRagdoll);
		// In Niko mode GTA plays anyway: no recovery to wait for.
		Logic l4(false);
		in = OnFoot(false);
		in.knockdowns = 1;
		out = l4.Step(in);
		CHECK(out.why == Why::kNikoMode && !l4.recovering() && !out.padLocked);
		// In a vehicle (and getting in) GTA gets the player's real input.
		Logic l5(true);
		in = OnFoot(false);
		in.inCar = true;
		out = l5.Step(in);
		CHECK(out.hostDrives && !out.padLocked);
		in.inCar = false;
		in.gettingIn = true;
		out = l5.Step(in);
		CHECK(out.hostDrives && !out.padLocked);
	}
}

int main()
{
	TestDefaultMinecraftMode();
	TestToggle();
	TestStartsInNikoMode();
	TestVehicleKeyEntersCar();
	TestTapsRepeatUntilGtaReacts();
	TestTapsWaitForControl();
	TestWalkToDoorSkipsFallbackUntilStalled();
	TestShortTapAtLowFrameRate();
	TestNoTapsWhilePaused();
	TestMovingBeforeAnyTapIsNotAcceptance();
	TestEnterFallbackThenGiveUp();
	TestVehicleKeyIgnoredInNikoModeOrInCar();
	TestScriptPutsPlayerInCar();
	TestExitSettlesBeforeHandBack();
	TestInCarInNikoModeThenToggle();
	TestCutsceneAndLoading();
	TestPausedTimersStandStill();
	TestDeathDropsAttempt();
	TestBailOutWaitsUntilStanding();
	TestKnockdownUntilBackUp();
	if (failures) {
		std::fprintf(stderr, "drive_test: %d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("drive_test: all passed\n");
	return EXIT_SUCCESS;
}
