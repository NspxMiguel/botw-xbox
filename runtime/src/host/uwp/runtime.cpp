// SPDX-License-Identifier: MIT
#if defined(BOTW_PLATFORM_UWP)
#include "runtime.h"
#include "../../hle.h"
#include "../../latte/latte.h"
#include "../../loader.h"
#include "../../os.h"
#include "../../settings.h"
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
bool prepare_runtime(const std::string &game, const std::string &user) {
    // One guest address space per process. Resume uses the existing guest; never load a second RPX.
    g_game_root = game;
    g_user_root = user;
    mem::init();
    dispatch::init();
    if (!loader::load_main(g_game_root + "/code/U-King.rpx"))
        return false;
    os::init();
    debug_watch_init();
    return true;
}
void start_runtime(void *window, int width, int height) {
    g_render_scale = std::clamp(std::min(width / 1280.f, height / 720.f), 1.f, 3.f);
    latte::create_renderer(window, width, height); // W2: IUnknown CoreWindow, not NSView.
    settings::apply();
    auto argv = os::build_argv("U-King.rpx");
    const auto &module = loader::main_module();
    threads::start_main(module.entry, 1, argv, module.stack_size);
}
} // namespace uwp
#endif
