// A mission script taking the player over on foot (Missions.cpp reads the game, this decides). Pure
// logic, no IV-SDK: asi/tests/scene_test.cpp tests it on Linux.
//
// GTA IV's mission scripts play their on-foot scenes with the player's own ped: they switch his control
// off, show a camera of their own, give him tasks (walk to a mark, play an animation, follow someone)
// and wait for them, or run a minigame that reads his pad (bowling). Puppet mode fought all of that: it
// put his control back on, drew Minecraft's camera over the script's, and every move it made cleared his
// tasks, so the scripts waited forever (soft locks). While any of those signals is up GTA IV drives the
// player instead (HostDrive's Why::kScript, as in Niko mode); once all of them have been clear for
// kReleaseSeconds Minecraft takes him back (the teleport handshake first). Signals that come and go
// between the shots of one scene (a camera cut, a task ending as the next starts) don't hand him back
// and forth: the hold covers the gaps.
//
// The scene flag (a cutscene or a script's camera: what the player watches rather than plays) pauses
// Minecraft and its sounds (kSkySceneShown); it has its own shorter hold, so a cut between two cameras
// doesn't resume Minecraft for a frame.
#pragma once

namespace lc::drive
{
	enum SceneSignal : unsigned
	{
		kSigControlOff = 1u << 0,    // the player's control is off (a script's SET_PLAYER_CONTROL, or GTA's own: busted)
		kSigScriptCamera = 1u << 1,  // a script's camera is drawn (a scripted cutscene)
		kSigMinigame = 1u << 2,      // IS_MINIGAME_IN_PROGRESS (bowling, darts, pool, air hockey, ...)
		kSigScriptTask = 1u << 3,    // the player ped runs a task a script gave him (his primary task slot)
		kSigCount = 4,
	};

	inline const char* SceneSignalName(unsigned a_bit)
	{
		switch (a_bit) {
		case kSigControlOff:
			return "player control off";
		case kSigScriptCamera:
			return "the script's camera";
		case kSigMinigame:
			return "a minigame";
		case kSigScriptTask:
			return "a script task on the player";
		default:
			return "?";
		}
	}

	class SceneLogic
	{
	public:
		static constexpr float kReleaseSeconds = 0.75f;  // every signal clear this long: Minecraft takes the player back
		static constexpr float kSceneHoldSeconds = 0.4f; // a cutscene / script camera gone this long: Minecraft resumes

		struct Out
		{
			bool     scripted = false;  // GTA drives the player for a mission script (Why::kScript)
			unsigned signals = 0;       // what holds it: this frame's signals, or the last ones seen while it waits to release
			bool     started = false;   // scripted switched on this frame
			bool     ended = false;     // ...off this frame
			bool     scene = false;     // a cutscene or a script's camera is shown (held kSceneHoldSeconds)
		};

		// a_signals: this frame's SceneSignal bits. a_eligible: the player is on foot in Minecraft mode
		// (alive, in game, no vehicle): otherwise nothing is held and the logic starts over. a_shown: a
		// cutscene or a script's camera is on screen this frame (for Out::scene, eligible or not). Paused
		// frames pass a_dt 0.
		Out Step(unsigned a_signals, float a_dt, bool a_eligible, bool a_shown)
		{
			Out out;
			sceneT_ = a_shown ? kSceneHoldSeconds : (sceneT_ > a_dt ? sceneT_ - a_dt : 0.0f);
			out.scene = a_shown || sceneT_ > 0.0f;
			const bool was = scripted_;
			if (!a_eligible) {
				scripted_ = false;
				clearT_ = 0.0f;
				last_ = 0;
			} else if (a_signals != 0) {
				scripted_ = true;
				clearT_ = 0.0f;
				last_ = a_signals;
			} else if (scripted_) {
				clearT_ += a_dt;
				if (clearT_ >= kReleaseSeconds) {
					scripted_ = false;
					clearT_ = 0.0f;
				}
			}
			out.scripted = scripted_;
			out.signals = scripted_ ? (a_signals ? a_signals : last_) : 0u;
			out.started = scripted_ && !was;
			out.ended = was && !scripted_;
			return out;
		}

		void Reset()
		{
			scripted_ = false;
			clearT_ = 0.0f;
			sceneT_ = 0.0f;
			last_ = 0;
		}

		bool scripted() const { return scripted_; }

	private:
		bool     scripted_ = false;
		float    clearT_ = 0.0f;  // seconds every signal has been clear while scripted
		float    sceneT_ = 0.0f;  // the scene flag's hold left
		unsigned last_ = 0;       // the signals of the last frame that had any
	};
}
