// Headless unit tests for Phisiks::Core (no window, no GPU).
#include "TestFramework.h"

#include "Phisiks/Core/Events.h"
#include "Phisiks/Core/InputState.h"
#include "Phisiks/Core/Profiler.h"
#include "Phisiks/Core/Time.h"

#include <string>
#include <vector>

using namespace Phisiks;

// ---------------------------------------------------------------------------------- FixedStepClock
TEST_CASE("Clock: first tick is exactly one fixed step") {
    FixedStepClock clock;
    clock.Tick(5.0);   // a huge first delta (e.g. loading) must not cause a burst of steps
    CHECK_NEAR(clock.Delta(), 1.0 / 60.0, 1e-12);
    CHECK(clock.ConsumeStep());
    CHECK(!clock.ConsumeStep());
}

TEST_CASE("Clock: accumulates and consumes whole steps, keeps the remainder") {
    FixedStepClock clock;
    clock.Tick(0.0);                    // start
    while (clock.ConsumeStep()) {}
    clock.Tick(2.5 / 60.0);
    int steps = 0;
    while (clock.ConsumeStep()) ++steps;
    CHECK(steps == 2);
    CHECK_NEAR(clock.Alpha(), 0.5, 1e-9);
    clock.Tick(0.5 / 60.0);             // remainder + half step = one whole step
    CHECK(clock.ConsumeStep());
    CHECK(!clock.ConsumeStep());
}

TEST_CASE("Clock: deltas are clamped and steps are capped per frame") {
    FixedStepConfig config;
    config.maxFrameDeltaSeconds = 0.1;
    config.maxStepsPerFrame = 5;
    FixedStepClock clock(config);
    clock.Tick(0.0);
    while (clock.ConsumeStep()) {}
    clock.Tick(3.0);                    // debugger pause
    CHECK_NEAR(clock.RawDelta(), 3.0, 1e-12);
    CHECK_NEAR(clock.Delta(), 0.1, 1e-12);
    int steps = 0;
    while (clock.ConsumeStep()) ++steps;
    CHECK(steps == 5);                  // 0.1 s = 6 steps of 1/60, capped at 5
    CHECK(clock.StepsThisFrame() == 5);
    CHECK_NEAR(clock.Accumulator(), 0.0, 1e-12);   // backlog discarded, no spiral of death
}

TEST_CASE("Clock: simulated time is frame-rate independent") {
    // 2 seconds of 144 Hz frames and of 30 Hz frames must simulate the same number of steps (+-1).
    auto simulate = [](double frameDelta, double seconds) {
        FixedStepClock clock;
        clock.Tick(0.0);
        while (clock.ConsumeStep()) {}
        uint64_t steps = 0;
        for (double t = 0.0; t < seconds - 1e-9; t += frameDelta) {
            clock.Tick(frameDelta);
            while (clock.ConsumeStep()) ++steps;
        }
        return steps;
    };
    const uint64_t fast = simulate(1.0 / 144.0, 2.0);
    const uint64_t slow = simulate(1.0 / 30.0, 2.0);
    CHECK(fast >= 119 && fast <= 121);
    CHECK(slow >= 119 && slow <= 121);
}

// ---------------------------------------------------------------------------------- InputState
TEST_CASE("Input: press/hold/release edges are latched per frame") {
    InputState input;
    input.OnKey(int(Key::W), true);
    input.NewFrame();
    CHECK(input.IsDown(Key::W));
    CHECK(input.WasPressed(Key::W));
    input.NewFrame();                   // held
    CHECK(input.IsDown(Key::W));
    CHECK(!input.WasPressed(Key::W));
    input.OnKey(int(Key::W), false);
    input.NewFrame();
    CHECK(!input.IsDown(Key::W));
    CHECK(input.WasReleased(Key::W));
}

TEST_CASE("Input: a tap within one frame is not lost") {
    InputState input;
    input.OnKey(int(Key::Space), true);
    input.OnKey(int(Key::Space), false);
    input.NewFrame();
    CHECK(input.WasPressed(Key::Space));
    CHECK(input.WasReleased(Key::Space));
    CHECK(!input.IsDown(Key::Space));
}

TEST_CASE("Input: repeated down events do not re-trigger a press") {
    InputState input;
    input.OnKey(int(Key::A), true);
    input.NewFrame();
    input.OnKey(int(Key::A), true);     // OS key repeat
    input.NewFrame();
    CHECK(!input.WasPressed(Key::A));
    CHECK(input.IsDown(Key::A));
}

TEST_CASE("Input: focus loss releases held keys and buttons") {
    InputState input;
    input.OnKey(int(Key::LeftShift), true);
    input.OnMouseButton(int(MouseButton::Left), true);
    input.NewFrame();
    input.OnFocusLost();
    input.NewFrame();
    CHECK(!input.IsDown(Key::LeftShift));
    CHECK(!input.IsDown(MouseButton::Left));
    CHECK(input.WasReleased(Key::LeftShift));
}

