#include "MotionCache.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace
{
	std::atomic<std::ptrdiff_t> allocationsBeforeFailure{ -1 };
	std::atomic<std::size_t> allocationCount{ 0 };
	std::atomic<bool> countAllocations{ false };

	bool ShouldFailAllocation() noexcept
	{
		if (countAllocations.load(std::memory_order_relaxed)) {
			allocationCount.fetch_add(1, std::memory_order_relaxed);
		}

		auto remaining = allocationsBeforeFailure.load(std::memory_order_relaxed);
		while (remaining >= 0) {
			if (remaining == 0) {
				if (allocationsBeforeFailure.compare_exchange_weak(remaining, -1, std::memory_order_relaxed)) {
					return true;
				}
			} else if (allocationsBeforeFailure.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
				break;
			}
		}
		return false;
	}

	void ArmFailure(std::ptrdiff_t a_successfulAllocations) noexcept
	{
		allocationCount.store(0, std::memory_order_relaxed);
		countAllocations.store(true, std::memory_order_relaxed);
		allocationsBeforeFailure.store(a_successfulAllocations, std::memory_order_relaxed);
	}

	std::size_t DisarmFailure() noexcept
	{
		allocationsBeforeFailure.store(-1, std::memory_order_relaxed);
		countAllocations.store(false, std::memory_order_relaxed);
		return allocationCount.load(std::memory_order_relaxed);
	}

	std::atomic<int> failures = 0;

	#define CHECK(condition)                                                                                   \
		do {                                                                                                     \
			if (!(condition)) {                                                                                    \
				std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #condition << '\n';                    \
				++failures;                                                                                          \
			}                                                                                                      \
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

	using Cache = amr::MotionCache<Data>;
	using Snapshot = Cache::Snapshot;

	Snapshot MakeData(int a_value, std::function<void()> a_onDestroy = {})
	{
		return std::make_shared<const Data>(a_value, std::move(a_onDestroy));
	}

	std::shared_ptr<const void> MakeOwner(int a_value)
	{
		return std::shared_ptr<const void>(new int(a_value), [](const void* a_ptr) {
			delete static_cast<const int*>(a_ptr);
		});
	}

	void TestFailedNewCharacterActivationDoesNotRetainOwner()
	{
		constexpr std::string_view longClip = "a deliberately long cache clip name that must allocate storage beyond string small-buffer capacity";
		bool reachedSuccess = false;
		int failedAllocations = 0;
		for (std::ptrdiff_t failAfter = 0; failAfter < 64; ++failAfter) {
			Cache cache;
			int character = 1;
			int graph = 2;
			int animation = 3;
			auto owner = MakeOwner(1);
			std::weak_ptr<const void> ownerLifetime = owner;
			auto data = MakeData(11);
			bool threw = false;

			ArmFailure(failAfter);
			try {
				cache.Activate(&character, &graph, std::move(owner), longClip, &animation, data, cache.GetGeneration());
			} catch (const std::bad_alloc&) {
				threw = true;
			}
			(void)DisarmFailure();

			if (threw) {
				++failedAllocations;
				CHECK(!cache.Lookup(&character, &graph, longClip));
				CHECK(ownerLifetime.expired());
			} else {
				reachedSuccess = true;
				CHECK(cache.Lookup(&character, &graph, longClip) == data);
				cache.Clear();
				CHECK(ownerLifetime.expired());
				break;
			}
		}
		CHECK(failedAllocations > 0);
		CHECK(reachedSuccess);
	}

	void TestFailedGraphReplacementLeavesOldEntryIntact()
	{
		constexpr std::string_view longClip = "a deliberately long replacement clip name that must allocate beyond string small-buffer capacity";
		Cache cache;
		int character = 1;
		int oldGraph = 2;
		int newGraph = 3;
		int oldAnimation = 4;
		int newAnimation = 5;
		auto oldData = MakeData(21);
		auto oldOwner = MakeOwner(1);
		std::weak_ptr<const void> oldOwnerLifetime = oldOwner;
		const auto generation = cache.GetGeneration();
		cache.Activate(&character, &oldGraph, std::move(oldOwner), "old", &oldAnimation, oldData, generation);
		CHECK(cache.Lookup(&character, &oldGraph, "old") == oldData);

		bool reachedSuccess = false;
		int failedAllocations = 0;
		for (std::ptrdiff_t failAfter = 0; failAfter < 64; ++failAfter) {
			auto replacementOwner = MakeOwner(2);
			std::weak_ptr<const void> replacementLifetime = replacementOwner;
			auto replacementData = MakeData(22);
			bool threw = false;
			ArmFailure(failAfter);
			try {
				cache.Activate(&character, &newGraph, std::move(replacementOwner), longClip, &newAnimation,
					replacementData, generation);
			} catch (const std::bad_alloc&) {
				threw = true;
			}
			(void)DisarmFailure();

			if (threw) {
				++failedAllocations;
				CHECK(replacementLifetime.expired());
				CHECK(cache.Lookup(&character, &oldGraph, "old") == oldData);
				CHECK(!cache.Lookup(&character, &newGraph, longClip));
			} else {
				reachedSuccess = true;
				CHECK(oldOwnerLifetime.expired());
				CHECK(cache.Lookup(&character, &newGraph, longClip) == replacementData);
				cache.Clear();
				CHECK(replacementLifetime.expired());
				break;
			}
		}
		CHECK(failedAllocations > 0);
		CHECK(reachedSuccess);
	}

		void TestCharacterRetirementNeedsNoAllocation()
		{
			Cache cache;
			int character = 1;
			int graph = 2;
			int animation = 3;
			const auto generation = cache.GetGeneration();
			bool released = false;
			bool visited = false;
			cache.Activate(&character, &graph, MakeOwner(1), "clip", &animation,
				MakeData(31, [&] {
					CHECK(!cache.Lookup(&character, &graph, "clip"));
					released = true;
				}), generation);
			bool threw = false;
			ArmFailure(0);
			try {
				cache.RetireCharacter(&character, generation, [&](const Cache::RuntimeOwner&) {
					CHECK(!cache.Lookup(&character, &graph, "clip"));
					visited = true;
				});
			} catch (const std::bad_alloc&) {
				threw = true;
			}
			const auto allocations = DisarmFailure();
			CHECK(!threw);
			CHECK(allocations == 0);
			CHECK(visited && released);
			CHECK(!cache.HasActive(&character, [](const Data&) { return true; }));

			cache.Clear();
			auto data = MakeData(32);
			cache.Activate(&character, &graph, MakeOwner(2), "clip", &animation, data, cache.GetGeneration());
			cache.RetireCharacter(&character, generation, [](const Cache::RuntimeOwner&) {});
			CHECK(cache.Lookup(&character, &graph, "clip") == data);
		}

		void TestFailedRebindRetiresOldAnimation()
		{
			Cache cache;
			int character = 1;
			int graph = 2;
			int animationA = 3;
			int animationB = 4;
			int generator = 5;
			const auto generation = cache.GetGeneration();
			cache.Activate(&character, &graph, MakeOwner(1), "clip", &animationA, MakeData(41), generation, &generator);
			bool threw = false;
			ArmFailure(0);
			try {
				auto parsed = MakeData(42);
				cache.Activate(&character, &graph, MakeOwner(2), "clip", &animationB, std::move(parsed), generation, &generator);
			} catch (const std::bad_alloc&) {
				threw = true;
				cache.RetireCharacter(&character, generation, [](const Cache::RuntimeOwner&) {});
			}
			const auto allocations = DisarmFailure();
			CHECK(threw);
			CHECK(allocations == 1);
			CHECK(!cache.Lookup(&character, &graph, "clip"));
			CHECK(!cache.HasActive(&character, [](const Data&) { return true; }));
		}

		std::size_t CountAppendAllocations(std::size_t a_existingBindings)
	{
		Cache cache;
		int character = 1;
		int graph = 2;
		int animation = 3;
		const auto generation = cache.GetGeneration();
		auto owner = MakeOwner(1);
		auto data = MakeData(1);
		cache.Activate(&character, &graph, owner, "clip", &animation, data, generation);
		for (std::size_t i = 1; i < a_existingBindings; ++i) {
			cache.Activate(&character, &graph, owner, "clip", &animation, data, generation);
		}

		auto appendOwner = MakeOwner(2);
		ArmFailure(-1);
		cache.Activate(&character, &graph, std::move(appendOwner), "clip", &animation, data, generation);
		return DisarmFailure();
	}

	void TestFailedExistingClipAppendReentersAfterUnlock()
	{
		Cache cache;
		int character = 1;
		int graph = 2;
		int animation = 3;
		const auto generation = cache.GetGeneration();
		auto owner = MakeOwner(1);
		auto firstData = MakeData(31);
		cache.Activate(&character, &graph, owner, "clip", &animation, firstData, generation);
		std::size_t setupAllocations = static_cast<std::size_t>(-1);
		std::size_t growthLength = 0;
		for (std::size_t count = 1; count < 128; ++count) {
			const auto allocations = CountAppendAllocations(count);
			if (allocations < setupAllocations) {
				setupAllocations = allocations;
			}
			if (allocations > setupAllocations) {
				growthLength = count;
				break;
			}
		}
		CHECK(growthLength > 0);
		if (growthLength == 0) {
			return;
		}
		for (std::size_t i = 1; i < growthLength; ++i) {
			cache.Activate(&character, &graph, owner, "clip", &animation, firstData, generation);
		}
		std::atomic<bool> destructorReentered = false;
		std::atomic<int> valueObservedByDestructor = -1;
		auto failedData = MakeData(32, [&] {
			const auto visible = cache.Lookup(&character, &graph, "clip");
			valueObservedByDestructor.store(visible ? visible->value : -1, std::memory_order_relaxed);
			destructorReentered.store(true, std::memory_order_relaxed);
		});
		std::weak_ptr<const Data> failedDataLifetime = failedData;
		auto failedOwner = MakeOwner(2);
		std::weak_ptr<const void> failedOwnerLifetime = failedOwner;
		bool threw = false;
		ArmFailure(static_cast<std::ptrdiff_t>(setupAllocations));
		try {
			cache.Activate(&character, &graph, std::move(failedOwner), "clip", &animation,
				std::move(failedData), generation);
		} catch (const std::bad_alloc&) {
			threw = true;
		}
		(void)DisarmFailure();
		CHECK(threw);
		CHECK(cache.Lookup(&character, &graph, "clip") == firstData);
		CHECK(failedDataLifetime.expired());
		CHECK(failedOwnerLifetime.expired());
		CHECK(destructorReentered.load(std::memory_order_relaxed));
		CHECK(valueObservedByDestructor.load(std::memory_order_relaxed) == firstData->value);
	}
}

int main()
{
	TestFailedNewCharacterActivationDoesNotRetainOwner();
	TestFailedGraphReplacementLeavesOldEntryIntact();
	TestFailedExistingClipAppendReentersAfterUnlock();
	TestCharacterRetirementNeedsNoAllocation();
	TestFailedRebindRetiresOldAnimation();
	if (failures.load() != 0) {
		std::cerr << failures << " cache exception test(s) failed\n";
		return 1;
	}
	std::cout << "MotionCache allocation-failure tests passed\n";
	return 0;
}

void* operator new(std::size_t a_size)
{
	if (ShouldFailAllocation()) {
		throw std::bad_alloc{};
	}
	if (void* result = std::malloc(a_size ? a_size : 1)) {
		return result;
	}
	throw std::bad_alloc{};
}

void* operator new[](std::size_t a_size)
{
	return ::operator new(a_size);
}

void operator delete(void* a_ptr) noexcept
{
	std::free(a_ptr);
}

void operator delete[](void* a_ptr) noexcept
{
	std::free(a_ptr);
}

void operator delete(void* a_ptr, std::size_t) noexcept
{
	std::free(a_ptr);
}

void operator delete[](void* a_ptr, std::size_t) noexcept
{
	std::free(a_ptr);
}
