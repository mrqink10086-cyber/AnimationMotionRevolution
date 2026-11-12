#include "MotionCache.h"
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::cerr << "check failed: " #x << " at " << __LINE__ << '\n'; std::exit(1); } } while (false)

int main()
{
    amr::MotionCache<int> cache;
    int character{}, graph{}, otherGraph{}, animation{}, generator{};
    CHECK(!cache.Begin(&character, &graph));
    cache.RegisterCharacter(&character);
    const auto old = cache.Begin(&character, &graph);
    auto data = std::make_shared<const int>(7);
    cache.Activate(old, "clip", &animation, data, &generator);
    auto held = cache.LookupBinding(&character, &graph, "clip");
    cache.UnregisterCharacter(&character);
    CHECK(!cache.HasActive(&character, [](int) { return true; }));
    cache.Activate(old, "clip", &animation, data, &generator);
    CHECK(!cache.Lookup(&character, &graph, "clip"));
    CHECK(*held.data == 7 && held.runtime);

    cache.RegisterCharacter(&character);
    const auto reused = cache.Begin(&character, &graph);
    CHECK(reused.instance != old.instance);
    cache.Activate(reused, "clip", &animation, std::make_shared<const int>(12), &generator);
    cache.Activate(old, "clip", &animation, data, &generator);
    cache.Deactivate(old, "clip", &animation, &generator);
    cache.DeactivateGenerator(old, "clip", &generator);
    cache.RetireCharacter(old, [](const auto&) { CHECK(false); });
    CHECK(*cache.Lookup(&character, &graph, "clip") == 12);

    const auto replacement = cache.Begin(&character, &otherGraph);
    CHECK(!cache.Lookup(&character, &graph, "clip"));
    cache.Activate(reused, "clip", &animation, data, &generator);
    CHECK(!cache.HasActive(&character, [](int) { return true; }));
    cache.Activate(replacement, "clip", &animation, data, &generator);
    cache.Clear();
    cache.Activate(replacement, "clip", &animation, data, &generator);
    CHECK(!cache.HasActive(&character, [](int) { return true; }));
    const auto afterClear = cache.Begin(&character, &otherGraph);
    cache.Activate(afterClear, "clip", &animation, data, &generator);
    CHECK(cache.Lookup(&character, &otherGraph, "clip") == data);

    std::barrier rendezvous(2);
    std::thread latePublisher([&] {
        const auto ticket = cache.Begin(&character, &otherGraph);
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
        cache.Activate(ticket, "late", &animation, data, &generator);
        cache.Deactivate(ticket, "current", &animation, &generator);
    });
    rendezvous.arrive_and_wait();
    cache.UnregisterCharacter(&character);
    cache.RegisterCharacter(&character);
    cache.Activate(cache.Begin(&character, &otherGraph), "current", &animation, data, &generator);
    rendezvous.arrive_and_wait();
    latePublisher.join();
    CHECK(!cache.Lookup(&character, &otherGraph, "late"));
    CHECK(cache.Lookup(&character, &otherGraph, "current") == data);
    cache.UnregisterCharacter(&character);
    CHECK(*held.data == 7);
    std::cout << "graph instance lifecycle tests passed\n";
}
