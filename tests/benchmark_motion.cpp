#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <random>
#include <string>
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
		struct Annotation
		{
			float time = 0.0f;
			std::string text;
		};
	}
}

#include "AnimMotionHandler.h"

namespace
{
	RE::NiPoint3 OldLinearSample(const std::vector<Translation>& a_list, float a_time)
	{
		const float sampleTime = a_time > a_list.back().time ? a_list.back().time : a_time;
		for (std::size_t index = 0; index < a_list.size(); ++index) {
			if (sampleTime <= a_list[index].time) {
				const float previousTime = index ? a_list[index - 1].time : 0.0f;
				const float duration = a_list[index].time - previousTime;
				const float progress = duration > std::numeric_limits<float>::epsilon() ? (sampleTime - previousTime) / duration : 1.0f;
				const float previousWeight = 1.0f - progress;
				const RE::NiPoint3 previous = index ? a_list[index - 1].delta : RE::NiPoint3{};
				return {
					a_list[index].delta.x * progress + previous.x * previousWeight,
					a_list[index].delta.y * progress + previous.y * previousWeight,
					a_list[index].delta.z * progress + previous.z * previousWeight
				};
			}
		}
		return {};
	}
}

int main()
{
	constexpr std::size_t sampleCount = 300'000;
	std::mt19937 generator{ 0xA11CEu };
	std::uniform_real_distribution<float> timeDistribution{ -0.25f, 1.25f };
	std::vector<float> sampleTimes(sampleCount);
	std::generate(sampleTimes.begin(), sampleTimes.end(), [&] { return timeDistribution(generator); });

	for (const std::size_t keyCount : { 2u, 4u, 8u, 16u, 32u, 64u, 128u }) {
		AnimMotionData curve;
		for (std::size_t index = 0; index < keyCount; ++index) {
			const float time = static_cast<float>(index + 1) / static_cast<float>(keyCount);
			const Translation key{ time, { static_cast<float>(index), static_cast<float>(index * 2), static_cast<float>(index * 3) } };
			curve.Add(&key);
		}
		curve.Finalize();

		double linearChecksum = 0.0;
		const auto linearStart = std::chrono::steady_clock::now();
		for (const float time : sampleTimes) {
			const RE::NiPoint3 sampled = OldLinearSample(curve.translationList, time);
			linearChecksum += static_cast<double>(sampled.x) + static_cast<double>(sampled.y) * 2.0 + static_cast<double>(sampled.z) * 3.0;
		}
		const auto linearEnd = std::chrono::steady_clock::now();

		double newSamplerChecksum = 0.0;
		const auto newSamplerStart = std::chrono::steady_clock::now();
		for (const float time : sampleTimes) {
			RE::NiPoint3 sampled{};
			curve.SampleTranslation(time, sampled);
			newSamplerChecksum += static_cast<double>(sampled.x) + static_cast<double>(sampled.y) * 2.0 + static_cast<double>(sampled.z) * 3.0;
		}
		const auto newSamplerEnd = std::chrono::steady_clock::now();

		const double linearMs = std::chrono::duration<double, std::milli>(linearEnd - linearStart).count();
		const double newSamplerMs = std::chrono::duration<double, std::milli>(newSamplerEnd - newSamplerStart).count();
		std::cout << "keys=" << keyCount << " linear_ms=" << linearMs << " new_sampler_ms=" << newSamplerMs
				  << " checksum_delta=" << std::abs(linearChecksum - newSamplerChecksum) << '\n';
	}
}
