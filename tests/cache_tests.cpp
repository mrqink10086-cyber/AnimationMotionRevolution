// Standalone host tests for MotionCache. These cover cache behavior only; they
// do not validate CommonLib or game ABI behavior.
#include "MotionCache.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
	std::atomic<int> failures = 0;

	#define CHECK(condition)                                                                                              \
		do {                                                                                                                \
			if (!(condition)) {                                                                                               \
				std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #condition << '\n';                             \
				++failures;                                                                                                     \
			}                                                                                                                 \
		} while (false)

	struct Data
	{
		Data(int a_value, std::function<void()> a_onDestroy = {}) : value(a_value), onDestroy(std::move(a_onDestroy)) {}
		~Data()
		{
			if (onDestroy) {
				onDestroy();
			}
		}

		int value;
		std::function<void()> onDestroy;
	};

	struct RuntimeState
	{
		int value = 0;
	};

	using Cache = amr::MotionCache<Data, std::shared_ptr<const void>, RuntimeState>;
	using Snapshot = Cache::Snapshot;

	Snapshot MakeData(int a_value, std::function<void()> a_onDestroy = {})
	{
		return std::make_shared<const Data>(a_value, std::move(a_onDestroy));
	}

	std::shared_ptr<const void> MakeOwner(int a_value, std::function<void()> a_onDestroy = {})
	{
		return std::shared_ptr<const void>(new int(a_value), [callback = std::move(a_onDestroy)](const void* a_ptr) {
			if (callback) {
				callback();
			}
			delete static_cast<const int*>(a_ptr);
		});
	}

	int Value(const Snapshot& a_snapshot)
	{
		return a_snapshot ? a_snapshot->value : -9999;
	}

	void TestRememberReturnsOriginalSharedObjectForPositiveAndNegativeData()
	{
		Cache cache;
		int animationPositive = 1;
		int animationNegative = 2;
		const auto generation = cache.GetGeneration();

		auto positive = MakeData(42);
		auto firstPositive = cache.Remember(&animationPositive, positive, generation);
		auto otherPositive = MakeData(43);
		auto secondPositive = cache.Remember(&animationPositive, otherPositive, generation);
		CHECK(firstPositive == positive);
		CHECK(secondPositive == firstPositive);
		CHECK(cache.FindParsed(&animationPositive) == firstPositive);

		auto negative = MakeData(-1);
		auto firstNegative = cache.Remember(&animationNegative, negative, generation);
		auto otherNegative = MakeData(-2);
		auto secondNegative = cache.Remember(&animationNegative, otherNegative, generation);
		CHECK(firstNegative == negative);
		CHECK(secondNegative == firstNegative);
		CHECK(Value(cache.FindParsed(&animationNegative)) == -1);
	}

	void TestParsedCacheUsesLruAndCapsAt32()
	{
		Cache cache;
		std::array<int, Cache::parsedCapacity + 1> animations{};
		const auto generation = cache.GetGeneration();
		for (std::size_t i = 0; i < Cache::parsedCapacity; ++i) {
			CHECK(cache.Remember(&animations[i], MakeData(static_cast<int>(i)), generation));
		}
		CHECK(cache.FindParsed(&animations[0])); // Refresh the oldest entry.
		CHECK(cache.Remember(&animations[Cache::parsedCapacity], MakeData(32), generation));
		CHECK(cache.FindParsed(&animations[0]));
		CHECK(!cache.FindParsed(&animations[1]));
		for (std::size_t i = 2; i <= Cache::parsedCapacity; ++i) {
			CHECK(cache.FindParsed(&animations[i]));
		}
	}

	void TestSnapshotsSurviveDeactivateAndClear()
	{
		Cache cache;
		int character = 1;
		int graph = 2;
		int animation = 3;
		const auto generation = cache.GetGeneration();
		auto value = MakeData(7);
		cache.Activate(&character, &graph, MakeOwner(1), "walk", &animation, value, generation);
		Snapshot held = cache.Lookup(&character, &graph, "walk");
		CHECK(held == value);
		cache.Deactivate(&character, &graph, "walk", &animation, generation);
		CHECK(!cache.Lookup(&character, &graph, "walk"));
		CHECK(Value(held) == 7);

		int animation2 = 4;
		cache.Activate(&character, &graph, MakeOwner(2), "run", &animation2, value, cache.GetGeneration());
		Snapshot heldAcrossClear = cache.Lookup(&character, &graph, "run");
		cache.Clear();
		CHECK(!cache.Lookup(&character, &graph, "run"));
		CHECK(Value(heldAcrossClear) == 7);
	}

	void TestActiveLookupAndPerActivationRuntime()
	{
		Cache cache;
		int character = 1;
		int graph = 2;
		int animation = 3;
		int firstGenerator = 4;
		int secondGenerator = 5;
		const auto generation = cache.GetGeneration();
		auto motion = MakeData(7);
		auto noMotion = MakeData(-1);

		// Parsed cache entries alone are not active API records.
		CHECK(cache.Remember(&animation, noMotion, generation));
		CHECK(!cache.HasActive(&character, [](const Data& a_data) { return a_data.value > 0; }));

		cache.Activate(&character, &graph, MakeOwner(1), "clip", &animation, motion, generation, &firstGenerator);
		auto first = cache.LookupBinding(&character, &graph, "clip");
		CHECK(first.data == motion);
		CHECK(first.runtime);
		first.runtime->value = 11;
		CHECK(cache.HasActive(&character, [](const Data& a_data) { return a_data.value > 0; }));

		cache.Activate(&character, &graph, MakeOwner(2), "clip", &animation, motion, generation, &secondGenerator);
		auto second = cache.LookupBinding(&character, &graph, "clip");
		CHECK(second.runtime != first.runtime);
		second.runtime->value = 22;
		CHECK(second.runtime->value == 22);
		CHECK(first.runtime->value == 11);

		cache.Deactivate(&character, &graph, "clip", &animation, generation, &secondGenerator);
		auto restored = cache.LookupBinding(&character, &graph, "clip");
		CHECK(restored.runtime == first.runtime);
		cache.Deactivate(&character, &graph, "clip", &animation, generation, &firstGenerator);
		CHECK(!cache.HasActive(&character, [](const Data& a_data) { return a_data.value > 0; }));
		CHECK(first.runtime->value == 11); // A retained view pins its runtime through hook completion.
	}

	void TestActivationCountsWrongAnimationPromotionAndRestore()
	{
		Cache cache;
		int character = 1;
		int graph = 2;
		int animationA = 3;
		int animationB = 4;
		int wrongAnimation = 5;
		int generatorAOld = 6;
		int generatorB = 7;
		int generatorANew = 8;
		int generatorRepeated = 9;
		const auto generation = cache.GetGeneration();
		auto dataA = MakeData(10);
		auto dataB = MakeData(20);
		const auto owner = MakeOwner(1);

		cache.Activate(&character, &graph, owner, "idle", &animationA, dataA, generation);
		cache.Activate(&character, &graph, owner, "idle", &animationB, dataB, generation);
		CHECK(Value(cache.Lookup(&character, &graph, "idle")) == 20);
		cache.Deactivate(&character, &graph, "idle", &wrongAnimation, generation);
		CHECK(Value(cache.Lookup(&character, &graph, "idle")) == 20);
		cache.Deactivate(&character, &graph, "idle", &animationB, generation);
		CHECK(Value(cache.Lookup(&character, &graph, "idle")) == 10);
		cache.Deactivate(&character, &graph, "idle", &animationA, generation);
		CHECK(!cache.Lookup(&character, &graph, "idle"));

		// A and B use the same parsed animation but come from different
		// generators. Removing the older A activation must leave newer A active;
		// removing newer A restores B.
		cache.Activate(&character, &graph, owner, "idle", &animationA, dataA, generation, &generatorAOld);
		cache.Activate(&character, &graph, owner, "idle", &animationA, dataB, generation, &generatorB);
		auto dataANew = MakeData(30);
		cache.Activate(&character, &graph, owner, "idle", &animationA, dataANew, generation, &generatorANew);
		CHECK(Value(cache.Lookup(&character, &graph, "idle")) == 30);
		cache.Deactivate(&character, &graph, "idle", &animationA, generation, &generatorAOld);
		CHECK(Value(cache.Lookup(&character, &graph, "idle")) == 30);
		cache.Deactivate(&character, &graph, "idle", &animationA, generation, &generatorANew);
		CHECK(Value(cache.Lookup(&character, &graph, "idle")) == 20);
		cache.Deactivate(&character, &graph, "idle", &animationA, generation, &generatorB);
		CHECK(!cache.Lookup(&character, &graph, "idle"));

		// Repeated activations from one generator are separate stack entries.
		// Deactivation reverse-pops the newest, revealing the prior data, then
		// removes the remaining entry.
		auto repeatedOld = MakeData(40);
		auto repeatedNew = MakeData(50);
		cache.Activate(&character, &graph, owner, "idle", &animationB, repeatedOld, generation, &generatorRepeated);
		cache.Activate(&character, &graph, owner, "idle", &animationB, repeatedNew, generation, &generatorRepeated);
		CHECK(Value(cache.Lookup(&character, &graph, "idle")) == 50);
		cache.Deactivate(&character, &graph, "idle", &animationB, generation, &generatorRepeated);
		CHECK(Value(cache.Lookup(&character, &graph, "idle")) == 40);
		cache.Deactivate(&character, &graph, "idle", &animationB, generation, &generatorRepeated);
		CHECK(!cache.Lookup(&character, &graph, "idle"));
	}

	void TestGeneratorRebindRemovesStaleMotion()
	{
		Cache cache;
		int character = 1;
		int graph = 2;
		int animationA = 3;
		int animationB = 4;
		int generatorA = 5;
		int generatorB = 6;
		const auto generation = cache.GetGeneration();
		auto motionA = MakeData(10);
		auto motionB = MakeData(20);
		auto noMotion = MakeData(-1);
		const auto owner = MakeOwner(1);
		auto hasMotion = [](const Data& a_data) { return a_data.value > 0; };
		CHECK(cache.Remember(&animationB, noMotion, generation));
		CHECK(!cache.HasActive(&character, hasMotion));

		cache.Activate(&character, &graph, owner, "clip", &animationA, motionA, generation, &generatorA);
		cache.Activate(&character, &graph, owner, "clip", &animationA, motionB, generation, &generatorB);
		cache.Activate(&character, &graph, owner, "clip", &animationB, motionB, generation, &generatorA);
		CHECK(!cache.FindBinding(&character, &graph, "clip", &animationA, &generatorA).data);
		CHECK(Value(cache.Lookup(&character, &graph, "clip")) == 20);
		CHECK(cache.HasActive(&character, hasMotion));

		// Resetting B must not resurrect A; the other generator remains active.
		cache.Deactivate(&character, &graph, "clip", &animationB, generation, &generatorA);
		CHECK(Value(cache.Lookup(&character, &graph, "clip")) == 20);
		CHECK(cache.HasActive(&character, hasMotion));

		// A no-motion rebind removes all records for that generator while leaving
		// another generator's motion and API-visible active state intact.
		cache.Activate(&character, &graph, owner, "clip", &animationA, motionA, generation, &generatorA);
		cache.DeactivateGenerator(&character, &graph, "clip", generation, &generatorA);
		CHECK(!cache.FindGeneratorBinding(&character, &graph, "clip", &generatorA).data);
		CHECK(Value(cache.Lookup(&character, &graph, "clip")) == 20);
		CHECK(cache.HasActive(&character, hasMotion));

		cache.Deactivate(&character, &graph, "clip", &animationA, generation, &generatorB);
		CHECK(!cache.Lookup(&character, &graph, "clip"));
		CHECK(!cache.HasActive(&character, hasMotion));

		// With no other generator, a no-motion rebind clears the active API result.
		cache.Activate(&character, &graph, owner, "clip", &animationA, motionA, generation, &generatorA);
		CHECK(cache.HasActive(&character, hasMotion));
		cache.DeactivateGenerator(&character, &graph, "clip", generation, &generatorA);
		CHECK(!cache.HasActive(&character, hasMotion));
	}

	void TestGraphReplacementWithNoMotionInvalidatesOldEntry()
	{
		Cache cache;
		int character = 1;
		int oldGraph = 2;
		int newGraph = 3;
		int oldAnimation = 4;
		int generator = 5;
		const auto generation = cache.GetGeneration();
		auto motion = MakeData(10);
		auto hasMotion = [](const Data& a_data) { return a_data.value > 0; };
		bool oldOwnerDestroyed = false;
		std::weak_ptr<const void> oldOwnerLifetime;
		{
			auto oldOwner = MakeOwner(1, [&] {
				CHECK(!cache.HasActive(&character, hasMotion));
				oldOwnerDestroyed = true;
			});
			oldOwnerLifetime = oldOwner;
			cache.Activate(&character, &oldGraph, std::move(oldOwner), "old", &oldAnimation,
				motion, generation, &generator);
		}
		CHECK(cache.HasActive(&character, hasMotion));

		// Processing a no-motion clip on a replacement graph retires the entire
		// old CharacterEntry even though the new clip has no binding to activate.
		const auto retired = cache.DeactivateGenerator(
			&character, &newGraph, "new", generation, &generator);
		CHECK(!retired.empty());
		CHECK(!cache.Lookup(&character, &oldGraph, "old"));
		CHECK(!cache.HasActive(&character, hasMotion));
		CHECK(oldOwnerDestroyed);
		CHECK(oldOwnerLifetime.expired());
	}

	void TestGraphIdentityAndReplacementOwner()
	{
		Cache cache;
		int character = 1;
		int oldGraph = 2;
		int newGraph = 3;
		int oldAnimation = 4;
		int newAnimation = 5;
		const auto generation = cache.GetGeneration();
		bool oldOwnerDestroyed = false;
		std::weak_ptr<const void> oldOwnerLifetime;
		{
			auto owner = MakeOwner(1, [&] {
				oldOwnerDestroyed = true;
				CHECK(cache.GetGeneration() == generation);
				CHECK(Value(cache.Lookup(&character, &newGraph, "walk")) == 2);
			});
			oldOwnerLifetime = owner;
			cache.Activate(&character, &oldGraph, std::move(owner), "walk", &oldAnimation, MakeData(1), generation);
		}
		CHECK(!cache.Lookup(&character, &newGraph, "walk"));
		cache.Deactivate(&character, &newGraph, "walk", &oldAnimation, generation);
		CHECK(Value(cache.Lookup(&character, &oldGraph, "walk")) == 1);

		auto newOwner = MakeOwner(2);
		cache.Activate(&character, &newGraph, std::move(newOwner), "walk", &newAnimation, MakeData(2), generation);
		CHECK(oldOwnerDestroyed);
		CHECK(oldOwnerLifetime.expired());
		CHECK(!cache.Lookup(&character, &oldGraph, "walk"));
		CHECK(Value(cache.Lookup(&character, &newGraph, "walk")) == 2);
		cache.Deactivate(&character, &oldGraph, "walk", &newAnimation, generation);
		CHECK(Value(cache.Lookup(&character, &newGraph, "walk")) == 2);
	}

	void TestStaleGenerationRejectsRememberActivateAndDeactivate()
	{
		Cache cache;
		int character = 1;
		int graph = 2;
		int animation = 3;
		const auto staleGeneration = cache.GetGeneration();
		cache.Clear();
		CHECK(cache.GetGeneration() == staleGeneration + 1);
		CHECK(!cache.Remember(&animation, MakeData(1), staleGeneration));
		cache.Activate(&character, &graph, MakeOwner(1), "walk", &animation, MakeData(1), staleGeneration);
		CHECK(!cache.Lookup(&character, &graph, "walk"));
		cache.Activate(&character, &graph, MakeOwner(2), "walk", &animation, MakeData(2), cache.GetGeneration());
		cache.Deactivate(&character, &graph, "walk", &animation, staleGeneration);
		CHECK(Value(cache.Lookup(&character, &graph, "walk")) == 2);
	}

	void TestDestructorCallbacksRunOutsideLocks()
	{
		// LRU eviction must release the parsed object after parsedMutex unlocks.
		{
			Cache cache;
			std::array<int, Cache::parsedCapacity + 1> animations{};
			std::atomic<bool> evicted = false;
			const auto generation = cache.GetGeneration();
			(void)cache.Remember(&animations[0], MakeData(0), generation);
			(void)cache.Remember(&animations[1], MakeData(1, [&] {
				CHECK(!cache.FindParsed(&animations[1]));
				CHECK(cache.GetGeneration() == generation);
				evicted = true;
			}), generation);
			for (std::size_t i = 2; i < Cache::parsedCapacity; ++i) {
				(void)cache.Remember(&animations[i], MakeData(static_cast<int>(i)), generation);
			}
			CHECK(cache.FindParsed(&animations[0]));
			(void)cache.Remember(&animations[Cache::parsedCapacity], MakeData(32), generation);
			CHECK(evicted.load());
		}

		// Deactivation and Clear release data and graph owners after shard locks.
		{
			Cache cache;
			int character = 1;
			int graph = 2;
			int animation = 3;
			const auto generation = cache.GetGeneration();
			std::atomic<bool> dataDestroyed = false;
			auto data = MakeData(5, [&] {
				CHECK(!cache.Lookup(&character, &graph, "clip"));
				CHECK(cache.GetGeneration() == generation);
				dataDestroyed = true;
			});
			cache.Activate(&character, &graph, MakeOwner(1), "clip", &animation, std::move(data), generation);
			cache.Deactivate(&character, &graph, "clip", &animation, generation);
			CHECK(dataDestroyed.load());

			std::atomic<bool> clearDataDestroyed = false;
			std::atomic<bool> clearOwnerDestroyed = false;
			auto clearData = MakeData(6, [&] {
				CHECK(!cache.Lookup(&character, &graph, "clear"));
				CHECK(cache.GetGeneration() == generation + 1);
				clearDataDestroyed = true;
			});
			auto clearOwner = MakeOwner(2, [&] {
				CHECK(!cache.Lookup(&character, &graph, "clear"));
				CHECK(cache.GetGeneration() == generation + 1);
				clearOwnerDestroyed = true;
			});
			cache.Activate(&character, &graph, std::move(clearOwner), "clear", &animation, std::move(clearData), generation);
			cache.Clear();
			CHECK(clearDataDestroyed.load());
			CHECK(clearOwnerDestroyed.load());
		}
	}

	void TestParallelReadersWritersAndClear()
	{
		Cache cache;
		constexpr std::size_t workerCount = 8;
		constexpr int iterations = 500;
		std::array<int, workerCount> characters{};
		std::array<int, workerCount> graphs{};
		std::array<int, workerCount> animations{};
		std::vector<std::thread> workers;
		for (std::size_t i = 0; i < workerCount; ++i) {
			workers.emplace_back([&, i] {
				for (int n = 0; n < iterations; ++n) {
					const auto generation = cache.GetGeneration();
					auto data = MakeData(static_cast<int>(i * iterations + n));
					cache.Activate(&characters[i], &graphs[i], MakeOwner(static_cast<int>(i)), "clip", &animations[i], data, generation);
					(void)cache.Lookup(&characters[i], &graphs[i], "clip");
					cache.Deactivate(&characters[i], &graphs[i], "clip", &animations[i], generation);
					(void)cache.Remember(&animations[i], data, generation);
					(void)cache.FindParsed(&animations[i]);
				}
			});
		}
		std::thread clearer([&] {
			for (int i = 0; i < 80; ++i) {
				cache.Clear();
			}
		});
		for (auto& worker : workers) {
			worker.join();
		}
		clearer.join();
		cache.Clear();
		for (std::size_t i = 0; i < workerCount; ++i) {
			CHECK(!cache.Lookup(&characters[i], &graphs[i], "clip"));
			CHECK(!cache.FindParsed(&animations[i]));
		}
	}
}

int main()
{
	TestRememberReturnsOriginalSharedObjectForPositiveAndNegativeData();
	TestParsedCacheUsesLruAndCapsAt32();
	TestSnapshotsSurviveDeactivateAndClear();
	TestActiveLookupAndPerActivationRuntime();
	TestActivationCountsWrongAnimationPromotionAndRestore();
	TestGeneratorRebindRemovesStaleMotion();
	TestGraphReplacementWithNoMotionInvalidatesOldEntry();
	TestGraphIdentityAndReplacementOwner();
	TestStaleGenerationRejectsRememberActivateAndDeactivate();
	TestDestructorCallbacksRunOutsideLocks();
	TestParallelReadersWritersAndClear();
	if (failures.load() != 0) {
		std::cerr << failures.load() << " cache test assertion(s) failed\n";
		return 1;
	}
	std::cout << "MotionCache host tests passed\n";
	return 0;
}
