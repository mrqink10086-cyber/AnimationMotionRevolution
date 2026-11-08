#pragma once

#include "Motion.h"
#include "TranslationMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

class AnimMotionData
{
public:
	enum class WarpSegmentKind
	{
		kInactive,
		kExplicit,
		kDefaultCombat
	};

	struct WarpMarker
	{
		float time;
		std::optional<WarpLimits> limits;
	};

	struct WarpSegment
	{
		float startTime;
		float endTime;
		RE::NiPoint3 startTranslation;
		RE::NiPoint3 endTranslation;
		WarpSegmentKind kind;
		WarpLimits limits;
	};

	AnimMotionData() = default;

	void Add(const Translation& a_translation)
	{
		if (!finalized_) {
			translationList.push_back(a_translation);
		}
	}

	void Add(const Translation* a_translation)
	{
		if (a_translation) {
			Add(*a_translation);
		}
	}

	void Add(const Rotation& a_rotation)
	{
		if (!finalized_) {
			rotationList.push_back(a_rotation);
		}
	}

	void Add(const Rotation* a_rotation)
	{
		if (a_rotation) {
			Add(*a_rotation);
		}
	}

	bool Add(const Warp& a_warp)
	{
		if (finalized_) {
			return false;
		}
		hasExplicitWarpControl = true;
		if (!std::isfinite(a_warp.time)) {
			return false;
		}

		const bool disablesWarping =
			a_warp.delta.lowerLimit == 1.0f && a_warp.delta.upperLimit == 1.0f;
		warpMarkers.push_back({ a_warp.time,
			disablesWarping ? std::nullopt : std::optional{ a_warp.delta } });
		return true;
	}

	bool Add(const WarpEnd& a_warpEnd)
	{
		if (finalized_) {
			return false;
		}
		hasExplicitWarpControl = true;
		if (!std::isfinite(a_warpEnd.time)) {
			return false;
		}
		warpMarkers.push_back({ a_warpEnd.time, std::nullopt });
		return true;
	}

	void MarkExplicitWarpControl()
	{
		if (!finalized_) {
			hasExplicitWarpControl = true;
		}
	}

	void AddCombatWarpBoundary(float a_time)
	{
		if (!finalized_ && std::isfinite(a_time)) {
			combatWarpBoundaries.push_back(a_time);
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
		std::stable_sort(warpMarkers.begin(), warpMarkers.end(), [](const WarpMarker& a_lhs, const WarpMarker& a_rhs) {
			return a_lhs.time < a_rhs.time;
		});
		std::sort(combatWarpBoundaries.begin(), combatWarpBoundaries.end());
		std::vector<float> uniqueCombatBoundaries;
		uniqueCombatBoundaries.reserve(combatWarpBoundaries.size());
		for (const float boundary : combatWarpBoundaries) {
			if (uniqueCombatBoundaries.empty() ||
				std::abs(boundary - uniqueCombatBoundaries.back()) > 1.0e-4f) {
				uniqueCombatBoundaries.push_back(boundary);
			}
		}
		combatWarpBoundaries = std::move(uniqueCombatBoundaries);
		BuildWarpSegments();
		finalized_ = true;
	}

	// Retained for callers using the upstream method name.
	void SortListsByTime()
	{
		Finalize();
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
	std::vector<WarpMarker> warpMarkers;
	std::vector<float> combatWarpBoundaries;
	std::vector<WarpSegment> warpSegments;
	bool hasExplicitWarpControl{ false };
	RE::NiPoint3 initialTranslation{};

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

	RE::NiPoint3 SampleTranslationAt(float a_time) const
	{
		RE::NiPoint3 result{};
		SampleTranslation(a_time, result);
		return result;
	}

	void AddWarpSegment(
		float a_startTime,
		float a_endTime,
		WarpSegmentKind a_kind,
		const WarpLimits& a_limits = {})
	{
		if (a_endTime <= a_startTime) {
			return;
		}
		warpSegments.push_back({ a_startTime,
			a_endTime,
			SampleTranslationAt(a_startTime),
			SampleTranslationAt(a_endTime),
			a_kind,
			a_limits });
	}

	void BuildWarpSegments()
	{
		warpSegments.clear();
		if (translationList.empty()) {
			initialTranslation = {};
			return;
		}
		initialTranslation = SampleTranslationAt(0.0f);

		const float duration = translationList.back().time;
		if (duration <= 0.0f) {
			return;
		}

		if (hasExplicitWarpControl) {
			float segmentStart = 0.0f;
			auto kind = WarpSegmentKind::kInactive;
			WarpLimits limits{};
			for (const auto& marker : warpMarkers) {
				const float markerTime = std::clamp(marker.time, 0.0f, duration);
				AddWarpSegment(segmentStart, markerTime, kind, limits);
				segmentStart = markerTime;
				if (marker.limits) {
					kind = WarpSegmentKind::kExplicit;
					limits = *marker.limits;
				} else {
					kind = WarpSegmentKind::kInactive;
				}
			}
			AddWarpSegment(segmentStart, duration, kind, limits);
			return;
		}

		float segmentStart = 0.0f;
		for (const float boundary : combatWarpBoundaries) {
			const float boundaryTime = std::clamp(boundary, 0.0f, duration);
			AddWarpSegment(segmentStart, boundaryTime, WarpSegmentKind::kDefaultCombat);
			segmentStart = boundaryTime;
		}
		AddWarpSegment(segmentStart, duration, WarpSegmentKind::kDefaultCombat);
	}

	bool finalized_ = false;
};
