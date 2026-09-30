#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined (_MSC_VER)
#pragma float_control (precise, on, push)
#elif defined (__GNUC__) && ! defined (__clang__)
#pragma GCC push_options
#pragma GCC optimize ("no-fast-math")
#endif

// BS.1770 双声道 K 权重能量之和，无门限的严格三秒短期响度。
// 启动时除以实际收到的样本数（包含处理链启动静音），而非补足三秒。
class ShortTermLoudness
{
public:
    static constexpr float silenceFloor = -70.0f;

    void prepare (double rate)
    {
        constexpr double pi = 3.1415926535897932384626433832795;
        const double fs = std::max (rate, 8000.0);
        const double k = std::tan (pi * 1681.974450955533 / fs);
        const double vh = std::pow (10.0, 3.999843853973347 / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double q = 0.7071752369554196;
        const double a0 = 1.0 + k / q + k * k;
        const std::array<double, 5> shelf {
            (vh + vb * k / q + k * k) / a0,
            2.0 * (k * k - vh) / a0,
            (vh - vb * k / q + k * k) / a0,
            2.0 * (k * k - 1.0) / a0,
            (1.0 - k / q + k * k) / a0
        };
        const double kh = std::tan (pi * 38.13547087602444 / fs);
        const double qh = 0.5003270373238773;
        const double ah = 1.0 + kh / qh + kh * kh;
        // RLB 高通的分子是 [1,-2,1]，不能再除以 ah。
        const std::array<double, 5> highpass {
            1.0, -2.0, 1.0, 2.0 * (kh * kh - 1.0) / ah, (1.0 - kh / qh + kh * kh) / ah
        };
        for (auto& channel : filters) { channel[0].c = shelf; channel[1].c = highpass; }
        energies.assign (static_cast<std::size_t> (std::llround (fs * 3.0)), 0.0);
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : filters)
            for (auto& filter : channel) filter.z1 = filter.z2 = 0.0;
        std::fill (energies.begin(), energies.end(), 0.0);
        position = valid = 0;
        sum = correction = 0.0;
    }

    void process (const float* left, const float* right, int samples) noexcept
    {
        if (energies.empty()) return;
        for (int n = 0; n < samples; ++n)
        {
            const double l = weighted (left[n], 0);
            const double r = weighted (right[n], 1);
            const double energy = l * l + r * r;
            add (-energies[position]);
            add (energy);
            energies[position] = energy;
            position = (position + 1) % energies.size();
            valid = std::min (valid + 1, energies.size());
        }
    }

    float getLUFS() const noexcept
    {
        if (valid == 0) return silenceFloor;
        const double energy = std::max (0.0, (sum + correction) / static_cast<double> (valid));
        return static_cast<float> (std::max (static_cast<double> (silenceFloor),
            -0.691 + 10.0 * std::log10 (std::max (energy, 1.0e-30))));
    }
    std::size_t getWindowSamples() const noexcept { return energies.size(); }
    std::size_t getValidSamples() const noexcept { return valid; }

private:
    struct Biquad
    {
        std::array<double, 5> c {};
        double z1 = 0.0, z2 = 0.0;
        double process (double x) noexcept
        {
            const double y = c[0] * x + z1;
            z1 = c[1] * x - c[3] * y + z2;
            z2 = c[2] * x - c[4] * y;
            return y;
        }
    };
    double weighted (float x, std::size_t channel) noexcept
    {
        std::uint32_t bits;
        std::memcpy (&bits, &x, sizeof (bits));
        const double input = (bits & 0x7f800000u) == 0x7f800000u ? 0.0 : static_cast<double> (x);
        return filters[channel][1].process (filters[channel][0].process (input));
    }
    void add (double value) noexcept
    {
#if defined (__clang__)
#pragma clang fp reassociate(off)
#pragma clang fp contract(off)
#endif
        const double next = sum + value;
        correction += std::abs (sum) >= std::abs (value) ? (sum - next) + value : (value - next) + sum;
        sum = next;
    }
    std::array<std::array<Biquad, 2>, 2> filters {};
    std::vector<double> energies;
    std::size_t position = 0, valid = 0;
    double sum = 0.0, correction = 0.0;
};

#if defined (_MSC_VER)
#pragma float_control (pop)
#elif defined (__GNUC__) && ! defined (__clang__)
#pragma GCC pop_options
#endif
