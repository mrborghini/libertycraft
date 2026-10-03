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
		int    pressFrames = 0, fallbacks = 0, failed = 0, resyncs = 0;
	};
	Run Frames(Logic& a_logic, Input a_in, float a_seconds)
	{
		Run r;
		for (float t = 0.0f; t < a_seconds; t += a_in.dt) {
			r.last = a_logic.Step(a_in);
			r.pressFrames += r.last.pressEnter;
			r.fallbacks += r.last.fallbackEnter;
			r.failed += r.last.enterFailed;
			r.resyncs += r.last.resync;
		}
		return r;
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
		// GTA takes the press: Niko walks to the door.
		auto r = Frames(logic, in, 0.2f);
		CHECK(r.pressFrames > 0 && r.last.pressEnter);
		in.gettingIn = true;
		r = Frames(logic, in, 4.0f);  // longer than the give-up time: getting in is progress
		CHECK(r.failed == 0 && r.fallbacks == 0);
		CHECK(r.last.hostDrives && !r.last.inVehicle);
		in.gettingIn = false;
		in.inCar = true;
		out = logic.Step(in);
		CHECK(!logic.entering());
		CHECK(out.hostDrives && out.inVehicle && !out.resync);
	}

	void TestPressHeldBriefly()
	{
		Logic logic(true);
		auto  in = OnFoot(true);
		in.vehicleActions = 1;
		logic.Step(in);
		in.vehicleActions = 0;
		in.puppeting = false;
		const auto r = Frames(logic, in, 0.9f);
		const int  expected = static_cast<int>(Logic::kPressSeconds / kDt);
		CHECK(r.pressFrames >= expected - 1 && r.pressFrames <= expected + 1);
		CHECK(!r.last.pressEnter);
	}

	void TestEnterFallbackThenGiveUp()
	{
		Logic logic(true);
		auto  in = OnFoot(true);
		in.vehicleActions = 1;
		logic.Step(in);
		in.vehicleActions = 0;
		in.puppeting = false;
		auto r = Frames(logic, in, 1.2f);
		CHECK(r.fallbacks == 1);
		r = Frames(logic, in, 2.0f);
		CHECK(r.fallbacks == 0);   // only once
		CHECK(r.failed == 1);      // nothing after 3 s
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
		in.toggles = 1;  // back to Minecraft mode while still driving: GTA keeps the wheel
		out = logic.Step(in);
		CHECK(logic.mode() == Mode::kMinecraft && out.inVehicle && !out.resync);
	}

	void TestCutsceneAndLoading()
	{
		Logic logic(true);
		Input in = OnFoot(true);
		in.cutscene = true;
		auto out = logic.Step(in);
		CHECK(out.hostDrives && std::strstr(out.blocker, "cutscene"));
		in.inGame = false;  // loading: nobody follows, no resync (Game teleports after loads anyway)
		out = logic.Step(in);
		CHECK(!out.hostDrives && !out.resync);
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
}

int main()
{
	TestDefaultMinecraftMode();
	TestToggle();
	TestStartsInNikoMode();
	TestVehicleKeyEntersCar();
	TestPressHeldBriefly();
	TestEnterFallbackThenGiveUp();
	TestVehicleKeyIgnoredInNikoModeOrInCar();
	TestScriptPutsPlayerInCar();
	TestExitSettlesBeforeHandBack();
	TestInCarInNikoModeThenToggle();
	TestCutsceneAndLoading();
	TestPausedTimersStandStill();
	TestDeathDropsAttempt();
	if (failures) {
		std::fprintf(stderr, "drive_test: %d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("drive_test: all passed\n");
	return EXIT_SUCCESS;
}
