#include "MotionCache.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
	constexpr std::size_t kCharacterCount = 256;
	constexpr std::size_t kThreadCountFour = 4;
	constexpr std::size_t kLookupsPerThread = 200'000;
	constexpr std::size_t kWarmupLookupsPerThread = 20'000;

	const std::string kClipName(64, 'x');

	struct Payload
	{
		explicit Payload(std::uint64_t a_value) : value(a_value) {}
		std::uint64_t value;
	};

	using Cache = amr::MotionCache<Payload>;
	using Clock = std::chrono::steady_clock;

	class OriginalShape
	{
	public:
		using ClipMap = std::map<std::string, std::uint64_t, std::less<>>;
		using CharacterMap = std::map<const void*, ClipMap, std::less<const void*>>;

		void Insert(const void* a_character, std::string a_clip, std::uint64_t a_value)
		{
			std::lock_guard guard(mutex);
			characters[a_character].emplace(std::move(a_clip), a_value);
		}

		std::uint64_t Lookup(const void* a_character, const char* a_clip) const
		{
			std::lock_guard guard(mutex);
			const auto character = characters.find(a_character);
			if (character == characters.end()) {
				return 0;
			}
			const auto clip = character->second.find(std::string(a_clip));
			return clip == character->second.end() ? 0 : clip->second;
		}

	private:
		mutable std::mutex mutex;
		CharacterMap characters;
	};

	struct RunResult
	{
		double milliseconds = 0.0;
		std::uint64_t checksum = 0;
	};

	template <class Lookup>
	RunResult RunLookups(std::size_t a_threadCount, std::size_t a_iterationsPerThread, Lookup&& a_lookup)
	{
		std::array<std::uint64_t, kThreadCountFour> sums{};
		std::array<Clock::time_point, kThreadCountFour> finished{};
		Clock::time_point started{};
		std::barrier startLine(static_cast<std::ptrdiff_t>(a_threadCount + 1), [&]() noexcept {
			started = Clock::now();
		});
		std::vector<std::thread> workers;
		workers.reserve(a_threadCount);
		const auto charactersPerThread = kCharacterCount / a_threadCount;
		for (std::size_t threadIndex = 0; threadIndex < a_threadCount; ++threadIndex) {
			workers.emplace_back([&, threadIndex] {
				startLine.arrive_and_wait();
				const auto rangeStart = threadIndex * charactersPerThread;
				for (std::size_t i = 0; i < a_iterationsPerThread; ++i) {
					sums[threadIndex] += a_lookup(rangeStart + (i % charactersPerThread));
				}
				finished[threadIndex] = Clock::now();
			});
		}
		startLine.arrive_and_wait();
		for (auto& worker : workers) {
			worker.join();
		}
		const auto lastFinished = *std::max_element(finished.begin(), finished.begin() + a_threadCount);
		const auto elapsed = std::chrono::duration<double, std::milli>(lastFinished - started).count();
		std::uint64_t checksum = 0;
		for (std::size_t i = 0; i < a_threadCount; ++i) {
			checksum += sums[i];
		}
		return { elapsed, checksum };
	}

	std::uint64_t ExpectedChecksum(std::size_t a_threadCount, std::size_t a_iterationsPerThread)
	{
		const auto charactersPerThread = kCharacterCount / a_threadCount;
		std::uint64_t result = 0;
		for (std::size_t threadIndex = 0; threadIndex < a_threadCount; ++threadIndex) {
			const auto rangeStart = threadIndex * charactersPerThread;
			for (std::size_t i = 0; i < a_iterationsPerThread; ++i) {
				result += 1000 + rangeStart + (i % charactersPerThread);
			}
		}
		return result;
	}

	void PrintResult(std::string_view a_name, const RunResult& a_result)
	{
		std::cout << "  " << a_name << ": " << a_result.milliseconds << " ms, checksum " << a_result.checksum << '\n';
	}
}

int main()
{
	std::array<int, kCharacterCount> characters{};
	int graphIdentity = 0;
	int animationIdentity = 0;
	OriginalShape original;
	Cache cache;
	for (std::size_t i = 0; i < kCharacterCount; ++i) {
		const auto value = 1000 + i;
		original.Insert(&characters[i], kClipName, value);
		auto payload = std::make_shared<const Payload>(value);
		cache.RegisterCharacter(&characters[i]);
		cache.Activate(cache.Begin(&characters[i], &graphIdentity), kClipName, &animationIdentity, std::move(payload));
	}

	std::cout << "Synthetic host cache lookup benchmark; no FPS claims\n";
	std::cout << "Characters: " << kCharacterCount << ", clip-name bytes: " << kClipName.size()
			  << ", measured lookups per thread: " << kLookupsPerThread
			  << ", warmup lookups per thread: " << kWarmupLookupsPerThread << '\n';
	std::cout << "Original shape: nested std::map, global mutex, temporary std::string, one find\n";
	std::cout << "MotionCache shape: sharded lookup returning a shared snapshot\n";
	std::cout << std::fixed << std::setprecision(3);

	for (const auto threadCount : { std::size_t{ 1 }, kThreadCountFour }) {
		const auto originalLookup = [&](std::size_t a_characterIndex) {
			return original.Lookup(&characters[a_characterIndex], kClipName.c_str());
		};
		const auto cacheLookup = [&](std::size_t a_characterIndex) {
			const auto snapshot = cache.Lookup(&characters[a_characterIndex], &graphIdentity, kClipName);
			return snapshot ? snapshot->value : std::uint64_t{ 0 };
		};

		const auto warmOriginal = RunLookups(threadCount, kWarmupLookupsPerThread, originalLookup);
		const auto warmCache = RunLookups(threadCount, kWarmupLookupsPerThread, cacheLookup);
		const auto resultOriginal = RunLookups(threadCount, kLookupsPerThread, originalLookup);
		const auto resultCache = RunLookups(threadCount, kLookupsPerThread, cacheLookup);
		const auto expected = ExpectedChecksum(threadCount, kLookupsPerThread);
		if (warmOriginal.checksum != warmCache.checksum || warmOriginal.checksum != ExpectedChecksum(threadCount, kWarmupLookupsPerThread) ||
			resultOriginal.checksum != resultCache.checksum || resultOriginal.checksum != expected) {
			std::cerr << "Checksum mismatch in " << threadCount << "-thread case\n";
			return 1;
		}

		std::cout << "\n" << threadCount << " thread" << (threadCount == 1 ? "" : "s")
				  << " (disjoint character ranges)\n";
		PrintResult("original-map", resultOriginal);
		PrintResult("MotionCache", resultCache);
		std::cout << "  original-map / MotionCache: " << (resultOriginal.milliseconds / resultCache.milliseconds) << "x\n";
	}
	return 0;
}
