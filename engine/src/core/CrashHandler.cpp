#include "core/CrashHandler.h"
#include "Phisiks/Core/Log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace Phisiks::CrashHandler {
    namespace {
        std::string g_dumpDirectory;
        std::atomic<bool> g_reported{ false };

        const char* Describe(DWORD code) {
            switch (code) {
                case EXCEPTION_ACCESS_VIOLATION:     return "access violation";
                case EXCEPTION_STACK_OVERFLOW:       return "stack overflow";
                case EXCEPTION_ILLEGAL_INSTRUCTION:  return "illegal instruction";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "integer divide by zero";
                case 0xC0000374:                     return "heap corruption";
                case 0xC0000409:                     return "stack buffer overrun / fail-fast";
                default:                             return "unhandled exception";
            }
        }

        bool IsFatal(DWORD code) {
            return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
                   code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                   code == 0xC0000374 || code == 0xC0000409;
        }

        std::string DescribeAddress(DWORD64 address) {
            HANDLE process = GetCurrentProcess();
            char buffer[sizeof(SYMBOL_INFO) + 512]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line{ sizeof(line) };
            DWORD lineDisplacement = 0;
            const bool hasSymbol = SymFromAddr(process, address, &displacement, symbol);
            const bool hasLine = SymGetLineFromAddr64(process, address, &lineDisplacement, &line);

            char moduleName[MAX_PATH] = "?";
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &module)) {
                char path[MAX_PATH];
                if (GetModuleFileNameA(module, path, MAX_PATH)) {
                    const char* slash = std::strrchr(path, '\\');
                    std::snprintf(moduleName, sizeof(moduleName), "%s", slash ? slash + 1 : path);
                }
            }
            char text[1100];
            std::snprintf(text, sizeof(text), "%s!%s+0x%llx%s%s:%lu", moduleName, hasSymbol ? symbol->Name : "?",
                          static_cast<unsigned long long>(hasSymbol ? displacement : address - reinterpret_cast<DWORD64>(module)),
                          hasLine ? "  " : "", hasLine ? line.FileName : "", hasLine ? line.LineNumber : 0ul);
            return text;
        }

        void LogCurrentStack() {
            void* frames[62];
            const USHORT count = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
            Log::Error() << "Live stack (" << count << " frames):";
            for (USHORT i = 0; i < count; ++i) {
                Log::Error() << "  @" << i << " " << DescribeAddress(reinterpret_cast<DWORD64>(frames[i]));
            }
        }

        void LogStack(CONTEXT* context) {
            HANDLE process = GetCurrentProcess();
            HANDLE thread = GetCurrentThread();
            CONTEXT ctx = *context;
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = ctx.Rip;    frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
            for (int depth = 0; depth < 48; ++depth) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) break;
                char buffer[sizeof(SYMBOL_INFO) + 512]{};
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 511;
                DWORD64 displacement = 0;
                IMAGEHLP_LINE64 line{ sizeof(line) };
                DWORD lineDisplacement = 0;
                (void)symbol; (void)displacement; (void)line; (void)lineDisplacement;
                Log::Error() << "  #" << depth << " " << DescribeAddress(frame.AddrPC.Offset);
            }
        }

        void WriteDump(EXCEPTION_POINTERS* info) {
            const std::string dumpPath = g_dumpDirectory + "/crash.dmp";
            HANDLE file = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
            exceptionInfo.ThreadId = GetCurrentThreadId();
            exceptionInfo.ExceptionPointers = info;
            exceptionInfo.ClientPointers = FALSE;
            const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                              MiniDumpWithThreadInfo, &exceptionInfo, nullptr, nullptr);
            CloseHandle(file);
            Log::Error() << (ok ? "Minidump written to " : "Minidump FAILED: ") << dumpPath;
        }

        void Report(EXCEPTION_POINTERS* info) {
            if (g_reported.exchange(true)) return;
            char message[256];
            std::snprintf(message, sizeof(message), "FATAL: %s (0x%08lX) at %p, thread %lu",
                          Describe(info->ExceptionRecord->ExceptionCode), info->ExceptionRecord->ExceptionCode,
                          info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
            Log::Error() << message;
            LogStack(info->ContextRecord);
            LogCurrentStack();
            WriteDump(info);
        }

        // First chance: sees fatal errors (incl. heap corruption reports) before fail-fast paths
        // terminate the process without running the unhandled-exception filter.
        LONG WINAPI OnVectoredException(EXCEPTION_POINTERS* info) {
            if (IsFatal(info->ExceptionRecord->ExceptionCode)) Report(info);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
            Report(info);
            Log::Shutdown();
            return EXCEPTION_EXECUTE_HANDLER;
        }
    }

    void Install(const std::string& dumpDirectory) {
        g_dumpDirectory = dumpDirectory;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        AddVectoredExceptionHandler(1, OnVectoredException);
        SetUnhandledExceptionFilter(OnUnhandledException);
    }
}
#else
// POSIX: fatal signals (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT) are reported with a symbolized
// backtrace (execinfo) to stderr, <dumpDirectory>/crash.txt and the log, then re-raised with the
// default action so the OS still produces its core dump / crash report.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace Phisiks::CrashHandler {
    namespace {
        std::string g_crashPath;
        std::atomic<bool> g_reported{ false };
        char g_altStack[64 * 1024];

        const char* Describe(int sig) {
            switch (sig) {
                case SIGSEGV: return "segmentation fault";
                case SIGBUS:  return "bus error";
                case SIGFPE:  return "floating-point exception";
                case SIGILL:  return "illegal instruction";
                case SIGABRT: return "abort";
                default:      return "fatal signal";
            }
        }

        void OnSignal(int sig, siginfo_t* info, void*) {
            if (!g_reported.exchange(true)) {
                char header[256];
                const int n = std::snprintf(header, sizeof(header), "FATAL: %s (signal %d) at %p\n", Describe(sig), sig, info ? info->si_addr : nullptr);
                void* frames[64];
                const int count = backtrace(frames, 64);
                const int file = g_crashPath.empty() ? -1 : open(g_crashPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                for (int fd : { 2, file }) {
                    if (fd < 0) continue;
                    if (n > 0) { const ssize_t written = write(fd, header, size_t(n)); (void)written; }
                    backtrace_symbols_fd(frames, count, fd);
                }
                if (file >= 0) close(file);
                // best effort (not async-signal-safe; the process is going down anyway)
                Log::Error() << header << "Backtrace written to " << (g_crashPath.empty() ? std::string("stderr") : g_crashPath);
                Log::Shutdown();
            }
            std::signal(sig, SIG_DFL);
            std::raise(sig);
        }
    }

    void Install(const std::string& dumpDirectory) {
        g_crashPath = dumpDirectory.empty() ? std::string() : dumpDirectory + "/crash.txt";
        stack_t stack{};
        stack.ss_sp = g_altStack;
        stack.ss_size = sizeof(g_altStack);
        sigaltstack(&stack, nullptr);   // stack overflows still get reported
        struct sigaction action {};
        action.sa_sigaction = OnSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT }) sigaction(sig, &action, nullptr);
    }
}
#endif
