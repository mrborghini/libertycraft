// LibertyCraft.asi: Minecraft inside GTA IV (1.0.7.0 / 1.0.8.0), loaded by Ultimate ASI Loader
// from <gamedir>/plugins/. The single translation unit that includes IV-SDK (its headers define
// non-inline globals); the SDK-dependent sources are compiled here, unity-style.
#include "IVSDK.cpp"  // DllMain, hooks, natives, game classes (patched copy, see CMakeLists.txt)

#define LC_IVSDK_INCLUDED 1
#undef LC_MODULE
#define LC_MODULE "main"
#include "Config.h"
#include "CrashLog.h"
#include "Game.h"
#include "Input.h"
#include "Link.h"
#include "Log.h"
#include "Render.h"

#include "Game.cpp"
#include "HostDrive.cpp"
#include "Input.cpp"
#include "Collision.cpp"
#include "Render.cpp"
#include "Overlay.cpp"
#include "Combat.cpp"

#undef LC_MODULE
#define LC_MODULE "main"

#ifndef LC_VERSION_STRING
#	define LC_VERSION_STRING "dev"
#endif

namespace
{
	// Runs while the DLL's globals are constructed (after IV-SDK's, which are defined above),
	// before IV-SDK's DllMain: log first, so even an unsupported game version leaves a trace.
	struct Boot
	{
		Boot()
		{
			lc::log::Init();
			LC_LOG("LibertyCraft %s (%s %s) loading; log %s", LC_VERSION_STRING, __DATE__, __TIME__, lc::log::Path());
			const auto version = AddressSetter::GetVersionFromEXE();
			if (version == 1070 || version == 1080) {
				LC_LOG("GTAIV.exe version %u.%u.%u.%u: supported", version / 1000, version / 100 % 10, version / 10 % 10, version % 10);
			} else {
				LC_LOG("**************************************************************************");
				LC_LOG("ERROR: unsupported GTAIV.exe version (%u; IV-SDK knows 1070 and 1080).", version);
				LC_LOG("ERROR: LibertyCraft stays INACTIVE: no hooks, no link. Install GTA IV 1.0.8.0.");
				LC_LOG("**************************************************************************");
			}
		}
		~Boot()
		{
			// DLL_PROCESS_DETACH: put the window procedure back. No logging and no lock here: at
			// process exit the other threads are already gone, maybe holding the log's lock (the
			// log is flushed after every line anyway).
			lc::Input::Uninstall(true);
		}
	} boot;
}

// IV-SDK calls this from DllMain once it has detected a supported version and placed its hooks.
void plugin::gameStartupEvent()
{
	lc::CrashLog::Install();
	wchar_t path[MAX_PATH]{};
	::GetModuleFileNameW(plugin::GetCurrentModule(), path, MAX_PATH);
	std::wstring dir = path;
	const auto   slash = dir.find_last_of(L"\\/");
	dir = slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1);
	lc::Config::Get().Load(dir.c_str());

	LC_LOG("game version %s detected (base 0x%08X); hooks placed: processScripts, camera, pad, drawing, ingameStartup",
		plugin::gameVer == plugin::VERSION_1080 ? "1.0.8.0" : "1.0.7.0", AddressSetter::gBaseAddress);
	plugin::processScriptsEvent::Add(&lc::Game::Tick);
	plugin::processCameraEvent::Add(&lc::Game::Camera);
	plugin::processPadEvent::Add(&lc::Input::Pad);
	plugin::drawingEvent::Add(&lc::Render::Draw);
	plugin::ingameStartupEvent::Add(&lc::Game::OnIngameStartup);
	// The heartbeat (and the link itself) shouldn't wait for the first game frame: Minecraft sees
	// the host through the loading screens too.
	lc::Link::Get().StartHeartbeatThread();
}
