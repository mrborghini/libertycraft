#define LC_MODULE "crash"
#include "CrashLog.h"

#include "Log.h"

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

// x86: no RtlVirtualUnwind; walk the frame-pointer chain (good enough for the game's code and ours).
namespace lc::CrashLog
{
	namespace
	{
		LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
		std::atomic<bool>            handled{ false };

		int Walk(const CONTEXT& a_context, DWORD* a_out, int a_max)
		{
			int   n = 0;
			DWORD ebp = a_context.Ebp;
			__try {
				a_out[n++] = a_context.Eip;
				for (; n < a_max; ++n) {
					if (ebp == 0 || (ebp & 3) != 0) {
						break;
					}
					const DWORD ret = *reinterpret_cast<DWORD*>(ebp + 4);
					const DWORD next = *reinterpret_cast<DWORD*>(ebp);
					if (!ret) {
						break;
					}
					a_out[n] = ret;
					if (next <= ebp) {
						++n;
						break;
					}
					ebp = next;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return n;
		}

		std::string Where(DWORD a_address)
		{
			char    buf[MAX_PATH + 32];
			HMODULE module = nullptr;
			if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a_address), &module) || !module) {
				std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(a_address));
				return buf;
			}
			char path[MAX_PATH]{};
			::GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = std::strrchr(path, '\\');
			std::snprintf(buf, sizeof(buf), "%s+0x%lX", name ? name + 1 : path, static_cast<unsigned long>(a_address - reinterpret_cast<DWORD>(module)));
			return buf;
		}

		void WriteDump(EXCEPTION_POINTERS* a_info)
		{
			using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
			HMODULE dbghelp = ::LoadLibraryW(L"dbghelp.dll");
			auto    fn = dbghelp ? reinterpret_cast<Fn>(::GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
			if (!fn) {
				LC_LOG("CRASH: no dbghelp.dll/MiniDumpWriteDump; no minidump");
				return;
			}
			std::wstring file = lc::log::GameDirW();
			file += L"LibertyCraft_crash.dmp";
			HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle == INVALID_HANDLE_VALUE) {
				return;
			}
			MINIDUMP_EXCEPTION_INFORMATION info{ ::GetCurrentThreadId(), a_info, FALSE };
			const BOOL ok = fn(::GetCurrentProcess(), ::GetCurrentProcessId(), handle, MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &info, nullptr, nullptr);
			::CloseHandle(handle);
			LC_LOG("CRASH: minidump %s LibertyCraft_crash.dmp", ok ? "written to" : "FAILED for");
		}

		LONG WINAPI Filter(EXCEPTION_POINTERS* a_info)
		{
			if (!handled.exchange(true) && a_info && a_info->ExceptionRecord && a_info->ContextRecord) {
				const auto* record = a_info->ExceptionRecord;
				LC_LOG("CRASH: exception 0x%08lX at %s (thread %lu)", static_cast<unsigned long>(record->ExceptionCode),
					Where(reinterpret_cast<DWORD>(record->ExceptionAddress)).c_str(), ::GetCurrentThreadId());
				if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
					LC_LOG("CRASH: %s address 0x%08lX", record->ExceptionInformation[0] ? "writing" : "reading", static_cast<unsigned long>(record->ExceptionInformation[1]));
				}
				DWORD     frames[48];
				const int count = Walk(*a_info->ContextRecord, frames, 48);
				for (int i = 0; i < count; ++i) {
					LC_LOG("CRASH:   %2d %s", i, Where(frames[i]).c_str());
				}
				WriteDump(a_info);
			}
			return previous ? previous(a_info) : EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void Install()
	{
		const auto old = ::SetUnhandledExceptionFilter(Filter);
		if (old != Filter) {
			previous = old;
		}
	}
}
