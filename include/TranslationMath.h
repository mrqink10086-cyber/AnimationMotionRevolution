#pragma once

#include <cstddef>
#include <immintrin.h>

namespace amr
{
    template <class Point>
    inline void InterpolateTranslation(const Point& a_current, const Point& a_previous, float a_progress, Point& a_out)
    {
        static_assert(sizeof(Point) == 12 && offsetof(Point, x) == 0 && offsetof(Point, y) == 4 && offsetof(Point, z) == 8);
        const float weight = 1.0F - a_progress;
        const auto current = _mm_set_ps(0.0F, a_current.z, a_current.y, a_current.x);
        const auto previous = _mm_set_ps(0.0F, a_previous.z, a_previous.y, a_previous.x);
        const auto progress = _mm_set_ps(0.0F, a_progress, a_progress, a_progress);
        const auto previousWeight = _mm_set_ps(0.0F, weight, weight, weight);
        const auto result = _mm_add_ps(_mm_mul_ps(current, progress), _mm_mul_ps(previous, previousWeight));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(&a_out.x), _mm_castps_si128(result));
        _mm_store_ss(&a_out.z, _mm_movehl_ps(result, result));
    }
}
