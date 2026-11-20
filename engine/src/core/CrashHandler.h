#pragma once
#include <string>

// Installs an unhandled-exception filter that logs the exception code/address, writes a
// minidump next to the log, and lets the process terminate. Windows only.
namespace Phisiks::CrashHandler {
    void Install(const std::string& dumpDirectory);
}
