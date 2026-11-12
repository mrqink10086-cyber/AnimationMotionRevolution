#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace RE
{
	struct NiPoint3 { float x{}, y{}, z{}; };
	struct NiQuaternion { float w{ 1.0F }, x{}, y{}, z{}; };
	struct hkaAnnotationTrack
	{
		struct Text
		{
			std::string value;
			const char* c_str() const { return value.c_str(); }
		};
		struct Annotation { float time{}; Text text; };
		std::vector<Annotation> annotations;
	};
	struct hkaAnimation { std::vector<hkaAnnotationTrack> annotationTracks; };
	inline std::unordered_set<const void*> unloadedAnimations;
	inline int referenceCalls = 0;
	inline int expiredReferenceCalls = 0;

	template <class T>
	class hkRefPtr
	{
	public:
		explicit hkRefPtr(T* a_ptr) : ptr(a_ptr) { Record(); }
		~hkRefPtr() { Record(); }
		T* get() const { return ptr; }
	private:
		void Record()
		{
			++referenceCalls;
			if (unloadedAnimations.contains(ptr)) {
				++expiredReferenceCalls;
			}
		}
		T* ptr;
	};
}

#include "MotionCache.h"
#include "ParsedAnimation.h"

#define CHECK(condition) do { if (!(condition)) { \
	std::cerr << "check failed: " #condition " at line " << __LINE__ << '\n'; std::exit(1); \
} } while (false)

namespace
{
	using Cache = amr::MotionCache<amr::ParsedAnimation>;

	std::shared_ptr<const amr::ParsedAnimation> Parse(const RE::hkaAnimation& a_animation)
	{
		return amr::ParseAnimation(a_animation, 1.0e-5F, [](const auto&, auto, bool) { CHECK(false); });
	}

	RE::hkaAnimation Source(float a_distance)
	{
		RE::hkaAnimation result;
		result.annotationTracks.push_back({ { { 1.0F, { "animmotion " + std::to_string(a_distance) + " 0 2" } },
			{ 1.0F, { "animrotation 90" } }, { 0.0F, { "animwarp 0.5 1.5" } }, { 1.0F, { "animwarpend" } } } });
		return result;
	}

	void CheckMotion(const std::shared_ptr<const amr::ParsedAnimation>& a_parsed, float a_distance)
	{
		CHECK(a_parsed);
		CHECK(a_parsed->hasVerticalMotion);
		RE::NiPoint3 sample{};
		CHECK(a_parsed->motion.SampleTranslation(1.0F, sample));
		CHECK(sample.x == a_distance && sample.y == 0.0F && sample.z == 2.0F);
		CHECK(a_parsed->motion.rotationList.size() == 1);
		CHECK(!a_parsed->motion.warpSegments.empty());
	}

	void TestUnloadedAnimationIsNotReferencedByChurnDeactivateOrClear()
	{
		RE::unloadedAnimations.clear();
		Cache cache;
		std::array<int, 64> characters{}, graphs{}, generators{};
		std::vector<std::shared_ptr<const amr::ParsedAnimation>> held;
		for (std::size_t i = 0; i < characters.size(); ++i) {
			auto source = std::make_unique<RE::hkaAnimation>(Source(static_cast<float>(i + 1)));
			RE::unloadedAnimations.erase(source.get());
			const auto identity = source.get();
			auto snapshot = Parse(*source);
			CheckMotion(snapshot, static_cast<float>(i + 1));
			cache.RegisterCharacter(&characters[i]);
			const auto ticket = cache.Begin(&characters[i], &graphs[i]);
			cache.Activate(ticket, "clip", identity, snapshot, &generators[i]);
			RE::unloadedAnimations.insert(identity);
			source.reset();
			CheckMotion(cache.Lookup(&characters[i], &graphs[i], "clip"), static_cast<float>(i + 1));
			held.push_back(std::move(snapshot));
			if (i % 2 == 0) {
				cache.Deactivate(ticket, "clip", identity, &generators[i]);
				CHECK(!cache.Lookup(&characters[i], &graphs[i], "clip"));
			}
		}
		cache.Clear();
		for (std::size_t i = 0; i < held.size(); ++i) {
			CheckMotion(held[i], static_cast<float>(i + 1));
		}
		held.clear();
		CHECK(RE::expiredReferenceCalls == 0);
		CHECK(RE::referenceCalls == 0);
	}

	void TestSameAddressLoadsFreshAnnotations()
	{
		Cache cache;
		int character{}, graph{}, generator{};
		alignas(RE::hkaAnimation) std::byte storage[sizeof(RE::hkaAnimation)];
		auto source = std::construct_at(reinterpret_cast<RE::hkaAnimation*>(storage), Source(4.0F));
		RE::unloadedAnimations.erase(source);
		auto old = Parse(*source);
		cache.RegisterCharacter(&character);
		const auto ticket = cache.Begin(&character, &graph);
		cache.Activate(ticket, "clip", source, old, &generator);
		RE::unloadedAnimations.insert(source);
		std::destroy_at(source);
		source = std::construct_at(reinterpret_cast<RE::hkaAnimation*>(storage), Source(12.0F));
		RE::unloadedAnimations.erase(source);
		cache.Activate(ticket, "clip", source, Parse(*source), &generator);
		CheckMotion(cache.Lookup(&character, &graph, "clip"), 12.0F);
		CheckMotion(old, 4.0F);
		RE::unloadedAnimations.insert(source);
		std::destroy_at(source);
		cache.DeactivateGenerator(ticket, "clip", &generator);
		CHECK(!cache.Lookup(&character, &graph, "clip"));
		old.reset();
		CHECK(RE::referenceCalls == 0);
	}
}

int main()
{
	TestUnloadedAnimationIsNotReferencedByChurnDeactivateOrClear();
	TestSameAddressLoadsFreshAnnotations();
	std::cout << "animation snapshot lifetime tests passed\n";
}
