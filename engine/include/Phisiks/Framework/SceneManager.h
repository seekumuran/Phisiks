#pragma once
#include "Phisiks/Scene/Scene.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Phisiks {
    struct GameContext;

    class SceneManager {
    public:
        using Factory = std::function<std::unique_ptr<Scene>()>;

        void Register(const std::string& name, Factory factory);
        bool Has(const std::string& name) const;
        std::vector<std::string> GetSceneNames() const;

        // Transitions are deferred to the start of the next frame (a safe point).
        void Request(const std::string& name);
        void RequestReload();
        void RequestNext();                       // cycles through registered scenes
        bool HasPendingRequest() const { return !m_pending.empty(); }

        // Called by the Application at the start of a frame. Returns true if a transition happened.
        bool ApplyPending(GameContext& ctx);
        // Tears down the current scene (shutdown).
        void UnloadCurrent(GameContext& ctx);

        Scene* Current() { return m_current.get(); }
        std::string CurrentName() const { return m_current ? m_current->Name() : std::string(); }
        int GetTransitionCount() const { return m_transitionCount; }

    private:
        void TearDown(GameContext& ctx);

        struct Entry { std::string name; Factory factory; };
        std::vector<Entry> m_entries;
        std::unique_ptr<Scene> m_current;
        std::string m_currentEntryName;
        std::string m_pending;
        int m_transitionCount = 0;
    };
}
