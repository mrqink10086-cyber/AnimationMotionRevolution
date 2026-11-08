#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
	constexpr float kTolerance = 2.0e-5f;

	bool SameFloat(float a_lhs, float a_rhs)
	{
		if (std::isnan(a_lhs) || std::isnan(a_rhs)) {
			return std::isnan(a_lhs) && std::isnan(a_rhs);
		}
		return std::bit_cast<std::uint32_t>(a_lhs) == std::bit_cast<std::uint32_t>(a_rhs);
	}

	bool SamePoint(const RE::NiPoint3& a_lhs, const RE::NiPoint3& a_rhs)
	{
		return SameFloat(a_lhs.x, a_rhs.x) && SameFloat(a_lhs.y, a_rhs.y) && SameFloat(a_lhs.z, a_rhs.z);
	}

	bool SameQuaternion(const RE::NiQuaternion& a_lhs, const RE::NiQuaternion& a_rhs)
	{
		return SameFloat(a_lhs.w, a_rhs.w) && SameFloat(a_lhs.x, a_rhs.x) &&
			SameFloat(a_lhs.y, a_rhs.y) && SameFloat(a_lhs.z, a_rhs.z);
	}

	RE::NiQuaternion OriginalYaw(float a_yawDegrees)
	{
		const float roll = 0.0f;
		const float pitch = 0.0f;
		const float yaw = a_yawDegrees * std::numbers::pi_v<float> / 180.0f;
		return {
			std::cos(roll / 2) * std::cos(pitch / 2) * std::cos(yaw / 2) + std::sin(roll / 2) * std::sin(pitch / 2) * std::sin(yaw / 2),
			std::sin(roll / 2) * std::cos(pitch / 2) * std::cos(yaw / 2) - std::cos(roll / 2) * std::sin(pitch / 2) * std::sin(yaw / 2),
			std::cos(roll / 2) * std::sin(pitch / 2) * std::cos(yaw / 2) + std::sin(roll / 2) * std::cos(pitch / 2) * std::sin(yaw / 2),
			std::cos(roll / 2) * std::cos(pitch / 2) * std::sin(yaw / 2) - std::sin(roll / 2) * std::sin(pitch / 2) * std::cos(yaw / 2)
		};
	}

	bool Near(float a_lhs, float a_rhs, float a_tolerance = kTolerance)
	{
		const float scale = std::max({ 1.0f, std::fabs(a_lhs), std::fabs(a_rhs) });
		return std::fabs(a_lhs - a_rhs) <= a_tolerance * scale;
	}

	bool Near(const RE::NiPoint3& a_lhs, const RE::NiPoint3& a_rhs, float a_tolerance = kTolerance)
	{
		return Near(a_lhs.x, a_rhs.x, a_tolerance) && Near(a_lhs.y, a_rhs.y, a_tolerance) && Near(a_lhs.z, a_rhs.z, a_tolerance);
	}

	bool Near(const RE::NiQuaternion& a_lhs, const RE::NiQuaternion& a_rhs, float a_tolerance = kTolerance)
	{
		return Near(a_lhs.w, a_rhs.w, a_tolerance) && Near(a_lhs.x, a_rhs.x, a_tolerance) &&
			Near(a_lhs.y, a_rhs.y, a_tolerance) && Near(a_lhs.z, a_rhs.z, a_tolerance);
	}

	template <class MotionT>
	float OldProgress(const std::vector<MotionT>& a_list, float a_time, std::size_t a_index)
	{
		const float previousTime = a_index ? a_list[a_index - 1].time : 0.0f;
		const float duration = a_list[a_index].time - previousTime;
		if (duration > std::numeric_limits<float>::epsilon()) {
			return (a_time - previousTime) / duration;
		}
		return 1.0f;
	}

	struct RotationSampleArguments
	{
		float progress;
		RE::NiQuaternion previous;
		RE::NiQuaternion current;
	};

	RotationSampleArguments OldRotationArguments(const std::vector<Rotation>& a_list, float a_time)
	{
		const float clampedTime = a_time > a_list.back().time ? a_list.back().time : a_time;
		for (std::size_t index = 0; index < a_list.size(); ++index) {
			if (clampedTime <= a_list[index].time) {
				const RE::NiQuaternion previous = index ? a_list[index - 1].delta : RE::NiQuaternion{};
				return { OldProgress(a_list, clampedTime, index), previous, a_list[index].delta };
			}
		}
		return { 1.0f, {}, {} };
	}

	RE::NiPoint3 OldTranslationSample(const std::vector<Translation>& a_list, float a_time)
	{
		if (std::isnan(a_time)) {
			return {};
		}
		const float clampedTime = a_time > a_list.back().time ? a_list.back().time : a_time;
		for (std::size_t index = 0; index < a_list.size(); ++index) {
			if (clampedTime <= a_list[index].time) {
				const RE::NiPoint3 previous = index ? a_list[index - 1].delta : RE::NiPoint3{};
				const float progress = OldProgress(a_list, clampedTime, index);
				const float previousWeight = 1.0f - progress;
				return {
					a_list[index].delta.x * progress + previous.x * previousWeight,
					a_list[index].delta.y * progress + previous.y * previousWeight,
					a_list[index].delta.z * progress + previous.z * previousWeight
				};
			}
		}
		return {};
	}

	RE::NiQuaternion OldRotationSample(const std::vector<Rotation>& a_list, float a_time)
	{
		if (std::isnan(a_time)) {
			return {};
		}
		const float clampedTime = a_time > a_list.back().time ? a_list.back().time : a_time;
		for (std::size_t index = 0; index < a_list.size(); ++index) {
			if (clampedTime <= a_list[index].time) {
				const RE::NiQuaternion previous = index ? a_list[index - 1].delta : RE::NiQuaternion{};
				const float progress = OldProgress(a_list, clampedTime, index);
				const float previousWeight = 1.0f - progress;
				return {
					a_list[index].delta.w * progress + previous.w * previousWeight,
					a_list[index].delta.x * progress + previous.x * previousWeight,
					a_list[index].delta.y * progress + previous.y * previousWeight,
					a_list[index].delta.z * progress + previous.z * previousWeight
				};
			}
		}
		return {};
	}

	void TestParser()
	{
		const auto translationResult = ParseAnnotation("animmotion 1.25 -2 3.5", 0.75f);
		const Translation* translation = std::get_if<Translation>(&translationResult);
		CHECK(translation);
		CHECK(translation->time == 0.75f);
		CHECK(translation->delta.x == 1.25f && translation->delta.y == -2.0f && translation->delta.z == 3.5f);

		const auto spacedResult = ParseAnnotation("animmotion 1\t2\n3  ", 0.0f);
		CHECK(std::get_if<Translation>(&spacedResult));

		const auto rotationResult = ParseAnnotation("animrotation 90", 1.0f);
		const Rotation* rotation = std::get_if<Rotation>(&rotationResult);
		CHECK(rotation);
		CHECK(Near(rotation->delta.w, std::sqrt(0.5f)));
		CHECK(rotation->delta.x == 0.0f && rotation->delta.y == 0.0f);
		CHECK(Near(rotation->delta.z, std::sqrt(0.5f)));
		CHECK(SameQuaternion(rotation->delta, OriginalYaw(90.0f)));

		const auto negativeZeroResult = ParseAnnotation("animrotation -0", 0.0f);
		const Rotation* negativeZeroRotation = std::get_if<Rotation>(&negativeZeroResult);
		CHECK(negativeZeroRotation);
		CHECK(SameQuaternion(negativeZeroRotation->delta, OriginalYaw(-0.0f)));

		std::mt19937 yawGenerator{ 0x9071u };
		std::uniform_real_distribution<float> yawDegreesDistribution{ -1.0e20f, 1.0e20f };
		for (int sample = 0; sample < 1000; ++sample) {
			const float yawDegrees = yawDegreesDistribution(yawGenerator);
			std::array<char, 64> yawBuffer{};
			const auto written = std::to_chars(yawBuffer.data(), yawBuffer.data() + yawBuffer.size(), yawDegrees,
				std::chars_format::general, std::numeric_limits<float>::max_digits10);
			CHECK(written.ec == std::errc{});
			std::string text = "animrotation ";
			text.append(yawBuffer.data(), written.ptr);
			float parsedDegrees = 0.0f;
			const auto parsed = std::from_chars(text.data() + 13, text.data() + text.size(), parsedDegrees);
			CHECK(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size());
			const auto result = ParseAnnotation(text, 0.25f);
			const Rotation* sampleRotation = std::get_if<Rotation>(&result);
			CHECK(sampleRotation);
			CHECK(SameQuaternion(sampleRotation->delta, OriginalYaw(parsedDegrees)));
		}

		for (const std::string_view invalid : {
			"animmotion 1 2", "animmotion 1 2 3 4", "animmotion x 2 3", "animmotion 1 inf 3",
			"animmotion 1 2 nan", "animrotation", "animrotation 90 trailing", "animrotation 1e999" }) {
			CHECK(std::holds_alternative<std::monostate>(ParseAnnotation(invalid, 0.5f)));
		}

		std::array<char, 64> maxYawBuffer{};
		const auto maxYawWritten = std::to_chars(maxYawBuffer.data(), maxYawBuffer.data() + maxYawBuffer.size(),
			std::numeric_limits<float>::max(), std::chars_format::general, std::numeric_limits<float>::max_digits10);
		CHECK(maxYawWritten.ec == std::errc{});
		std::string maxYawText = "animrotation ";
		maxYawText.append(maxYawBuffer.data(), maxYawWritten.ptr);
		CHECK(std::holds_alternative<std::monostate>(ParseAnnotation(maxYawText, 0.5f)));

		CHECK(std::holds_alternative<std::monostate>(ParseAnnotation("animmotion 1 2 3", std::numeric_limits<float>::infinity())));
		CHECK(std::holds_alternative<std::monostate>(ParseAnnotation("unrelated 1", 0.5f)));

		RE::hkaAnnotationTrack::Annotation annotation{ 2.0f, "animmotion 4 5 6" };
		const auto annotationResult = ParseAnnotation(annotation);
		CHECK(std::get_if<Translation>(&annotationResult));

		RE::hkaAnnotationTrack::Annotation nullTextAnnotation{ 2.0f, nullptr };
		CHECK(std::holds_alternative<std::monostate>(ParseAnnotation(nullTextAnnotation)));
	}

	void TestExplicitBoundaries()
	{
		AnimMotionData empty;
		RE::NiPoint3 translationOut{ 8.0f, 8.0f, 8.0f };
		CHECK(!empty.SampleTranslation(0.0f, translationOut));
		RE::NiQuaternion rotationOut{};
		CHECK(!empty.SampleRotation(0.0f, rotationOut, [](auto&, float, const auto&, const auto&) {}));

		AnimMotionData curve;
		const Translation last{ 2.0f, { 30.0f, 0.0f, 0.0f } };
		const Translation firstAtTime{ 1.0f, { 10.0f, 0.0f, 0.0f } };
		const Translation secondAtTime{ 1.0f, { 20.0f, 0.0f, 0.0f } };
		curve.Add(&last);
		curve.Add(&firstAtTime);
		curve.Add(&secondAtTime);
		curve.Finalize();
		CHECK(curve.translationList[0].delta.x == 10.0f);
		CHECK(curve.translationList[1].delta.x == 20.0f);

		CHECK(curve.SampleTranslation(1.0f, translationOut));
		CHECK(translationOut.x == 10.0f);
		CHECK(curve.SampleTranslation(-1.0f, translationOut));
		CHECK(translationOut.x == -10.0f);
		CHECK(curve.SampleTranslation(10.0f, translationOut));
		CHECK(translationOut.x == 30.0f);
		CHECK(curve.SampleTranslation(std::numeric_limits<float>::quiet_NaN(), translationOut));
		CHECK(Near(translationOut, {}));

		AnimMotionData epsilon;
		const Translation epsilonFirst{ std::numeric_limits<float>::epsilon() / 2.0f, { 5.0f, 0.0f, 0.0f } };
		const Translation epsilonSecond{ 1.0f, { 15.0f, 0.0f, 0.0f } };
		epsilon.Add(&epsilonFirst);
		epsilon.Add(&epsilonSecond);
		epsilon.Finalize();
		CHECK(epsilon.SampleTranslation(0.0f, translationOut));
		CHECK(translationOut.x == 5.0f);
	}

	void TestRandomizedSampling()
	{
		std::mt19937 generator{ 0x41A77u };
		std::uniform_real_distribution<float> stepDistribution{ 0.0f, 0.8f };
		std::uniform_real_distribution<float> valueDistribution{ -100.0f, 100.0f };
		std::uniform_real_distribution<float> timeDistribution{ -2.0f, 12.0f };

		for (int run = 0; run < 100; ++run) {
			AnimMotionData curve;
			float time = 0.0f;
			const int keyCount = 1 + static_cast<int>(generator() % 160);
			for (int key = 0; key < keyCount; ++key) {
				time += stepDistribution(generator);
				const Translation translation{ time, { valueDistribution(generator), valueDistribution(generator), valueDistribution(generator) } };
				const Rotation rotation{ time, { valueDistribution(generator), valueDistribution(generator), valueDistribution(generator), valueDistribution(generator) } };
				curve.Add(&translation);
				curve.Add(&rotation);
			}
			curve.Finalize();

			for (int sample = 0; sample < 200; ++sample) {
				const float sampleTime = sample % 4 == 0 ? -1.0f : sample % 4 == 1 ? time + 1.0f : timeDistribution(generator);
				RE::NiPoint3 actualTranslation{};
				CHECK(curve.SampleTranslation(sampleTime, actualTranslation));
				const RE::NiPoint3 expectedTranslation = OldTranslationSample(curve.translationList, sampleTime);
				CHECK(SamePoint(actualTranslation, expectedTranslation));

				RE::NiQuaternion actualRotation{};
				const RotationSampleArguments expectedArguments = OldRotationArguments(curve.rotationList, sampleTime);
				const bool foundRotation = curve.SampleRotation(sampleTime, actualRotation,
					[&](RE::NiQuaternion& a_out, float a_progress, const RE::NiQuaternion& a_previous, const RE::NiQuaternion& a_current) {
						CHECK(SameFloat(a_progress, expectedArguments.progress));
						CHECK(SameQuaternion(a_previous, expectedArguments.previous));
						CHECK(SameQuaternion(a_current, expectedArguments.current));
					const float previousWeight = 1.0f - a_progress;
					a_out = {
						a_current.w * a_progress + a_previous.w * previousWeight,
						a_current.x * a_progress + a_previous.x * previousWeight,
						a_current.y * a_progress + a_previous.y * previousWeight,
						a_current.z * a_progress + a_previous.z * previousWeight
					};
					});
				CHECK(foundRotation);
				CHECK(SameQuaternion(actualRotation, OldRotationSample(curve.rotationList, sampleTime)));
			}
		}
	}

	void TestSortingCompatibility()
	{
		AnimMotionData curve;
		std::vector<Translation> expected;
		for (int index = 0; index < 128; ++index) {
			const Translation key{ static_cast<float>((index * 13) % 7), { static_cast<float>(index), 0.0f, 0.0f } };
			expected.push_back(key);
			curve.Add(&key);
		}
		std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
		curve.Finalize();
		for (std::size_t index = 0; index < expected.size(); ++index) {
			CHECK(SameFloat(curve.translationList[index].time, expected[index].time));
			CHECK(SamePoint(curve.translationList[index].delta, expected[index].delta));
		}
		for (int sample = -1; sample < 32; ++sample) {
			RE::NiPoint3 actual{};
			const float time = static_cast<float>(sample) / 4.0f;
			CHECK(curve.SampleTranslation(time, actual));
			CHECK(SamePoint(actual, OldTranslationSample(expected, time)));
		}
	}

	void TestLongTracks()
	{
		for (const std::size_t keyCount : { 65u, 128u, 256u }) {
			AnimMotionData curve;
			for (std::size_t index = 0; index < keyCount; ++index) {
				const float time = static_cast<float>(index + 1) * 0.125f;
				const Translation translation{ time, { static_cast<float>(index) * 0.25f, -static_cast<float>(index), static_cast<float>(index) * 3.0f } };
				const Rotation rotation{ time, { static_cast<float>(index) * 0.5f, static_cast<float>(index), -static_cast<float>(index) * 2.0f, static_cast<float>(index) * 4.0f } };
				curve.Add(&translation);
				curve.Add(&rotation);
			}
			curve.Finalize();

			const float endTime = curve.translationList.back().time;
			for (int sample = 0; sample <= 1024; ++sample) {
				const float sampleTime = -1.0f + (endTime + 2.0f) * (static_cast<float>(sample) / 1024.0f);
				RE::NiPoint3 actualTranslation{};
				CHECK(curve.SampleTranslation(sampleTime, actualTranslation));
				CHECK(SamePoint(actualTranslation, OldTranslationSample(curve.translationList, sampleTime)));

				const RotationSampleArguments expectedArguments = OldRotationArguments(curve.rotationList, sampleTime);
				RE::NiQuaternion actualRotation{};
				CHECK(curve.SampleRotation(sampleTime, actualRotation,
					[&](RE::NiQuaternion& a_out, float a_progress, const RE::NiQuaternion& a_previous, const RE::NiQuaternion& a_current) {
						CHECK(SameFloat(a_progress, expectedArguments.progress));
						CHECK(SameQuaternion(a_previous, expectedArguments.previous));
						CHECK(SameQuaternion(a_current, expectedArguments.current));
						const float previousWeight = 1.0f - a_progress;
						a_out = {
							a_current.w * a_progress + a_previous.w * previousWeight,
							a_current.x * a_progress + a_previous.x * previousWeight,
							a_current.y * a_progress + a_previous.y * previousWeight,
							a_current.z * a_progress + a_previous.z * previousWeight
						};
					}));
				CHECK(SameQuaternion(actualRotation, OldRotationSample(curve.rotationList, sampleTime)));
			}
		}
	}
}

int main()
{
	TestParser();
	TestExplicitBoundaries();
	TestRandomizedSampling();
	TestSortingCompatibility();
	TestLongTracks();
	std::cout << "motion parser and sampling tests passed\n";
}
