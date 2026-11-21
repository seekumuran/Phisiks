#pragma once
#include <string>

// Windows: unhandled-exception filter that logs the exception code/address and stack, writes a
// minidump (crash.dmp) next to the log, and lets the process terminate.
// POSIX: fatal-signal handler that logs a backtrace (also to crash.txt) and re-raises the signal.
namespace Phisiks::CrashHandler {
    void Install(const std::string& dumpDirectory);
}
