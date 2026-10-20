// Present the existing launcher overlay without guest memory or a GPU command processor thread.
#if defined(BOTW_BUILD_GAME) && !BOTW_BUILD_GAME
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

namespace window {
void botw_draw_launcher_frame() {
    static bool initialized = false;
    if (!initialized) {
        g_renderer->Initialize();
        initialized = true;
    }
    if (!g_renderer->BeginFrame(true))
        return;
    g_renderer->ClearColorbuffer(false);
    if (g_renderer->ImguiBegin(true)) {
        LatteOverlay_render(false);
        g_renderer->ImguiEnd();
    }
    g_renderer->SwapBuffers(true, false);
}
} // namespace window
#endif
