// stacksample: prints symbolized call stacks of a running process's busiest threads.
// Usage: stacksample <pid> [threadCount=3] [samples=1]
// Used to diagnose hangs (no debugger is required). Needs a PDB next to the target exe.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct ThreadInfo { DWORD id; ULONGLONG cpu; };

static ULONGLONG ThreadCpu(HANDLE thread) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(thread, &c, &e, &k, &u)) return 0;
    return (ULONGLONG(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (ULONGLONG(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: stacksample <pid> [threads] [samples]\n"); return 1; }
    const DWORD pid = std::strtoul(argv[1], nullptr, 10);
    const int threadCount = argc > 2 ? std::atoi(argv[2]) : 3;
    const int samples = argc > 3 ? std::atoi(argv[3]) : 1;

    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!process) { std::printf("OpenProcess failed (%lu)\n", GetLastError()); return 2; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(process, nullptr, TRUE)) { std::printf("SymInitialize failed (%lu)\n", GetLastError()); return 3; }

    std::vector<ThreadInfo> threads;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        threads.push_back({ te.th32ThreadID, ThreadCpu(t) });
        CloseHandle(t);
    }
    CloseHandle(snap);
    std::sort(threads.begin(), threads.end(), [](auto& a, auto& b) { return a.cpu > b.cpu; });
    if ((int)threads.size() > threadCount) threads.resize(threadCount);

    for (int s = 0; s < samples; ++s) {
        for (const ThreadInfo& info : threads) {
            HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, info.id);
            if (!t) continue;
            SuspendThread(t);
            CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
            GetThreadContext(t, &ctx);
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = ctx.Rip;    frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
            std::printf("--- sample %d thread %lu (cpu %.1fs)\n", s, info.id, info.cpu / 1e7);
            for (int depth = 0; depth < 40; ++depth) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, t, &frame, &ctx, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) break;
                char buffer[sizeof(SYMBOL_INFO) + 512]{};
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 511;
                DWORD64 disp = 0; IMAGEHLP_LINE64 line{ sizeof(line) }; DWORD ldisp = 0;
                const bool hasSym = SymFromAddr(process, frame.AddrPC.Offset, &disp, sym);
                const bool hasLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &ldisp, &line);
                std::printf("  %2d %s", depth, hasSym ? sym->Name : "?");
                if (hasLine) std::printf("  (%s:%lu)", line.FileName, line.LineNumber);
                std::printf("\n");
            }
            ResumeThread(t);
            CloseHandle(t);
        }
        if (s + 1 < samples) Sleep(250);
    }
    SymCleanup(process);
    CloseHandle(process);
    return 0;
}
