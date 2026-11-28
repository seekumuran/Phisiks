// PAI stdio transport: newline-delimited JSON-RPC on the process's stdin / stdout.
// A reader thread queues lines (bounded); the editor's frame hook handles them on the main thread
// (where the scene lives) and writes one response line each. stdout carries only responses: the
// console log moves to stderr while PAI runs.
#include "Pai.h"

#include "Editor.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Framework/GameContext.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Pai {
    void FrameStep(Phisiks::GameContext& ctx);   // Pai.cpp

    namespace {
        struct Server {
            Dispatcher dispatcher;
            Limits limits;
            std::mutex mutex;          // queue + stdout
            std::deque<std::string> queue;
            std::atomic<bool> running{ false };
            std::atomic<bool> inputClosed{ false };
            std::thread reader;
            Server(const Permissions& p, const Limits& l) : dispatcher(p, l), limits(l) {}
        };
        std::unique_ptr<Server> g_server;
        Stats g_lastStats;

        void WriteLine(Server& s, const std::string& line) {
            if (line.empty()) return;
            std::lock_guard<std::mutex> lock(s.mutex);
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }

        void ReadLoop(Server* s) {
            std::string line;
            while (s->running && std::getline(std::cin, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.find_first_not_of(" \t") == std::string::npos) continue;
                bool full = false;
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (s->queue.size() >= s->limits.maxQueued) full = true;
                    else s->queue.push_back(line);
                }
                if (full) WriteLine(*s, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32004,\"message\":\"request queue full (" + std::to_string(s->limits.maxQueued) + "): wait for responses\"}}");
            }
            s->inputClosed = true;
            Phisiks::Log::Info() << "PAI: input closed (" << (std::cin.eof() ? "end of stream" : "read error") << "); the editor keeps running";
        }

        void Pump(Phisiks::GameContext& ctx) {
            Server* s = g_server.get();
            if (!s) return;
            FrameStep(ctx);
            for (int i = 0; i < s->limits.maxPerFrame; ++i) {
                std::string request;
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (s->queue.empty()) break;
                    request = std::move(s->queue.front());
                    s->queue.pop_front();
                }
                WriteLine(*s, s->dispatcher.Handle(ctx, request));
            }
            g_lastStats = s->dispatcher.GetStats();
        }
    }

    bool StartStdio(const Permissions& permissions, const Limits& limits) {
        if (g_server) return false;
        Phisiks::Log::SetConsoleToStderr(true);
        g_server = std::make_unique<Server>(permissions, limits);
        g_server->running = true;
        g_server->reader = std::thread(ReadLoop, g_server.get());
        g_server->reader.detach();   // blocked in getline until the client closes stdin or the process exits
        Editor::AddFrameHook(Pump);
        Phisiks::Log::Info() << "PAI: listening on stdio (" << PROTOCOL << " " << PROTOCOL_VERSION << "; permissions: read"
                             << (permissions.write ? ", write" : "") << (permissions.simulate ? ", simulate" : "") << ")";
        return true;
    }

    void Stop() {
        if (!g_server) return;
        g_server->running = false;
        g_lastStats = g_server->dispatcher.GetStats();
        g_server.release();   // the detached reader may still reference it until the process ends
        Phisiks::Log::SetConsoleToStderr(false);
    }

    bool Running() { return g_server != nullptr; }
    Stats GetStats() { return g_server ? g_server->dispatcher.GetStats() : g_lastStats; }
}
