// SPDX-License-Identifier: MIT
#if defined(BOTW_PLATFORM_UWP)
#include "runtime.h"
#include "../../../cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12PackHooks.h"
#include "../../fsr3_policy.h"
#include "../../hle.h"
#include "../../latte/latte.h"
#include "../../loader.h"
#include "../../os.h"
#include "../../packs/host_install.h"
#include "../../settings.h"
#include "../../sim_series_s.h"
#include "console_policy.h"
#include "first_frame.h"
#include "output_policy.h"
#include "latte_pipeline_config.h"
#if defined(_WIN32)
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#if __has_include(<gamingdeviceinformation.h>)
#include <combaseapi.h> // STDAPI, used by gamingdeviceinformation.h
#include <gamingdeviceinformation.h>
#define BOTW_HAS_GAMING_DEVICE_INFO 1
#endif
#endif
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

// Keep the POSIX implementation of GuestThread out of the UWP host. W1 owns thread.h/thread.cpp.
namespace threads {
void start_main(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size);
}
void debug_watch_init();
std::string g_game_root, g_user_root;
bool g_headless = false, g_mute = false;
int g_run_seconds = 0, g_target_fps = 60;
float g_render_scale = 0;
uint64_t g_trace_from = 0, g_trace_to = 0;
extern "C" PPC_NORETURN void rt_fatal_exit() { std::_Exit(3); }

