#pragma once

#include <charconv>
#include <cmath>
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
}

inline std::variant<std::monostate, Translation, Rotation>
	ParseAnnotation(std::string_view a_text, float a_time)
{
	constexpr std::string_view animmotionPrefix = "animmotion ";
	constexpr std::string_view animrotationPrefix = "animrotation ";

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

	return {};
}

inline std::variant<std::monostate, Translation, Rotation>
	ParseAnnotation(const RE::hkaAnnotationTrack::Annotation& a_annotation)
{
        const auto text = a_annotation.text.c_str();
        return text ? ParseAnnotation(std::string_view{ text }, a_annotation.time) : std::variant<std::monostate, Translation, Rotation>{};
}
