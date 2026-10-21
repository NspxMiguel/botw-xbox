// SPDX-License-Identifier: MIT
// UWP settings replacement: no SDL touch/menu dependency. Launcher preferences live in LocalSettings.
#if defined(BOTW_PLATFORM_UWP)
#include "../../settings.h"
#include "../../audio/output.h"
#include "../../host/window.h"
#include "../../latte/latte.h"
#include "../../os.h"
#include <mutex>
namespace settings {
namespace {
std::mutex lock;
Settings current;
} // namespace
Settings get() {
    std::lock_guard guard(lock);
    return current;
}
void apply() {
    auto s = get();
    g_target_fps = s.fps;
    fsr::upscaler.store(fsr::effective_upscaler(s.upscaler));
    fsr::frame_generation.store(fsr::effective_frame_generation(s.frame_generation));
    latte::set_display_mode(s.display);
    latte::set_output_filter(s.filter);
    window::set_rumble(s.rumble);
    window::set_layout(s.layout);
    audio::set_volume(s.volume / 100.f);
}
void set(const Settings &value, bool) {
    {
        std::lock_guard guard(lock);
        current = value;
    }
    apply();
}
void load(const std::string &) { apply(); }
} // namespace settings
#endif
