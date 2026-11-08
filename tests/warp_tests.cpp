#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <variant>

namespace RE
{
	struct NiPoint3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	struct NiQuaternion
	{
		float w = 1.0f;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	namespace hkaAnnotationTrack
	{
		struct NullableText
		{
			NullableText() = default;
			NullableText(const char* a_value) : value(a_value) {}

			const char* c_str() const noexcept { return value; }

			const char* value = nullptr;
		};

		struct Annotation
		{
			float time = 0.0f;
			NullableText text;
		};
	}
}

#include "AnimMotionHandler.h"

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			std::cerr << "check failed: " #condition ", file " << __FILE__ << ", line " << __LINE__ << '\n'; \
			std::abort(); \
		} \
	} while (false)

namespace
{
	bool Near(float a_lhs, float a_rhs, float a_tolerance = 1.0e-5f)
	{
		return std::fabs(a_lhs - a_rhs) <= a_tolerance * std::max({ 1.0f, std::fabs(a_lhs), std::fabs(a_rhs) });
	}

	void TestWarpParser()
	{
		const auto defaultsResult = ParseAnnotation("animwarp 0.5 1.25", 2.0f);
		const Warp* defaults = std::get_if<Warp>(&defaultsResult);
		CHECK(defaults);
		CHECK(defaults->time == 2.0f);
		CHECK(defaults->delta.lowerLimit == 0.5f);
		CHECK(defaults->delta.upperLimit == 1.25f);
		CHECK(defaults->delta.maximumAngleDegrees == 60.0f);
		CHECK(std::isinf(defaults->delta.maximumDistance));

		const auto configuredResult = ParseAnnotation("animwarp\t0.75 1.5 135 240", 1.0f);
		const Warp* configured = std::get_if<Warp>(&configuredResult);
		CHECK(configured);
		CHECK(configured->delta.lowerLimit == 0.75f);
		CHECK(configured->delta.upperLimit == 1.5f);
		CHECK(configured->delta.maximumAngleDegrees == 135.0f);
		CHECK(configured->delta.maximumDistance == 240.0f);

		CHECK(std::holds_alternative<WarpEnd>(ParseAnnotation("animwarpend", 1.5f)));
		CHECK(IsWarpControlAnnotation("animwarp 0 1"));
		CHECK(IsWarpControlAnnotation("animwarpend"));
		CHECK(!IsWarpControlAnnotation("animwarper 0 1"));
		CHECK(!IsWarpControlAnnotation(" animwarp 0 1"));

		for (const std::string_view invalid : {
				 "animwarp", "animwarp 0.5", "animwarp -0.1 1", "animwarp 1.1 1",
				 "animwarp 0.5 0.4", "animwarp 0 1 181", "animwarp 0 1 60 -1",
				 "animwarp 0 1 60 2 extra", "animwarpend trailing" }) {
			CHECK(std::holds_alternative<std::monostate>(ParseAnnotation(invalid, 0.5f)));
		}
		CHECK(std::holds_alternative<std::monostate>(ParseAnnotation("animwarp 0 1", std::numeric_limits<float>::infinity())));
	}

	void TestCombatBoundaries()
	{
		CHECK(IsCombatWarpBoundary("hitframe"));
		CHECK(IsCombatWarpBoundary("  HitFrame.Damage"));
		CHECK(IsCombatWarpBoundary("collision_add"));
		CHECK(IsCombatWarpBoundary("COLLISION_ADD.someEvent"));
		CHECK(!IsCombatWarpBoundary("other.hitframe"));
		CHECK(!IsCombatWarpBoundary("prefix_collision_add"));
		CHECK(!IsCombatWarpBoundary(""));
	}

	void AddThreeKeyCurve(AnimMotionData& a_motion)
	{
		a_motion.Add(Translation{ 1.0f, { 10.0f, 0.0f, 0.0f } });
		a_motion.Add(Translation{ 2.0f, { 20.0f, 0.0f, 0.0f } });
		a_motion.Add(Translation{ 3.0f, { 40.0f, 0.0f, 0.0f } });
	}

