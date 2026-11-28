#pragma once
// PAI, the PHISIKS Agent Interface: a controlled, local interface through which an external program
// (an AI agent, a script, a test tool) inspects and edits the scene open in the PHISIKS editor and
// runs its simulation. PHISIKS contains no AI and needs no network: the client is a separate
// process the user starts and connects (library PhisiksPAI; protocol in docs/PAI.md).
//
// Protocol: JSON-RPC 2.0, one message per line (UTF-8, newline-delimited). Transport: the editor
// process's standard input / output (`Phisiks --pai`); the engine log moves to stderr. There is no
// network listener. Disabled unless the user starts the editor with --pai.
//
// Permissions (chosen by the user on the command line, fixed for the session):
//   read      (--pai)            project, scene, entities, components, assets, history, engine state
//   write     (--pai-write)      create / delete / rename / re-parent entities, set transforms and
//                                component values, assign project assets, undo / redo, save the
//                                current scene file (only that file). Every change is one undo step.
//   simulate  (--pai-simulate)   play / pause / step / stop the simulation, read results
// Not possible through PAI: shell commands, arbitrary file access (no paths are accepted except
// project asset ids), code execution (Lua sources are not writable through PAI), network access.
// Every value is validated by the same serializer that loads scene files; limits bound request
// size, queue length, list sizes and work per frame. Mutations are refused while playing.
#include <cstddef>
#include <cstdint>
#include <string>

namespace Phisiks { struct GameContext; }

namespace Pai {
    inline constexpr const char* PROTOCOL = "phisiks-pai";
    inline constexpr const char* PROTOCOL_VERSION = "1.0";

    struct Permissions {
        bool read = true;
        bool write = false;
        bool simulate = false;
    };

    struct Limits {
        size_t maxRequestBytes = 256 * 1024;   // longer lines are rejected (-32600)
        size_t maxQueued = 64;                 // pending requests; more are rejected (-32004)
        int maxPerFrame = 32;                  // requests handled per editor frame
        size_t maxListed = 5000;               // entities / assets per list response
        size_t maxNameLength = 256;
        int maxStepFrames = 3600;              // sim.step
    };

    struct Stats {
        uint64_t requests = 0, errors = 0, rejected = 0, mutations = 0;
    };

    // Handles requests in the editor's context (main thread). Transport-independent: tests and
    // in-process tools call Handle directly.
    class Dispatcher {
    public:
        explicit Dispatcher(Permissions permissions = {}, Limits limits = {});
        // One JSON-RPC request (or a batch array) -> the response text ("" for notifications).
        std::string Handle(Phisiks::GameContext& ctx, const std::string& request);
        // The capability manifest: every method with its parameters, permission and description.
        std::string Manifest() const;
        const Permissions& GetPermissions() const { return m_permissions; }
        const Limits& GetLimits() const { return m_limits; }
        const Stats& GetStats() const { return m_stats; }
    private:
        Permissions m_permissions;
        Limits m_limits;
        Stats m_stats;
    };

    // stdio transport: a reader thread queues request lines; the editor handles them at the start of
    // each frame (Editor::AddFrameHook) and writes the responses to stdout. Returns false if PAI is
    // already running. The console log is moved to stderr.
    bool StartStdio(const Permissions& permissions, const Limits& limits = {});
    void Stop();
    bool Running();
    Stats GetStats();
}
