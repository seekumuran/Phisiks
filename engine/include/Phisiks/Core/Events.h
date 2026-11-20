#pragma once
// Typed publish/subscribe event bus.
//
//   struct WindowResized { int width, height; };
//   Phisiks::Subscription s = bus.Subscribe<WindowResized>([](const WindowResized& e) { ... });
//   bus.Publish(WindowResized{ 1280, 720 });   // immediate
//   bus.Enqueue(WindowResized{ 1280, 720 });   // delivered by the next DispatchQueued()
//
// Subscriptions unsubscribe automatically when the Subscription object is destroyed. Handlers may
// subscribe/unsubscribe or publish during dispatch; changes apply to the next delivery.
// Not thread-safe: use from the main thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Phisiks {

    class EventBus;

    class Subscription {
    public:
        Subscription() = default;
        Subscription(std::weak_ptr<EventBus*> bus, std::type_index type, uint64_t id) : m_bus(std::move(bus)), m_type(type), m_id(id) {}
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept { *this = std::move(other); }
        Subscription& operator=(Subscription&& other) noexcept;
        ~Subscription() { Reset(); }
        void Reset();
        bool Active() const { return m_id != 0 && !m_bus.expired(); }

    private:
        std::weak_ptr<EventBus*> m_bus;
        std::type_index m_type = typeid(void);
        uint64_t m_id = 0;
    };

    class EventBus {
    public:
        EventBus() : m_self(std::make_shared<EventBus*>(this)) {}
        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;

        template<class E>
        [[nodiscard]] Subscription Subscribe(std::function<void(const E&)> handler) {
            const uint64_t id = ++m_nextId;
            m_handlers[typeid(E)].push_back({ id, [h = std::move(handler)](const void* e) { h(*static_cast<const E*>(e)); } });
            return Subscription(m_self, typeid(E), id);
        }

        template<class E>
        void Publish(const E& event) {
            auto it = m_handlers.find(typeid(E));
            if (it == m_handlers.end()) return;
            const std::vector<Handler> snapshot = it->second;   // stable against (un)subscribe during dispatch
            for (const Handler& handler : snapshot) {
                if (IsLive(typeid(E), handler.id)) handler.fn(&event);
            }
        }

        template<class E>
        void Enqueue(E event) {
            auto shared = std::make_shared<E>(std::move(event));
            m_queue.push_back([this, shared] { Publish(*shared); });
        }

        // Delivers queued events (including ones enqueued by handlers during this call, in order).
        size_t DispatchQueued() {
            size_t delivered = 0;
            while (!m_queue.empty()) {
                std::vector<std::function<void()>> batch;
                batch.swap(m_queue);
                for (auto& deliver : batch) { deliver(); ++delivered; }
            }
            return delivered;
        }

        size_t HandlerCount() const {
            size_t count = 0;
            for (const auto& [type, handlers] : m_handlers) count += handlers.size();
            return count;
        }

        void Unsubscribe(std::type_index type, uint64_t id) {
            auto it = m_handlers.find(type);
            if (it == m_handlers.end()) return;
            auto& handlers = it->second;
            for (size_t i = 0; i < handlers.size(); ++i) {
                if (handlers[i].id == id) { handlers.erase(handlers.begin() + i); return; }
            }
        }

    private:
        struct Handler { uint64_t id; std::function<void(const void*)> fn; };

        bool IsLive(std::type_index type, uint64_t id) const {
            auto it = m_handlers.find(type);
            if (it == m_handlers.end()) return false;
            for (const Handler& handler : it->second) if (handler.id == id) return true;
            return false;
        }

        std::shared_ptr<EventBus*> m_self;   // Subscriptions hold a weak_ptr; safe if the bus dies first
        std::unordered_map<std::type_index, std::vector<Handler>> m_handlers;
        std::vector<std::function<void()>> m_queue;
        uint64_t m_nextId = 0;
    };

    inline Subscription& Subscription::operator=(Subscription&& other) noexcept {
        if (this != &other) {
            Reset();
            m_bus = std::move(other.m_bus);
            m_type = other.m_type;
            m_id = other.m_id;
            other.m_id = 0;
        }
        return *this;
    }

    inline void Subscription::Reset() {
        if (m_id == 0) return;
        if (auto bus = m_bus.lock()) (*bus)->Unsubscribe(m_type, m_id);
        m_id = 0;
        m_bus.reset();
    }

    // Engine-wide bus (window, input focus, scene lifecycle events).
    EventBus& GlobalEvents();

    // Built-in engine events.
    struct WindowResizedEvent { int framebufferWidth = 0; int framebufferHeight = 0; };
    struct WindowFocusEvent { bool focused = false; };
    struct WindowCloseRequestedEvent {};
    struct SceneLoadedEvent { std::string sceneName; };
}
