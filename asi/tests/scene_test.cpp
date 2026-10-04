// drive/SceneLogic.h: when a mission script has the player (GTA drives him), and DriveLogic.h's
// Why::kScript hand-over around it.
#include "DriveLogic.h"
#include "drive/SceneLogic.h"

#include <cstdio>

namespace
{
	int failures = 0;

#define CHECK(cond)                                                                       \
	do {                                                                                  \
		if (!(cond)) {                                                                    \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                                   \
		}                                                                                 \
	} while (0)

	using lc::drive::SceneLogic;
	constexpr float kDt = 1.0f / 60.0f;

	// Runs a_seconds of frames with the same signals; returns the last output and counts starts/ends.
	SceneLogic::Out Run(SceneLogic& a_l, unsigned a_signals, float a_seconds, bool a_eligible = true, bool a_shown = false, int* a_starts = nullptr,
		int* a_ends = nullptr)
	{
		SceneLogic::Out out;
		for (float t = 0.0f; t < a_seconds - 1e-4f; t += kDt) {
			out = a_l.Step(a_signals, kDt, a_eligible, a_shown);
			if (a_starts && out.started) {
				++*a_starts;
			}
			if (a_ends && out.ended) {
				++*a_ends;
			}
		}
		return out;
	}

	void TakesOverAtOnceAndLetsGoAfterTheHold()
	{
		SceneLogic l;
		auto out = l.Step(lc::drive::kSigScriptTask, kDt, true, false);
		CHECK(out.scripted && out.started);
		CHECK(out.signals == lc::drive::kSigScriptTask);
		out = Run(l, lc::drive::kSigScriptTask, 2.0f);
		CHECK(out.scripted && !out.started);
		// Clear: held for kReleaseSeconds, still naming what held it.
		out = Run(l, 0, SceneLogic::kReleaseSeconds - 0.1f);
		CHECK(out.scripted);
		CHECK(out.signals == lc::drive::kSigScriptTask);
		int ends = 0;
		out = Run(l, 0, 0.2f, true, false, nullptr, &ends);
		CHECK(!out.scripted);
		CHECK(ends == 1);
		CHECK(out.signals == 0);
	}

	void GapsBetweenShotsDontHandBack()
	{
		// A scene: camera, a 0.3 s gap (one task ends, the next starts), control off, gap, minigame.
		SceneLogic l;
		int starts = 0, ends = 0;
		Run(l, lc::drive::kSigScriptCamera, 1.0f, true, true, &starts, &ends);
		Run(l, 0, 0.3f, true, false, &starts, &ends);
		Run(l, lc::drive::kSigControlOff | lc::drive::kSigScriptTask, 2.0f, true, false, &starts, &ends);
		Run(l, 0, 0.5f, true, false, &starts, &ends);
		Run(l, lc::drive::kSigMinigame, 5.0f, true, false, &starts, &ends);
		CHECK(starts == 1);
		CHECK(ends == 0);
		Run(l, 0, 1.0f, true, false, &starts, &ends);
		CHECK(ends == 1);
	}

	void IneligibleForgetsEverything()
	{
		SceneLogic l;
		Run(l, lc::drive::kSigControlOff, 1.0f);
		CHECK(l.scripted());
		// Into a vehicle (or Niko mode): nothing held, no release timer left over.
		auto out = l.Step(lc::drive::kSigControlOff, kDt, false, false);
		CHECK(!out.scripted && out.ended);
		out = Run(l, lc::drive::kSigControlOff, 2.0f, false);
		CHECK(!out.scripted);
		// Back on foot with the control still off: at once again.
		out = l.Step(lc::drive::kSigControlOff, kDt, true, false);
		CHECK(out.scripted && out.started);
	}

	void PausedFramesHold()
	{
		SceneLogic l;
		Run(l, lc::drive::kSigScriptTask, 0.5f);
		for (int i = 0; i < 600; ++i) {
			const auto out = l.Step(0, 0.0f, true, false);  // the pause menu: no time passes
			CHECK(out.scripted);
		}
		CHECK(l.scripted());
	}

	void SceneFlagHasItsOwnShortHold()
	{
		SceneLogic l;
		auto out = l.Step(0, kDt, true, true);
		CHECK(out.scene);
		// A cut between two cameras (a few frames without either) keeps the scene.
		out = Run(l, 0, 0.2f, true, false);
		CHECK(out.scene);
		out = Run(l, 0, SceneLogic::kSceneHoldSeconds, true, false);
		CHECK(!out.scene);
		// Shown counts whether the player is eligible or not (a cutscene in a car).
		out = l.Step(0, kDt, false, true);
		CHECK(out.scene && !out.scripted);
	}

	// DriveLogic: a mission script's scene hands the player to GTA (Why::kScript) with GTA's pad, and back
	// with the teleport handshake.
	void DriveLogicScriptReason()
	{
		using namespace lc::drive;
		Logic logic(true);
		Input in;
		in.dt = kDt;
		in.inGame = true;
		in.puppeting = true;
		auto out = logic.Step(in);
		CHECK(!out.hostDrives);
		in.scripted = true;
		out = logic.Step(in);
		CHECK(out.hostDrives);
		CHECK(out.why == Why::kScript);
		CHECK(!out.padLocked);
		CHECK(!out.inVehicle);
		in.puppeting = false;
		out = logic.Step(in);
		CHECK(out.why == Why::kScript);
		// A cutscene during the scene: the cutscene's reason.
		in.cutscene = true;
		out = logic.Step(in);
		CHECK(out.why == Why::kCutscene);
		in.cutscene = false;
		// Into a vehicle: the vehicle's.
		in.inCar = true;
		out = logic.Step(in);
		CHECK(out.why == Why::kVehicle);
		in.inCar = false;
		in.scripted = false;
		// Out of the car: getting back up first (just left a vehicle), then Minecraft with the handshake.
		bool resync = false;
		for (int i = 0; i < 120; ++i) {
			in.upright = true;
			out = logic.Step(in);
			resync = resync || out.resync;
		}
		CHECK(!out.hostDrives);
		CHECK(resync);
		// Niko mode wins over a scene.
		in.toggles = 1;
		in.scripted = true;
		out = logic.Step(in);
		CHECK(out.why == Why::kNikoMode);
	}

	void DriveLogicScriptEndsWithResync()
	{
		using namespace lc::drive;
		Logic logic(true);
		Input in;
		in.dt = kDt;
		in.inGame = true;
		in.scripted = true;
		auto out = logic.Step(in);
		CHECK(out.hostDrives && out.why == Why::kScript);
		in.scripted = false;
		out = logic.Step(in);
		CHECK(!out.hostDrives);
		CHECK(out.resync);
		CHECK(out.blocker == nullptr);
	}
}

int main()
{
	TakesOverAtOnceAndLetsGoAfterTheHold();
	GapsBetweenShotsDontHandBack();
	IneligibleForgetsEverything();
	PausedFramesHold();
	SceneFlagHasItsOwnShortHold();
	DriveLogicScriptReason();
	DriveLogicScriptEndsWithResync();
	if (failures) {
		std::fprintf(stderr, "scene_test: %d check(s) failed\n", failures);
		return 1;
	}
	std::printf("scene_test: all passed\n");
	return 0;
}
