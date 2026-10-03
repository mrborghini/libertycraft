#include "Log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace lc::log
{
	namespace
	{
		std::mutex   mutex;
		FILE*        file = nullptr;
		std::string  pathUtf8;
		std::wstring gameDir;
		bool         warnedNoFile = false;

		std::string ToUtf8(const std::wstring& a_text)
		{
			if (a_text.empty()) {
				return {};
			}
			const int n = ::WideCharToMultiByte(CP_UTF8, 0, a_text.c_str(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
			if (n > 0) {
				::WideCharToMultiByte(CP_UTF8, 0, a_text.c_str(), static_cast<int>(a_text.size()), out.data(), n, nullptr, nullptr);
			}
			return out;
		}

		void Timestamp(char* a_out, std::size_t a_size)
		{
			SYSTEMTIME t;
			::GetLocalTime(&t);
			std::snprintf(a_out, a_size, "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		}
	}

	void Init(const wchar_t* a_fileName)
	{
		std::lock_guard lock(mutex);
		if (file) {
			return;
		}
		wchar_t exe[MAX_PATH]{};
		::GetModuleFileNameW(nullptr, exe, MAX_PATH);
		gameDir = exe;
		const auto slash = gameDir.find_last_of(L"\\/");
		gameDir = slash == std::wstring::npos ? L".\\" : gameDir.substr(0, slash + 1);
		const std::wstring path = gameDir + a_fileName;
		pathUtf8 = ToUtf8(path);
		file = ::_wfopen(path.c_str(), L"w");
		if (file) {
			std::setvbuf(file, nullptr, _IOLBF, 1 << 12);
		}
	}

	void Shutdown()
	{
		std::lock_guard lock(mutex);
		if (file) {
			std::fclose(file);
			file = nullptr;
		}
	}

	const char* Path()
	{
		return pathUtf8.c_str();
	}

	const wchar_t* GameDirW()
	{
		return gameDir.c_str();
	}

	void WriteV(const char* a_module, const char* a_fmt, std::va_list a_args)
	{
		char text[2048];
		std::vsnprintf(text, sizeof(text), a_fmt, a_args);
		char stamp[32];
		Timestamp(stamp, sizeof(stamp));
		char line[2200];
		std::snprintf(line, sizeof(line), "[%s] [%s] %s\n", stamp, a_module ? a_module : "lc", text);

		std::lock_guard lock(mutex);
		if (file) {
			std::fputs(line, file);
			std::fflush(file);
		} else if (!warnedNoFile) {
			warnedNoFile = true;
			::OutputDebugStringA("[LibertyCraft] log file not open; lines go to the debugger only\n");
		}
		::OutputDebugStringA("[LibertyCraft] ");
		::OutputDebugStringA(line);
	}

	void Write(const char* a_module, const char* a_fmt, ...)
	{
		std::va_list args;
		va_start(args, a_fmt);
		WriteV(a_module, a_fmt, args);
		va_end(args);
	}

	bool Limiter::Due(std::uint32_t a_everyMs)
	{
		const std::uint64_t now = ::GetTickCount64();
		if (now < next) {
			++suppressed;
			return false;
		}
		next = now + a_everyMs;
		suppressed = 0;
		return true;
	}
}
