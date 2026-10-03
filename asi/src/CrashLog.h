#pragma once

namespace lc::CrashLog
{
	// When the game crashes: the exception, where (module+offset) and an EBP-chain call stack into
	// LibertyCraft.log, and a minidump (LibertyCraft_crash.dmp in the game dir) if dbghelp is there.
	// Chains to whatever filter came before. Call at load, and again once the game is up.
	void Install();
}
