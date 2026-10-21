// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace uwp {
// Deterministic mono float PCM. No assets, global RNG, or platform dependencies.
inline std::vector<float> make_drive_spinup(int rate) {
    if (rate < 8000 || rate > 192000)
        return {};
    std::vector<float> samples(static_cast<size_t>(rate) * 8 / 5);
    constexpr double pi = 3.14159265358979323846;
    double phase = 0, noise = 0;
    uint32_t random = 0x4d6f746f;
    for (size_t i = 0; i < samples.size(); ++i) {
        double t = double(i) / rate, ramp = std::clamp(t / 1.2, 0., 1.);
        double frequency = 70 + 310 * ramp * ramp * (3 - 2 * ramp);
        phase += 2 * pi * frequency / rate;
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        double white = double(random) / 2147483648. - 1;
        double cutoff = 250 + frequency * 3;
        noise += (1 - std::exp(-2 * pi * cutoff / rate)) * (white - noise);
        double signal =
            .33 * std::sin(phase) + .13 * std::sin(2 * phase) + .055 * std::sin(3 * phase) + .045 * noise;
        for (double start : {.035, .13, .24}) {
            double age = t - start;
            if (age >= 0 && age < .018)
                signal += .12 * std::exp(-age * 260) * std::sin(2 * pi * 950 * age);
        }
        double envelope = std::min(1., t / .025) * std::min(1., (1.6 - t) / .20);
        samples[i] = static_cast<float>(signal * envelope);
    }
    return samples;
}
} // namespace uwp
