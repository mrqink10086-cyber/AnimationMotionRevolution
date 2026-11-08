#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>
#include <system_error>
#include <variant>

template <typename T>
struct Motion
{
	float time;
	T delta;
};

using Translation = Motion<RE::NiPoint3>;
using Rotation = Motion<RE::NiQuaternion>;

struct WarpLimits
{
	float lowerLimit{ 0.0f };
	float upperLimit{ 1.0f };
	float maximumAngleDegrees{ 60.0f };
	float maximumDistance{ std::numeric_limits<float>::infinity() };
};

using Warp = Motion<WarpLimits>;

struct WarpEnd
{
	float time;
};

namespace anim_motion_detail
{
	inline bool IsWhitespace(char a_char) noexcept
	{
		return a_char == ' ' || a_char == '\t' || a_char == '\n' || a_char == '\r' || a_char == '\f' || a_char == '\v';
	}

	inline void SkipWhitespace(std::string_view& a_text) noexcept
	{
		while (!a_text.empty() && IsWhitespace(a_text.front())) {
			a_text.remove_prefix(1);
		}
	}

	inline bool ParseFloat(std::string_view& a_text, float& a_value) noexcept
	{
		SkipWhitespace(a_text);
		if (a_text.empty()) {
			return false;
		}

		std::size_t tokenLength = 0;
		while (tokenLength < a_text.size() && !IsWhitespace(a_text[tokenLength])) {
			++tokenLength;
		}

		const char* const begin = a_text.data();
		const char* const end = begin + tokenLength;
		const auto result = std::from_chars(begin, end, a_value, std::chars_format::general);
		if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(a_value)) {
			return false;
		}

		a_text.remove_prefix(tokenLength);
		return true;
	}

	inline bool HasOnlyWhitespace(std::string_view a_text) noexcept
	{
		SkipWhitespace(a_text);
		return a_text.empty();
	}

	inline bool HasExactToken(std::string_view a_text, std::string_view a_token) noexcept
	{
		return a_text.starts_with(a_token) &&
			(a_text.size() == a_token.size() || IsWhitespace(a_text[a_token.size()]));
	}

	inline bool EqualsIgnoreCase(std::string_view a_value, std::string_view a_expected) noexcept
	{
		return a_value.size() == a_expected.size() &&
			std::equal(a_value.begin(), a_value.end(), a_expected.begin(), [](char a_lhs, char a_rhs) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(a_lhs))) ==
					static_cast<char>(std::tolower(static_cast<unsigned char>(a_rhs)));
			});
	}
}

inline bool IsWarpControlAnnotation(std::string_view a_text) noexcept
{
	return anim_motion_detail::HasExactToken(a_text, "animwarp") ||
		anim_motion_detail::HasExactToken(a_text, "animwarpend");
}

inline bool IsCombatWarpBoundary(std::string_view a_text) noexcept
{
	constexpr std::string_view hitFrame = "hitframe";
	constexpr std::string_view collisionAdd = "collision_add";

	anim_motion_detail::SkipWhitespace(a_text);
	if (a_text.empty()) {
		return false;
	}

	const auto eventEnd = a_text.find('.');
	auto eventName = a_text.substr(0, eventEnd);
	while (!eventName.empty() && anim_motion_detail::IsWhitespace(eventName.back())) {
		eventName.remove_suffix(1);
	}
	if (anim_motion_detail::EqualsIgnoreCase(eventName, hitFrame)) {
		return true;
	}
	return a_text.size() >= collisionAdd.size() &&
		anim_motion_detail::EqualsIgnoreCase(a_text.substr(0, collisionAdd.size()), collisionAdd);
}

inline std::variant<std::monostate, Translation, Rotation, Warp, WarpEnd>
	ParseAnnotation(std::string_view a_text, float a_time)
{
	constexpr std::string_view animmotionPrefix = "animmotion ";
	constexpr std::string_view animrotationPrefix = "animrotation ";
	constexpr std::string_view animwarpToken = "animwarp";
	constexpr std::string_view animwarpendToken = "animwarpend";

	if (!std::isfinite(a_time)) {
		return {};
	}

	if (a_text.starts_with(animmotionPrefix)) {
		std::string_view values = a_text.substr(animmotionPrefix.size());
		RE::NiPoint3 translation{};
		if (!anim_motion_detail::ParseFloat(values, translation.x) ||
			!anim_motion_detail::ParseFloat(values, translation.y) ||
			!anim_motion_detail::ParseFloat(values, translation.z) ||
			!anim_motion_detail::HasOnlyWhitespace(values)) {
			return {};
		}

		return Translation{ a_time, translation };
	}

	if (a_text.starts_with(animrotationPrefix)) {
		std::string_view values = a_text.substr(animrotationPrefix.size());
		float yawDegrees = 0.0f;
		if (!anim_motion_detail::ParseFloat(values, yawDegrees) || !anim_motion_detail::HasOnlyWhitespace(values)) {
			return {};
		}

		const float yaw = yawDegrees * std::numbers::pi_v<float> / 180.0f;
		if (!std::isfinite(yaw)) {
			return {};
		}
		const float halfYaw = yaw / 2.0f;
		constexpr float cosRoll = 1.0f;
		constexpr float sinRoll = 0.0f;
		constexpr float cosPitch = 1.0f;
		constexpr float sinPitch = 0.0f;
		const float cosYaw = std::cos(halfYaw);
		const float sinYaw = std::sin(halfYaw);
		RE::NiQuaternion rotation{
			cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw,
			sinRoll * cosPitch * cosYaw - cosRoll * sinPitch * sinYaw,
			cosRoll * sinPitch * cosYaw + sinRoll * cosPitch * sinYaw,
			cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw
		};
		return Rotation{ a_time, rotation };
	}

	if (anim_motion_detail::HasExactToken(a_text, animwarpToken)) {
		std::string_view values = a_text.substr(animwarpToken.size());
		WarpLimits limits{};
		if (!anim_motion_detail::ParseFloat(values, limits.lowerLimit) ||
			!anim_motion_detail::ParseFloat(values, limits.upperLimit) ||
			limits.lowerLimit < 0.0f || limits.upperLimit < limits.lowerLimit) {
			return {};
		}

		if (!anim_motion_detail::HasOnlyWhitespace(values)) {
			if (!anim_motion_detail::ParseFloat(values, limits.maximumAngleDegrees) ||
				limits.maximumAngleDegrees < 0.0f || limits.maximumAngleDegrees > 180.0f) {
				return {};
			}
		}
		if (!anim_motion_detail::HasOnlyWhitespace(values)) {
			if (!anim_motion_detail::ParseFloat(values, limits.maximumDistance) ||
				limits.maximumDistance < 0.0f) {
				return {};
			}
		}
		if (!anim_motion_detail::HasOnlyWhitespace(values)) {
			return {};
		}
		return Warp{ a_time, limits };
	}

	if (anim_motion_detail::HasExactToken(a_text, animwarpendToken) &&
		anim_motion_detail::HasOnlyWhitespace(a_text.substr(animwarpendToken.size()))) {
		return WarpEnd{ a_time };
	}

	return {};
}

inline std::variant<std::monostate, Translation, Rotation, Warp, WarpEnd>
	ParseAnnotation(const RE::hkaAnnotationTrack::Annotation& a_annotation)
{
	const auto text = a_annotation.text.c_str();
	return text ? ParseAnnotation(std::string_view{ text }, a_annotation.time) :
			std::variant<std::monostate, Translation, Rotation, Warp, WarpEnd>{};
}
