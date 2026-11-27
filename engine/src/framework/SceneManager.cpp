#include "Phisiks/Framework/SceneManager.h"
#include "Phisiks/Framework/GameContext.h"
#include "Phisiks/Core/Log.h"


#include <algorithm>

namespace Phisiks {
    void SceneManager::Register(const std::string& name, Factory factory) {
        m_entries.push_back({ name, std::move(factory) });
    }

    bool SceneManager::Has(const std::string& name) const {
        return std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.name == name; });
    }

    std::vector<std::string> SceneManager::GetSceneNames() const {
        std::vector<std::string> names;
        for (const Entry& e : m_entries) names.push_back(e.name);
        return names;
    }

    void SceneManager::Request(const std::string& name) {
        if (!Has(name)) {
            Log::Error() << "SceneManager: unknown scene '" << name << "'";
            return;
        }
        m_pending = name;
    }

    void SceneManager::RequestReload() {
        if (!m_currentEntryName.empty()) m_pending = m_currentEntryName;
    }

    void SceneManager::RequestNext() {
        if (m_entries.empty()) return;
        size_t index = 0;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].name == m_currentEntryName) { index = (i + 1) % m_entries.size(); break; }
        }
        m_pending = m_entries[index].name;
    }

    void SceneManager::TearDown(GameContext& ctx) {
        if (m_current) {
            m_current->OnExit(ctx);
            Log::Info() << "Scene '" << m_current->Name() << "' exited";
        }
        ctx.player.Despawn();
        // Destroying the scene destroys its entities; EnTT hooks release their physics bodies,
        // render objects, lights and voices.
        m_current.reset();
    }

    bool SceneManager::ApplyPending(GameContext& ctx) {
        if (m_pending.empty()) return false;
        const std::string name = m_pending;
        m_pending.clear();

        auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.name == name; });
        if (it == m_entries.end()) return false;

        Log::Info() << "Scene transition: '" << m_currentEntryName << "' -> '" << name << "'";
        TearDown(ctx);

        m_current = it->factory();
        m_currentEntryName = name;
        if (!m_current) {
            Log::Error() << "SceneManager: factory for '" << name << "' returned null";
            return false;
        }
        m_current->OnEnter(ctx);
        ++m_transitionCount;
        ctx.PushEvent("Entered scene '" + name + "' (" + std::to_string(m_current->EntityCount()) + " entities)");
        return true;
    }

    void SceneManager::UnloadCurrent(GameContext& ctx) {
        TearDown(ctx);
        m_currentEntryName.clear();
        m_pending.clear();
    }
}
