// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace uwp {
// Borrowed IUnknown* for IDXGIFactory2::CreateSwapChainForCoreWindow. Never an HWND/SDL_Window.
// Valid from SetWindow through Uninitialize. Renderer must AddRef if retaining it.
void *core_window();
void present_quick_menu(void *device, void *queue, void *swapchain);
void present_overlays(void *device, void *queue, void *swapchain); // every frame: debug HUD, recorder
// Renderer thread, before backbuffer references are released or ResizeBuffers is called.
void release_presentation_resources(void *swapchain);
bool gamepad_screen_requested(); // True in full-screen DRC mode; compositor also supports Corner.
bool presentation_allowed();     // W2: skip presentation while suspended/invisible.
// W1/W2 may install a nonblocking lifecycle callback before CoreApplication::Run.
// Called on UI thread; must not synchronously wait for UI work from another thread.
using VisibilityCallback = void (*)(bool active);
void set_visibility_callback(VisibilityCallback callback);
} // namespace uwp
