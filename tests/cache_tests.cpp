#include "MotionCache.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::cerr << "check failed: " #x << " at " << __LINE__ << '\n'; std::exit(1); } } while (false)

namespace
{
    struct Data
    {
        int value;
        std::function<void()> onDestroy;
        ~Data() { if (onDestroy) onDestroy(); }
    };
    struct Runtime { int value{}; };
    using Cache = amr::MotionCache<Data, Runtime>;
    Cache::Snapshot MakeData(int value, std::function<void()> callback = {})
    {
        return std::shared_ptr<const Data>(new Data{ value, std::move(callback) });
    }

    void PlaybackOrderingAndSnapshots()
    {
        Cache cache;
        int character{}, graph{}, animationA{}, animationB{}, genOld{}, genB{}, genNew{};
        cache.RegisterCharacter(&character);
        const auto ticket = cache.Begin(&character, &graph);
        auto first = MakeData(1), second = MakeData(2), third = MakeData(3);
        cache.Activate(ticket, "clip", &animationA, first, &genOld);
        const auto held = cache.LookupBinding(&character, &graph, "clip");
        held.runtime->value = 10;
        cache.Activate(ticket, "clip", &animationA, second, &genB);
        cache.Activate(ticket, "clip", &animationA, third, &genNew);
        CHECK(cache.LookupBinding(&character, &graph, "clip").runtime != held.runtime);
        cache.Deactivate(ticket, "clip", &animationB, &genNew);
        CHECK(cache.Lookup(&character, &graph, "clip") == third);
        cache.Deactivate(ticket, "clip", &animationA, &genOld);
        CHECK(cache.Lookup(&character, &graph, "clip") == third);
        cache.Deactivate(ticket, "clip", &animationA, &genNew);
        CHECK(cache.Lookup(&character, &graph, "clip") == second);
        cache.Deactivate(ticket, "clip", &animationA, &genB);
        CHECK(!cache.HasActive(&character, [](const auto&) { return true; }));
        CHECK(held.data == first && held.runtime->value == 10);

        cache.Activate(ticket, "clip", &animationA, first, &genOld);
        cache.Activate(ticket, "clip", &animationA, second, &genOld);
        cache.Deactivate(ticket, "clip", &animationA, &genOld);
        CHECK(cache.Lookup(&character, &graph, "clip") == first);
        cache.Activate(ticket, "clip", &animationB, third, &genOld);
        CHECK(!cache.FindBinding(&character, &graph, "clip", &animationA, &genOld).data);
        CHECK(cache.FindGeneratorBinding(&character, &graph, "clip", &genOld).data == third);
        cache.Activate(ticket, "clip", &animationA, second, &genB);
        const auto retired = cache.DeactivateGenerator(ticket, "clip", &genOld);
        CHECK(retired.size() == 1 && cache.Lookup(&character, &graph, "clip") == second);
        cache.Clear();
        CHECK(!cache.Lookup(&character, &graph, "clip") && held.data == first);
    }

    void DestructionAndRetirementOutsideLocks()
    {
        Cache cache;
        int character{}, graph{}, nextGraph{}, animation{};
        cache.RegisterCharacter(&character);
        auto ticket = cache.Begin(&character, &graph);
        int destroyed = 0;
        auto make = [&] { return MakeData(1, [&] {
            CHECK(!cache.HasActive(&character, [](const auto&) { return true; }));
            ++destroyed;
        }); };
        cache.Activate(ticket, "clip", &animation, make());
        cache.Deactivate(ticket, "clip", &animation);
        CHECK(destroyed == 1);
        cache.Activate(ticket, "clip", &animation, make());
        cache.RetireCharacter(ticket, [&](const auto&) {
            CHECK(!cache.HasActive(&character, [](const auto&) { return true; }));
        });
        CHECK(destroyed == 2);
        cache.Activate(ticket, "clip", &animation, make());
        ticket = cache.Begin(&character, &nextGraph);
        CHECK(destroyed == 3);
        cache.Activate(ticket, "clip", &animation, make());
        cache.UnregisterCharacter(&character);
        CHECK(destroyed == 4);
        cache.RegisterCharacter(&character);
        cache.Activate(cache.Begin(&character, &graph), "clip", &animation, make());
        cache.Clear();
        CHECK(destroyed == 5);
    }

    void ConcurrentReadersWritersAndClear()
    {
        Cache cache;
        std::array<int, 8> characters{}, graphs{}, animations{};
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < characters.size(); ++i) {
            cache.RegisterCharacter(&characters[i]);
            threads.emplace_back([&, i] {
                for (int n = 0; n < 500; ++n) {
                    const auto ticket = cache.Begin(&characters[i], &graphs[i]);
                    cache.Activate(ticket, "clip", &animations[i], MakeData(n));
                    (void)cache.LookupBinding(&characters[i], &graphs[i], "clip");
                    cache.Deactivate(ticket, "clip", &animations[i]);
                }
            });
        }
        std::thread clearer([&] { for (int n = 0; n < 80; ++n) cache.Clear(); });
        for (auto& thread : threads) thread.join();
        clearer.join();
        cache.Clear();
        for (const auto& character : characters) {
            CHECK(!cache.HasActive(&character, [](const auto&) { return true; }));
        }
    }
}

int main()
{
    PlaybackOrderingAndSnapshots();
    DestructionAndRetirementOutsideLocks();
    ConcurrentReadersWritersAndClear();
    std::cout << "MotionCache host tests passed\n";
}
