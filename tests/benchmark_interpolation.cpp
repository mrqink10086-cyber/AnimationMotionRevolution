#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <immintrin.h>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include "TranslationMath.h"

struct Point
{
    float x, y, z;
};

struct Input
{
    Point current, previous;
    float progress;
};

static Point Scalar(const Input& a)
{
    const float weight = 1.0F - a.progress;
    return { a.current.x * a.progress + a.previous.x * weight,
        a.current.y * a.progress + a.previous.y * weight,
        a.current.z * a.progress + a.previous.z * weight };
}

static Point SSE(const Input& a)
{
    Point out;
    amr::InterpolateTranslation(a.current, a.previous, a.progress, out);
    return out;
}

static bool Same(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b) || (std::isnan(a) && std::isnan(b));
}

template <auto Interpolate>
static double Measure(const std::vector<Input>& inputs, std::vector<Point>& outputs)
{
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        outputs[i] = Interpolate(inputs[i]);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int main()
{
    std::mt19937 random{ 0xA1170 };
    std::uniform_real_distribution<float> values(-10000.0F, 10000.0F);
    std::uniform_real_distribution<float> progress(-4.0F, 2.0F);
    std::vector<Input> inputs(262144);
    for (auto& input : inputs) {
        input = { { values(random), values(random), values(random) },
            { values(random), values(random), values(random) }, progress(random) };
    }
    std::vector<Point> scalar(inputs.size()), simd(inputs.size());
    Measure<Scalar>(inputs, scalar);
    Measure<SSE>(inputs, simd);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (!Same(scalar[i].x, simd[i].x) || !Same(scalar[i].y, simd[i].y) || !Same(scalar[i].z, simd[i].z)) {
            std::cerr << "Interpolation mismatch at " << i << '\n';
            return EXIT_FAILURE;
        }
    }
    constexpr std::array edgeValues{ 0.0F, -0.0F, 1.0F, -1.0F,
        std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
    for (const auto value : edgeValues) {
        for (const auto t : edgeValues) {
            const Input input{ { value, -value, 0.0F }, { -0.0F, value, -value }, t };
            const auto a = Scalar(input);
            const auto b = SSE(input);
            if (!Same(a.x, b.x) || !Same(a.y, b.y) || !Same(a.z, b.z)) {
                std::cerr << "Interpolation edge mismatch\n";
                return EXIT_FAILURE;
            }
        }
    }
    double scalarMs = 0, sseMs = 0;
    for (int round = 0; round < 12; ++round) {
        if (round % 2) {
            sseMs += Measure<SSE>(inputs, simd);
            scalarMs += Measure<Scalar>(inputs, scalar);
        } else {
            scalarMs += Measure<Scalar>(inputs, scalar);
            sseMs += Measure<SSE>(inputs, simd);
        }
    }
    double checksum = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        checksum += scalar[i].x + simd[i].z;
    }
    std::cout << "Host synthetic interpolation, bitwise outputs match; scalar ms=" << scalarMs
        << ", SSE ms=" << sseMs << ", checksum=" << checksum << '\n';
}
