// LibertyCraft.log in the game directory (next to GTAIV.exe), mirrored to OutputDebugStringA.
// printf-style; every line is "[HH:MM:SS.mmm] [module] text". Thread-safe; the file is flushed
// after every line (a crash takes the last lines with it otherwise).
#pragma once

#include <cstdarg>
#include <cstdint>

namespace lc::log
{
	// Opens the log (truncating). a_gameDir may be null: derived from GetModuleFileNameW(nullptr).
	void Init(const wchar_t* a_fileName = L"LibertyCraft.log");
	void Shutdown();
	// Full path of the log file (UTF-8), for other modules to say where things are.
	const char* Path();
	// The game's directory (UTF-16, with trailing backslash), as derived at Init.
	const wchar_t* GameDirW();

	void Write(const char* a_module, const char* a_fmt, ...);
	void WriteV(const char* a_module, const char* a_fmt, std::va_list a_args);

	// For per-frame lines: true at most once per a_everyMs for the given slot (a static in the caller).
	struct Limiter
	{
		std::uint64_t next{ 0 };
		std::uint32_t suppressed{ 0 };
		bool Due(std::uint32_t a_everyMs);
	};
}

// Every translation unit sets its module name: #define LC_MODULE "game" before including Log.h.
#ifndef LC_MODULE
#	define LC_MODULE "lc"
#endif
#define LC_LOG(...) ::lc::log::Write(LC_MODULE, __VA_ARGS__)
// LC_LOG_EVERY(1000, "...") logs at most once a second from this call site.
#define LC_LOG_EVERY(ms, ...)                                   \
	do {                                                        \
		static ::lc::log::Limiter lc_limiter_;                  \
		if (lc_limiter_.Due(ms)) {                              \
			::lc::log::Write(LC_MODULE, __VA_ARGS__);           \
		}                                                       \
	} while (0)
