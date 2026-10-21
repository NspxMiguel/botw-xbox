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
#include "first_frame.h"
#if defined(_WIN32)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.UI.Core.h>
#if __has_include(<gamingdeviceinformation.h>)
#include <combaseapi.h> // STDAPI, used by gamingdeviceinformation.h
#include <gamingdeviceinformation.h>
#define BOTW_HAS_GAMING_DEVICE_INFO 1
#endif
#endif
#include <algorithm>
#include <cstdlib>

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
void output_pixels([[maybe_unused]] int &width, [[maybe_unused]] int &height) {
#if defined(_WIN32)
    try {
        const auto hdmi =
            winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
        if (hdmi) {
#ifdef BOTW_HAS_FFX
            static winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation monitored{nullptr};
            if (!monitored) {
                monitored = hdmi;
                monitored.DisplayModesChanged([](auto const &sender, auto const &) {
                    try {
                        const auto current = sender.GetCurrentDisplayMode();
                        fsr3::display_hz.store(current ? current.RefreshRate() : 0);
                    } catch (const winrt::hresult_error &) {
                        fsr3::display_hz.store(0);
                    }
                });
            }
#endif
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
fsr3::Console console_profile() {
    if (sim_series_s::settings().enabled)
        return fsr3::Console::SeriesS;
#if defined(BOTW_HAS_GAMING_DEVICE_INFO)
    GAMING_DEVICE_MODEL_INFORMATION info{};
    if (SUCCEEDED(GetGamingDeviceModelInformation(&info)) &&
        info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT) {
        if (info.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S)
            return fsr3::Console::SeriesS;
        if (info.deviceId == GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X)
            return fsr3::Console::SeriesX;
    }
#endif
    return fsr3::Console::Desktop;
}
// Series S holds 60 fps at 1080p internal; the swap chain stays at the output size.
float hardware_max_render_scale() { return console_profile() == fsr3::Console::SeriesS ? 1.5f : 3.f; }
} // namespace
namespace {
float requested_scale = 0;
}
void refresh_output_mode(int &width, int &height) {
#ifdef BOTW_HAS_FFX
    fsr3::display_hz.store(0);
    output_pixels(width, height);
    fsr3::console.store(console_profile());
    const auto policy =
        fsr3::select(fsr3::console.load(), sim_series_s::settings().enabled, "", true, true, height);
    if (height > policy.output_height) {
        width = std::max(1, width * policy.output_height / height);
        height = policy.output_height;
    }
#else
    (void)width;
    (void)height;
#endif
}
float runtime_max_render_scale() { return hardware_max_render_scale(); }
float runtime_render_scale() { return g_render_scale; }
void set_runtime_render_scale(float value) { requested_scale = value; }
bool prepare_runtime(const std::string &game, const std::string &user) {
    // One guest address space per process. Resume uses the existing guest; never load a second RPX.
    g_game_root = game;
    g_user_root = user;
    mem::init();
    dispatch::init();
    std::string error;
    if (!packs::load_rpx(packs::compiled_plan(), g_game_root + "/code/U-King.rpx", g_user_root,
                         loader::load_main, loaded_rpx, error))
        return false;
    if (!error.empty())
        LOG("%s", error.c_str());
    os::init();
    debug_watch_init();
    return true;
}
void start_runtime(void *window, int width, int height) {
    first_frame_presented.store(false, std::memory_order_release);
    output_pixels(width, height);
    fsr3::console.store(console_profile());
    // The quick menu's video scale wins; otherwise FSR picks the scale for this console and output.
    const auto preferences = settings::get();
    const auto quality =
        preferences.upscaler == fsr::Upscaler::Off ? fsr::Quality::Auto : preferences.fsr_quality;
    g_render_scale = requested_scale > 0
                         ? std::clamp(requested_scale, 1.f, hardware_max_render_scale())
                         : fsr::select_scale(width, height, hardware_max_render_scale() <= 1.5f, quality,
                                             preferences.scale);
#ifdef BOTW_HAS_FFX
    const char *override_value = std::getenv("BOTW_UPSCALER");
    const auto automatic = fsr3::select(fsr3::console.load(), sim_series_s::settings().enabled,
                                        override_value ? override_value : "", true, true, height);
    if (height > automatic.output_height) {
        width = std::max(1, width * automatic.output_height / height);
        height = automatic.output_height;
    }
    const float display_scale = std::min(width / 1280.f, height / 720.f);
    if (requested_scale <= 0 && !(std::isfinite(preferences.scale) && preferences.scale > 0) &&
        quality == fsr::Quality::Auto)
        g_render_scale =
            std::clamp(display_scale / (automatic.path == fsr3::Path::Off ? 1.f : automatic.ratio), 0.5f,
                       hardware_max_render_scale());
#endif
    LOG("output %dx%d, render scale %.2f", width, height, g_render_scale);
    latte::create_renderer(window, width, height); // W2: IUnknown CoreWindow, not NSView.
    settings::apply();
    packs::install_loaded(loaded_rpx, d3d12_packs::hooks());
    // The base texture constructor consumes the built-in scaling rules once.
    // Accepted pack texture rules supply all overrides instead.
    if (const auto *plan = d3d12_packs::active_plan) {
        for (const auto &pack : plan->packs)
            if (!pack.textures.empty()) {
                g_render_scale = 1.f;
                break;
            }
    }
    auto argv = os::build_argv("U-King.rpx");
    const auto &module = loader::main_module();
    threads::start_main(module.entry, 1, argv, module.stack_size);
}
} // namespace uwp
#endif
