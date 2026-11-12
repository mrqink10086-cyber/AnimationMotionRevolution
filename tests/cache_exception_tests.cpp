#include "MotionCache.h"
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>

namespace
{
    std::atomic<std::ptrdiff_t> beforeFailure{-1};
    bool FailAllocation() noexcept
    {
        auto remaining = beforeFailure.load();
        while (remaining >= 0) {
            if (beforeFailure.compare_exchange_weak(remaining, remaining - 1)) return remaining == 0;
        }
        return false;
    }
    #define CHECK(x) do { if (!(x)) { std::cerr << "check failed: " #x << " at " << __LINE__ << '\n'; std::exit(1); } } while (false)
    struct Data
    {
        int value;
        std::function<void()> onDestroy;
        ~Data() { if (onDestroy) onDestroy(); }
    };
    using Cache = amr::MotionCache<Data>;
    auto Make(int value, std::function<void()> onDestroy = {})
    {
        return std::shared_ptr<const Data>(new Data{value, std::move(onDestroy)});
    }
    constexpr std::string_view clip = "a long playback name that allocates storage beyond the small string buffer";

    void FirstActivationFailures()
    {
        int failures = 0;
        bool success = false;
        for (std::ptrdiff_t failAt = 0; failAt < 64; ++failAt) {
            Cache cache;
            int character{}, graph{}, animation{};
            cache.RegisterCharacter(&character);
            const auto ticket = cache.Begin(&character, &graph);
            auto data = Make(1);
            bool threw = false;
            beforeFailure = failAt;
            try { cache.Activate(ticket, clip, &animation, data); }
            catch (const std::bad_alloc&) { threw = true; }
            beforeFailure = -1;
            if (threw) {
                ++failures;
                CHECK(!cache.HasActive(&character, [](const auto&) { return true; }));
            } else {
                CHECK(cache.Lookup(&character, &graph, clip) == data);
                success = true;
                break;
            }
        }
        CHECK(failures > 0 && success);
    }

    void AppendFailuresReleaseOutsideLock()
    {
        int failures = 0;
        bool success = false;
        for (std::ptrdiff_t failAt = 0; failAt < 64; ++failAt) {
            Cache cache;
            int character{}, graph{}, animation{};
            cache.RegisterCharacter(&character);
            const auto ticket = cache.Begin(&character, &graph);
            auto first = Make(1);
            cache.Activate(ticket, clip, &animation, first);
            bool destroyed = false;
            auto next = Make(2, [&] {
                CHECK(cache.Lookup(&character, &graph, clip) == first);
                destroyed = true;
            });
            bool threw = false;
            beforeFailure = failAt;
            try { cache.Activate(ticket, clip, &animation, std::move(next)); }
            catch (const std::bad_alloc&) { threw = true; }
            beforeFailure = -1;
            if (threw) {
                ++failures;
                CHECK(destroyed && cache.Lookup(&character, &graph, clip) == first);
            } else {
                CHECK(cache.Lookup(&character, &graph, clip)->value == 2);
                cache.Deactivate(ticket, clip, &animation);
                CHECK(destroyed);
                success = true;
                break;
            }
        }
        CHECK(failures > 0 && success);
    }

    void RetirementAndDestructionNeedNoAllocation()
    {
        Cache cache;
        int character{}, graph{}, animation{}, nextGraph{};
        cache.RegisterCharacter(&character);
        auto ticket = cache.Begin(&character, &graph);
        bool destroyed = false, visited = false;
        cache.Activate(ticket, clip, &animation, Make(1, [&] {
            CHECK(!cache.Lookup(&character, &graph, clip));
            destroyed = true;
        }));
        beforeFailure = 0;
        cache.RetireCharacter(ticket, [&](const auto&) {
            CHECK(!cache.Lookup(&character, &graph, clip));
            visited = true;
        });
        CHECK(beforeFailure == 0 && destroyed && visited);
        beforeFailure = -1;
        cache.Activate(ticket, clip, &animation, Make(2));
        beforeFailure = 0;
        ticket = cache.Begin(&character, &nextGraph);
        CHECK(beforeFailure == 0 && !cache.Lookup(&character, &graph, clip));
        beforeFailure = -1;
        cache.Activate(ticket, clip, &animation, Make(3));
        beforeFailure = 0;
        cache.UnregisterCharacter(&character);
        CHECK(beforeFailure == 0 && !cache.HasActive(&character, [](const auto&) { return true; }));
        beforeFailure = -1;
    }

    void FailedRebindRetiresOldPlayback()
    {
        for (const bool failParsing : {true, false}) {
            Cache cache;
            int character{}, graph{}, animationA{}, animationB{}, generator{};
            cache.RegisterCharacter(&character);
            const auto ticket = cache.Begin(&character, &graph);
            cache.Activate(ticket, clip, &animationA, Make(1), &generator);
            auto parsed = failParsing ? Cache::Snapshot{} : Make(2);
            bool threw = false;
            beforeFailure = 0;
            try {
                if (failParsing) parsed = Make(2);
                cache.Activate(ticket, clip, &animationB, std::move(parsed), &generator);
            } catch (const std::bad_alloc&) {
                threw = true;
                cache.RetireCharacter(ticket, [](const auto&) {});
            }
            beforeFailure = -1;
            CHECK(threw && !cache.HasActive(&character, [](const auto&) { return true; }));
        }
    }

    void FailedRegistrationCannotPublish()
    {
        Cache cache;
        int character{}, graph{}, animation{};
        bool threw = false;
        beforeFailure = 0;
        try { cache.RegisterCharacter(&character); }
        catch (const std::bad_alloc&) { threw = true; }
        beforeFailure = -1;
        CHECK(threw && !cache.Begin(&character, &graph));
        cache.Activate({}, clip, &animation, Make(1));
        CHECK(!cache.HasActive(&character, [](const auto&) { return true; }));
    }
}

int main()
{
    FirstActivationFailures();
    AppendFailuresReleaseOutsideLock();
    RetirementAndDestructionNeedNoAllocation();
    FailedRebindRetiresOldPlayback();
    FailedRegistrationCannotPublish();
    std::cout << "MotionCache allocation-failure tests passed\n";
}

void* operator new(std::size_t size)
{
    if (FailAllocation()) throw std::bad_alloc{};
    if (auto* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
