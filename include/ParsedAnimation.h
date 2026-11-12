#pragma once

#include "AnimMotionHandler.h"

#include <memory>

namespace amr
{
	struct ParsedAnimation
	{
		AnimMotionData motion;
		bool hasVerticalMotion{ false };
	};

	template <class ReportInvalidWarp>
	std::shared_ptr<const ParsedAnimation> ParseAnimation(const RE::hkaAnimation& a_animation,
		float a_verticalMotionEpsilon, ReportInvalidWarp&& a_reportInvalidWarp)
	{
		auto parsed = std::make_shared<ParsedAnimation>();
		for (const auto& track : a_animation.annotationTracks) {
			for (const auto& annotation : track.annotations) {
				const auto annotationText = annotation.text.c_str();
				const std::string_view text = annotationText ? std::string_view(annotationText) : std::string_view{};
				const bool warpControl = IsWarpControlAnnotation(text);
				if (warpControl) {
					parsed->motion.MarkExplicitWarpControl();
				}
				const auto annotationData = ParseAnnotation(annotation);
				if (const auto* translation = std::get_if<Translation>(&annotationData)) {
					parsed->motion.Add(*translation);
				} else if (const auto* rotation = std::get_if<Rotation>(&annotationData)) {
					parsed->motion.Add(*rotation);
				} else if (const auto* warp = std::get_if<Warp>(&annotationData)) {
					if (!parsed->motion.Add(*warp)) {
						a_reportInvalidWarp(annotation, text, true);
					}
				} else if (const auto* warpEnd = std::get_if<WarpEnd>(&annotationData)) {
					if (!parsed->motion.Add(*warpEnd)) {
						a_reportInvalidWarp(annotation, text, true);
					}
				} else if (warpControl) {
					a_reportInvalidWarp(annotation, text, false);
				}
				if (IsCombatWarpBoundary(text)) {
					parsed->motion.AddCombatWarpBoundary(annotation.time);
				}
			}
		}
		parsed->motion.Finalize();
		parsed->hasVerticalMotion = std::any_of(parsed->motion.translationList.begin(), parsed->motion.translationList.end(),
			[=](const Translation& a_key) { return std::abs(a_key.delta.z) > a_verticalMotionEpsilon; });
		return parsed;
	}
}
