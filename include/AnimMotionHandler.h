#pragma once

#include "Motion.h"
#include "TranslationMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

class AnimMotionData
{
public:
	AnimMotionData() = default;

	void Add(const Translation* a_translation)
	{
		if (!finalized_ && a_translation) {
			translationList.push_back(*a_translation);
		}
	}

	void Add(const Rotation* a_rotation)
	{
		if (!finalized_ && a_rotation) {
			rotationList.push_back(*a_rotation);
		}
	}

	// Call Finalize() after Add() and before sampling.
	void Finalize()
	{
		if (finalized_) {
			return;
		}

		FinalizeList(translationList);
		FinalizeList(rotationList);
		finalized_ = true;
	}

	bool SampleTranslation(float a_time, RE::NiPoint3& a_out) const
	{
		if (translationList.empty()) {
			return false;
		}

		if (std::isnan(a_time)) {
			a_out = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
			return true;
		}

		const float sampleTime = a_time > translationList.back().time ? translationList.back().time : a_time;
		const auto writeSample = [&](std::size_t a_index, const Translation& a_current) {
			const float previousTime = a_index ? translationList[a_index - 1].time : 0.0f;
			const RE::NiPoint3 previous = a_index ? translationList[a_index - 1].delta : RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
			const float progress = SegmentProgress(sampleTime, previousTime, a_current.time - previousTime);
			amr::InterpolateTranslation(a_current.delta, previous, progress, a_out);
		};

		if (translationList.size() <= kLinearSearchMaxKeys) {
			for (std::size_t index = 0; index < translationList.size(); ++index) {
				const Translation& current = translationList[index];
				if (sampleTime <= current.time) {
					writeSample(index, current);
					return true;
				}
			}
			writeSample(translationList.size() - 1, translationList.back());
			return true;
		}

		const auto segment = std::lower_bound(translationList.begin(), translationList.end(), sampleTime,
			[](const Translation& a_motion, float a_value) {
				return a_motion.time < a_value;
			});
		const std::size_t index = static_cast<std::size_t>(segment - translationList.begin());
		writeSample(index, translationList[index]);
		return true;
	}

	template <class Interpolator>
	bool SampleRotation(float a_time, RE::NiQuaternion& a_out, Interpolator&& a_interpolator) const
	{
		if (rotationList.empty()) {
			return false;
		}

		if (std::isnan(a_time)) {
			a_out = RE::NiQuaternion{ 1.0f, 0.0f, 0.0f, 0.0f };
			return true;
		}

		const float sampleTime = a_time > rotationList.back().time ? rotationList.back().time : a_time;
		const auto interpolateSample = [&](std::size_t a_index, const Rotation& a_current) {
			const float previousTime = a_index ? rotationList[a_index - 1].time : 0.0f;
			const RE::NiQuaternion previous = a_index ? rotationList[a_index - 1].delta : RE::NiQuaternion{ 1.0f, 0.0f, 0.0f, 0.0f };
			const float progress = SegmentProgress(sampleTime, previousTime, a_current.time - previousTime);
			a_interpolator(a_out, progress, previous, a_current.delta);
		};

		if (rotationList.size() <= kLinearSearchMaxKeys) {
			for (std::size_t index = 0; index < rotationList.size(); ++index) {
				const Rotation& current = rotationList[index];
				if (sampleTime <= current.time) {
					interpolateSample(index, current);
					return true;
				}
			}
			interpolateSample(rotationList.size() - 1, rotationList.back());
			return true;
		}

		const auto segment = std::lower_bound(rotationList.begin(), rotationList.end(), sampleTime,
			[](const Rotation& a_motion, float a_value) {
				return a_motion.time < a_value;
			});
		const std::size_t index = static_cast<std::size_t>(segment - rotationList.begin());
		interpolateSample(index, rotationList[index]);
		return true;
	}

	std::vector<Translation> translationList;
	std::vector<Rotation> rotationList;

private:
	template <class MotionT>
	static void FinalizeList(std::vector<MotionT>& a_list)
	{
		a_list.erase(std::remove_if(a_list.begin(), a_list.end(), [](const MotionT& a_motion) {
			return !std::isfinite(a_motion.time);
		}), a_list.end());

		std::sort(a_list.begin(), a_list.end(), [](const MotionT& a_lhs, const MotionT& a_rhs) {
			return a_lhs.time < a_rhs.time;
		});
	}

	static float SegmentProgress(float a_time, float a_previousTime, float a_duration)
	{
		return a_duration > std::numeric_limits<float>::epsilon() ? (a_time - a_previousTime) / a_duration : 1.0f;
	}

	static constexpr std::size_t kLinearSearchMaxKeys = 64;
	bool finalized_ = false;
};