	void TestExplicitWarpSegments()
	{
		AnimMotionData motion;
		AddThreeKeyCurve(motion);
		const WarpLimits limits{ 0.25f, 1.5f, 90.0f, 400.0f };
		CHECK(motion.Add(Warp{ 0.5f, limits }));
		CHECK(motion.Add(WarpEnd{ 2.5f }));
		motion.Finalize();

		CHECK(motion.hasExplicitWarpControl);
		CHECK(motion.warpSegments.size() == 3);
		const auto& before = motion.warpSegments[0];
		CHECK(before.kind == AnimMotionData::WarpSegmentKind::kInactive);
		CHECK(Near(before.startTime, 0.0f) && Near(before.endTime, 0.5f));
		CHECK(Near(before.startTranslation.x, 0.0f));
		CHECK(Near(before.endTranslation.x, 5.0f));

		const auto& active = motion.warpSegments[1];
		CHECK(active.kind == AnimMotionData::WarpSegmentKind::kExplicit);
		CHECK(Near(active.startTime, 0.5f) && Near(active.endTime, 2.5f));
		CHECK(active.limits.lowerLimit == limits.lowerLimit);
		CHECK(active.limits.upperLimit == limits.upperLimit);
		CHECK(active.limits.maximumAngleDegrees == limits.maximumAngleDegrees);
		CHECK(active.limits.maximumDistance == limits.maximumDistance);
		CHECK(Near(active.startTranslation.x, 5.0f));
		CHECK(Near(active.endTranslation.x, 30.0f));

		const auto& after = motion.warpSegments[2];
		CHECK(after.kind == AnimMotionData::WarpSegmentKind::kInactive);
		CHECK(Near(after.startTime, 2.5f) && Near(after.endTime, 3.0f));
		CHECK(Near(after.startTranslation.x, 30.0f));
		CHECK(Near(after.endTranslation.x, 40.0f));
		CHECK(Near(motion.initialTranslation.x, 0.0f));

		RE::NiPoint3 sampled{};
		CHECK(motion.SampleTranslation(2.5f, sampled));
		CHECK(Near(sampled.x, active.endTranslation.x));
	}

	void TestCombatSegmentsAndDisabledWarp()
	{
		AnimMotionData combat;
		AddThreeKeyCurve(combat);
		combat.AddCombatWarpBoundary(1.2f);
		combat.AddCombatWarpBoundary(1.20005f);
		combat.AddCombatWarpBoundary(2.2f);
		combat.Finalize();

		CHECK(!combat.hasExplicitWarpControl);
		CHECK(combat.combatWarpBoundaries.size() == 2);
		CHECK(combat.warpSegments.size() == 3);
		CHECK(combat.warpSegments[0].kind == AnimMotionData::WarpSegmentKind::kDefaultCombat);
		CHECK(combat.warpSegments[1].kind == AnimMotionData::WarpSegmentKind::kDefaultCombat);
		CHECK(combat.warpSegments[2].kind == AnimMotionData::WarpSegmentKind::kDefaultCombat);
		CHECK(Near(combat.warpSegments[0].startTime, 0.0f));
		CHECK(Near(combat.warpSegments[0].endTime, 1.2f));
		CHECK(Near(combat.warpSegments[1].startTime, 1.2f));
		CHECK(Near(combat.warpSegments[1].endTime, 2.2f));
		CHECK(Near(combat.warpSegments[2].startTime, 2.2f));
		CHECK(Near(combat.warpSegments[2].endTime, 3.0f));

		AnimMotionData disabled;
		AddThreeKeyCurve(disabled);
		CHECK(disabled.Add(Warp{ 1.5f, WarpLimits{ 1.0f, 1.0f, 60.0f, std::numeric_limits<float>::infinity() } }));
		disabled.AddCombatWarpBoundary(1.0f);
		disabled.Finalize();
		CHECK(disabled.hasExplicitWarpControl);
		CHECK(disabled.warpSegments.size() == 2);
		CHECK(disabled.warpSegments[0].kind == AnimMotionData::WarpSegmentKind::kInactive);
		CHECK(disabled.warpSegments[1].kind == AnimMotionData::WarpSegmentKind::kInactive);
	}
}

int main()
{
	TestWarpParser();
	TestCombatBoundaries();
	TestExplicitWarpSegments();
	TestCombatSegmentsAndDisabledWarp();
	std::cout << "warp parser and segment tests passed\n";
}
