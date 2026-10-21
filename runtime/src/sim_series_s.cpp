// SPDX-License-Identifier: MIT
#include "sim_series_s.h"
#include "log.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#if defined(BOTW_PLATFORM_UWP) && defined(_WIN32)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#endif

namespace sim_series_s {
namespace {
std::mutex stats_lock;
CpuStats stats{};
using Clock = std::chrono::steady_clock;
[[maybe_unused]] std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
Config load() {
    std::map<std::string, std::string> values;
#if defined(BOTW_PLATFORM_UWP) && defined(_WIN32)
    try {
        const auto data = winrt::Windows::Storage::ApplicationData::Current();
        const std::filesystem::path local(data.LocalFolder().Path().c_str());
        auto properties = data.LocalSettings().Values();
        for (const auto *key : {L"BOTW_SIM_SERIES_S", L"BOTW_SIM_GPU_FACTOR", L"BOTW_SIM_CPU_FACTOR"}) {
            if (properties.HasKey(key)) {
                auto value = properties.Lookup(key);
                // String settings use the same validation as the text file.
                if (auto text = value.try_as<winrt::Windows::Foundation::IPropertyValue>();
                    text && text.Type() == winrt::Windows::Foundation::PropertyType::String)
                    values[winrt::to_string(key)] = winrt::to_string(text.GetString());
            }
        }
        for (const auto *file : {L"settings", L"botw_env.txt"}) {
            std::ifstream input(local / file);
            for (std::string line; std::getline(input, line);) {
                const auto eq = line.find('=');
                if (eq != std::string::npos)
                    values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
        }
        if (!values.contains("BOTW_SIM_SERIES_S") && std::filesystem::exists(local / L"sim_series_s.txt"))
            values["BOTW_SIM_SERIES_S"] = "1";
    } catch (const winrt::hresult_error &) {
        // No application-data container (for example, an unpackaged diagnostic harness).
    } catch (const std::exception &) {
        // Missing or unreadable optional configuration keeps the defaults.
    }
#else
    for (const auto *key : {"BOTW_SIM_SERIES_S", "BOTW_SIM_GPU_FACTOR", "BOTW_SIM_CPU_FACTOR"})
        if (const char *value = std::getenv(key))
            values[key] = value;
#endif
    Config result;
    result.enabled = values["BOTW_SIM_SERIES_S"] == "1";
    result.gpu_factor = factor(values["BOTW_SIM_GPU_FACTOR"], 3.0, 10.0);
    result.cpu_factor = factor(values["BOTW_SIM_CPU_FACTOR"], 1.05, 2.0);
    if (result.enabled)
        LOG("Series S simulation ON (gpu x%.1f, cpu x%.2f, scale 1.5)", result.gpu_factor, result.cpu_factor);
    return result;
}
} // namespace
const Config &settings() {
    static const Config config = load();
    return config;
}
void cpu_frame(uint64_t waited_ns) {
    const auto &config = settings();
    if (!config.enabled)
        return;
    thread_local Clock::time_point previous{};
    thread_local uint64_t previous_wait = 0;
    const auto now = Clock::now();
    if (previous != Clock::time_point{}) {
        const double elapsed = std::chrono::duration<double>(now - previous).count();
        const double waited = double(waited_ns - previous_wait) * 1e-9;
        const double work = std::max(0.0, elapsed - waited);
        {
            std::lock_guard lock(stats_lock);
            stats.work_ms += work * 1000;
            ++stats.frames;
        }
        const auto extra = extra_seconds(work, config.cpu_factor);
        if (extra > 0) {
            const auto deadline =
                now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(extra));
            // Sleep the long portion, then spin the tail without depending on Windows timer resolution.
            if (extra > 0.002)
                std::this_thread::sleep_until(deadline - std::chrono::milliseconds(1));
            while (Clock::now() < deadline)
                std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
    previous = Clock::now(); // Exclude our own stretch from the following sample.
    previous_wait = waited_ns;
}
CpuStats take_cpu_stats() {
    std::lock_guard lock(stats_lock);
    const auto result = stats;
    stats = {};
    return result;
}
} // namespace sim_series_s