namespace uwp {
namespace {
packs::LoadedRpx loaded_rpx;
std::atomic<bool> mode_changed{false};
void output_pixels([[maybe_unused]] int &width, [[maybe_unused]] int &height) {
#if defined(_WIN32)
    try {
        const auto hdmi =
            winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
        if (hdmi) {
            static winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation monitored{nullptr};
            if (!monitored) {
                monitored = hdmi;
                monitored.DisplayModesChanged([](auto const &sender, auto const &) {
                    mode_changed.store(true);
                    try {
                        const auto current = sender.GetCurrentDisplayMode();
                        fsr3::display_hz.store(current ? current.RefreshRate() : 0);
                    } catch (const winrt::hresult_error &) {
                        fsr3::display_hz.store(0);
                    }
                });
            }
            const auto mode = hdmi.GetCurrentDisplayMode();
            if (mode && mode.ResolutionWidthInRawPixels() && mode.ResolutionHeightInRawPixels()) {
                width = static_cast<int>(mode.ResolutionWidthInRawPixels());
                height = static_cast<int>(mode.ResolutionHeightInRawPixels());
                fsr3::display_hz.store(mode.RefreshRate());
                return;
            }
        }
    } catch (const winrt::hresult_error &) {
        // HDMI information is unavailable on desktop/remote displays.
    }
    try {
        const auto bounds = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Bounds();
        const auto scale = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView()
                               .RawPixelsPerViewPixel();
        width = std::max(1, static_cast<int>(bounds.Width * scale + 0.5));
        height = std::max(1, static_cast<int>(bounds.Height * scale + 0.5));
    } catch (const winrt::hresult_error &) {
        LOG("Display pixel size unavailable; using host size %dx%d", width, height);
    }
#endif
}
// Pass the real console profile to both scaling and renderer policy selection.
ConsoleModel detect_console_model() {
    ConsoleModel model = ConsoleModel::Desktop;
#if defined(_WIN32)
    try {
        const auto family = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily();
        if (family == L"Windows.Xbox")
            model = ConsoleModel::UnknownXbox;
        LOG("Device family=%s", winrt::to_string(family).c_str());
        const auto form = winrt::Windows::System::Profile::AnalyticsInfo::DeviceForm();
        LOG("Device form=%s", winrt::to_string(form).c_str());
    } catch (const winrt::hresult_error &e) {
        LOG("Device analytics unavailable (0x%08x)", unsigned(e.code().value));
        // The package also runs on Windows; an API failure alone is not model evidence.
    }
#endif
#if defined(BOTW_HAS_GAMING_DEVICE_INFO)
    GAMING_DEVICE_MODEL_INFORMATION info{};
    const auto result = GetGamingDeviceModelInformation(&info);
    LOG("Gaming device query=0x%08x vendor=0x%08x device=0x%08x", unsigned(result), unsigned(info.vendorId),
        unsigned(info.deviceId));
    if (SUCCEEDED(result) && info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT) {
        model = ConsoleModel::UnknownXbox;
        if (info.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S)
            model = ConsoleModel::SeriesS;
        if (info.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X ||
            info.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT)
            model = ConsoleModel::SeriesX;
    }
#endif
    LOG("Console model: %s; Series S simulation=%s", console_model_name(model),
        sim_series_s::settings().enabled ? "on" : "off");
    return model;
}
fsr3::Console console_profile() {
    static const auto model = detect_console_model();
    return select_console_profile(model, sim_series_s::settings().enabled);
}
// Manual scaling can reach 1080p on Series S; automatic native scaling starts at 900p.
float hardware_max_render_scale() { return console_profile() == fsr3::Console::SeriesS ? 1.5f : 3.f; }
} // namespace
namespace {
float requested_scale = 0;
}
bool take_output_mode_change() { return mode_changed.exchange(false); }
#if defined(_WIN32)
winrt::Windows::Foundation::IAsyncAction configure_output_mode() {
    using namespace winrt::Windows::Graphics::Display::Core;
    const auto console = console_profile();
    if (console == fsr3::Console::Desktop)
        co_return;
    try {
        const auto hdmi = HdmiDisplayInformation::GetForCurrentView();
        if (!hdmi) {
            LOG("HDMI information unavailable; retaining CoreWindow output size");
            co_return;
        }
        const auto current = hdmi.GetCurrentDisplayMode();
        if (!current) {
            LOG("HDMI current mode unavailable; retaining CoreWindow output size");
            co_return;
        }
        auto describe = [](const HdmiDisplayMode &mode) {
            return OutputMode{int(mode.ResolutionWidthInRawPixels()), int(mode.ResolutionHeightInRawPixels()),
                              mode.RefreshRate(), mode.IsSdrLuminanceSupported(), mode.StereoEnabled()};
        };
        auto selected = current;
        for (const auto &mode : hdmi.GetSupportedDisplayModes())
            if (better_game_mode(describe(mode), describe(selected), console))
                selected = mode;
        LOG("HDMI current %ux%u %.2f Hz; selected %ux%u %.2f Hz", current.ResolutionWidthInRawPixels(),
            current.ResolutionHeightInRawPixels(), current.RefreshRate(),
            selected.ResolutionWidthInRawPixels(), selected.ResolutionHeightInRawPixels(),
            selected.RefreshRate());
        if (selected != current) {
            const bool accepted =
                co_await hdmi.RequestSetCurrentDisplayModeAsync(selected, HdmiDisplayHdrOption::None);
            LOG("HDMI mode request %s", accepted ? "accepted" : "rejected; retaining current mode");
        }
    } catch (const winrt::hresult_error &e) {
        LOG("HDMI mode selection unavailable (0x%08x); retaining current display", unsigned(e.code().value));
    }
}
#endif
void refresh_output_mode(int &width, int &height) {
    fsr3::display_hz.store(0);
    output_pixels(width, height);
    fsr3::console.store(console_profile());
    // Also cap buffers when HDMI selection is unavailable or rejected.
    cap_output_extent(width, height, fsr3::console.load());
}
float runtime_max_render_scale() { return hardware_max_render_scale(); }
float runtime_render_scale() { return g_render_scale; }
void set_runtime_render_scale(float value) { requested_scale = value; }
bool prepare_runtime(const std::string &game, const std::string &user) {
    // One guest address space per process. Resume uses the existing guest; never load a second RPX.
    g_game_root = game;
    g_user_root = user;
    LOG("prepare_runtime: game=%s user=%s", game.c_str(), user.c_str());
    fsr3::console.store(console_profile()); // Log identity even if memory admission rejects startup.
    // LocalState/latte-profile.txt turns the Latte profiler on (it reads its switch from the environment).
    if (std::filesystem::exists(std::filesystem::path(user) / "latte-profile.txt")) {
#if defined(_WIN32)
        _putenv_s("CEMU_LATTE_PROFILE", "1");
#else
        setenv("CEMU_LATTE_PROFILE", "1", 1);
#endif
    }
    // Ship the single back recorder. Modes 0/1/2/3 remain explicitly selectable.
    _putenv_s("CEMU_LATTE_PIPELINE",
              ReadLattePipelineMode(std::filesystem::path(user) / "latte-pipeline.txt"));
    try {
        mem::init();
        dispatch::init();
        LOG("prepare_runtime: memory and dispatch ready, loading RPX");
        std::string error;
        if (!packs::load_rpx(packs::compiled_plan(), g_game_root + "/code/U-King.rpx", g_user_root,
                             loader::load_main, loaded_rpx, error)) {
            LOG("prepare_runtime: load_rpx failed: %s", error.c_str());
            return false;
        }
        if (!error.empty())
            LOG("%s", error.c_str());
        os::init();
        debug_watch_init();
    } catch (const std::exception &e) {
        LOG("prepare_runtime: exception: %s", e.what());
        throw;
    }
    LOG("prepare_runtime: done");
    return true;
}
void start_runtime(void *window, int width, int height) {
    LOG("start_runtime: window size %dx%d", width, height);
    try {
        first_frame_presented.store(false, std::memory_order_release);
        refresh_output_mode(width, height);
        // The quick menu's video scale wins; otherwise FSR picks the scale for this console and output.
        const auto preferences = settings::get();
        const auto quality =
            preferences.upscaler == fsr::Upscaler::Off ? fsr::Quality::Auto : preferences.fsr_quality;
        g_render_scale = requested_scale > 0
                             ? std::clamp(requested_scale, 1.f, hardware_max_render_scale())
                             : fsr::select_scale(width, height, hardware_max_render_scale() <= 1.5f, quality,
                                                 preferences.scale);
#ifdef BOTW_HAS_FFX
        fsr::upscaler.store(preferences.upscaler); // The renderer reads it before settings::apply().
        const auto automatic = fsr3::select(fsr3::console.load(), sim_series_s::settings().enabled,
                                            fsr::upscaler_override(), true, true, height);
        if (height > automatic.output_height) {
            width = std::max(1, width * automatic.output_height / height);
            height = automatic.output_height;
        }
        const float display_scale = std::min(width / 1280.f, height / 720.f);
        // Up to 1080p output, native rendering measured free on Series X (the Latte CPU thread is the limit,
        // 46 presents/s at 1.0 and at 1.5); upscale only above that, where GPU cost is real.
        const bool upscale_output = automatic.path != fsr3::Path::Off && height > 1080;
        if (requested_scale <= 0 && !(std::isfinite(preferences.scale) && preferences.scale > 0) &&
            quality == fsr::Quality::Auto)
            g_render_scale = std::clamp(display_scale / (upscale_output ? automatic.ratio : 1.f), 0.5f,
                                        hardware_max_render_scale());
#endif
        if (requested_scale <= 0 && !(std::isfinite(preferences.scale) && preferences.scale > 0) &&
            fsr::effective_upscaler(preferences.upscaler) == fsr::Upscaler::Off)
            g_render_scale = native_render_scale(width, height, console_profile());
        LOG("output %dx%d, render scale %.2f", width, height, g_render_scale);
        latte::create_renderer(window, width, height); // W2: IUnknown CoreWindow, not NSView.
        settings::apply();
        packs::install_loaded(loaded_rpx, d3d12_packs::hooks());
        LOG("Graphic pack plan: %s", packs::selected_plan().fingerprint.c_str());
        auto argv = os::build_argv("U-King.rpx");
        const auto &module = loader::main_module();
        LOG("start_runtime: starting guest main thread entry=0x%08x", module.entry);
        threads::start_main(module.entry, 1, argv, module.stack_size);
        LOG("start_runtime: guest main thread started");
    } catch (const std::exception &e) {
        LOG("start_runtime: exception: %s", e.what());
        throw;
    }
}
} // namespace uwp
#endif