TEST_CASE("Input: cursor delta and scroll accumulate within a frame") {
    InputState input;
    input.OnCursorPosition(100, 100);   // first event only establishes the position
    input.OnCursorPosition(110, 95);
    input.OnCursorPosition(115, 90);
    input.OnScroll(0, 1);
    input.OnScroll(0, 2);
    input.NewFrame();
    CHECK_NEAR(input.CursorDelta().x, 15.0, 1e-12);
    CHECK_NEAR(input.CursorDelta().y, -10.0, 1e-12);
    CHECK_NEAR(input.ScrollDelta().y, 3.0, 1e-12);
    input.NewFrame();
    CHECK_NEAR(input.CursorDelta().x, 0.0, 1e-12);
    CHECK_NEAR(input.ScrollDelta().y, 0.0, 1e-12);
    input.ResetCursorTracking();        // e.g. cursor mode change: no jump
    input.OnCursorPosition(500, 500);
    input.NewFrame();
    CHECK_NEAR(input.CursorDelta().x, 0.0, 1e-12);
}

TEST_CASE("Input: out-of-range codes are ignored safely") {
    InputState input;
    input.OnKey(-1, true);
    input.OnKey(100000, true);
    input.OnMouseButton(42, true);
    input.NewFrame();
    CHECK(!input.IsDown(-1));
    CHECK(!input.IsDown(100000));
}

// ---------------------------------------------------------------------------------- EventBus
namespace { struct Ping { int value; }; struct Pong { std::string text; }; }

TEST_CASE("Events: publish reaches typed subscribers only") {
    EventBus bus;
    int pings = 0, pongs = 0;
    Subscription a = bus.Subscribe<Ping>([&](const Ping& e) { pings += e.value; });
    Subscription b = bus.Subscribe<Pong>([&](const Pong&) { ++pongs; });
    bus.Publish(Ping{ 3 });
    bus.Publish(Ping{ 4 });
    CHECK(pings == 7);
    CHECK(pongs == 0);
}

TEST_CASE("Events: subscription unsubscribes on destruction and on move") {
    EventBus bus;
    int count = 0;
    {
        Subscription s = bus.Subscribe<Ping>([&](const Ping&) { ++count; });
        bus.Publish(Ping{ 1 });
        Subscription moved = std::move(s);
        CHECK(!s.Active());
        CHECK(moved.Active());
        bus.Publish(Ping{ 1 });
    }
    bus.Publish(Ping{ 1 });
    CHECK(count == 2);
    CHECK(bus.HandlerCount() == 0);
}

TEST_CASE("Events: queued events are delivered in order, including ones enqueued during dispatch") {
    EventBus bus;
    std::vector<int> order;
    Subscription s = bus.Subscribe<Ping>([&](const Ping& e) {
        order.push_back(e.value);
        if (e.value == 1) bus.Enqueue(Ping{ 3 });
    });
    bus.Enqueue(Ping{ 1 });
    bus.Enqueue(Ping{ 2 });
    CHECK(order.empty());
    const size_t delivered = bus.DispatchQueued();
    CHECK(delivered == 3);
    CHECK((order == std::vector<int>{ 1, 2, 3 }));
}

TEST_CASE("Events: unsubscribing during dispatch stops later delivery; bus may die first") {
    Subscription survivor;
    {
        EventBus bus;
        int first = 0, second = 0;
        Subscription b;
        Subscription a = bus.Subscribe<Ping>([&](const Ping&) { ++first; b.Reset(); });
        b = bus.Subscribe<Ping>([&](const Ping&) { ++second; });
        bus.Publish(Ping{ 0 });
        CHECK(first == 1);
        CHECK(second == 0);                 // removed before its turn
        survivor = bus.Subscribe<Pong>([](const Pong&) {});
    }
    CHECK(!survivor.Active());              // bus destroyed: subscription is inert, no crash on reset
    survivor.Reset();
}

// ---------------------------------------------------------------------------------- Profiler
TEST_CASE("Profiler: zones sum calls within a frame and average across frames") {
    Profiler& profiler = Profiler::Get();
    profiler.Clear();
    for (int frame = 0; frame < 4; ++frame) {
        profiler.BeginFrame();
        profiler.AddSample("Physics", 1.0);
        profiler.AddSample("Physics", 2.0);
        profiler.AddSample("Render", frame == 3 ? 8.0 : 4.0);
        profiler.EndFrame();
    }
    const ProfileZoneStats* physics = profiler.GetZone("Physics");
    const ProfileZoneStats* render = profiler.GetZone("Render");
    CHECK(physics && render);
    CHECK_NEAR(physics->lastMs, 3.0, 1e-12);
    CHECK(physics->callsLastFrame == 2);
    CHECK_NEAR(render->averageMs, 5.0, 1e-12);
    CHECK_NEAR(render->maxMs, 8.0, 1e-12);
    const std::vector<ProfileZoneStats> zones = profiler.GetZones();
    CHECK(zones.size() == 2 && zones[0].name == "Render");   // sorted by average
    profiler.Clear();
}

TEST_CASE("Profiler: scoped zone measures elapsed time") {
    Profiler& profiler = Profiler::Get();
    profiler.Clear();
    profiler.BeginFrame();
    {
        PHISIKS_PROFILE_SCOPE("Busy");
        const double start = Time::NowSeconds();
        while (Time::NowSeconds() - start < 0.005) {}
    }
    profiler.EndFrame();
    const ProfileZoneStats* busy = profiler.GetZone("Busy");
    CHECK(busy && busy->lastMs >= 4.5 && busy->lastMs < 200.0);
    profiler.Clear();
}
