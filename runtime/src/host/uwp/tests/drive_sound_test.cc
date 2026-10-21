// SPDX-License-Identifier: MIT
#include "../drive_sound.h"
#include <complex>
#include <iostream>
#include <stdexcept>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (0)
// Hann-windowed DFT power centroid across several windows in each 300 ms segment.
double centroid(const std::vector<float> &s, size_t start) {
    constexpr size_t n = 2048;
    double weighted = 0, power = 0;
    for (size_t window = 0; window < 6; ++window)
        for (size_t k = 1; k < n / 2; ++k) {
            std::complex<double> value{};
            for (size_t i = 0; i < n; ++i) {
                double angle = 2 * 3.141592653589793 * k * i / n;
                double hann = .5 - .5 * std::cos(2 * 3.141592653589793 * i / (n - 1));
                value += double(s[start + window * n + i]) * hann *
                         std::complex<double>(std::cos(angle), -std::sin(angle));
            }
            double p = std::norm(value);
            power += p;
            weighted += p * (k * 48000. / n);
        }
    return weighted / power;
}
int main() {
    auto s = uwp::make_drive_spinup(48000);
    CHECK(s.size() == 76800);
    double peak = 0;
    for (auto v : s) {
        CHECK(std::isfinite(v));
        peak = std::max(peak, std::abs(double(v)));
    }
    CHECK(peak <= 1 && peak > .1);
    CHECK(std::abs(s.front()) < .001 && std::abs(s.back()) < .001);
    double first = centroid(s, 0), last = centroid(s, s.size() - 14400);
    CHECK(last > first * 1.5);
    CHECK(uwp::make_drive_spinup(0).empty());
    std::cout << "drive_sound: 76800 mono samples at 48 kHz; peak=" << peak << "; centroid " << first
              << " -> " << last << " Hz; passed\n";
}
