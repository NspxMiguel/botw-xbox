// SPDX-License-Identifier: MIT
#pragma once
#include "host.h"
#include "quick_menu.h"
#include "ui_style.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <d2d1_1.h>
#include <d3d11on12.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <memory>
#include <string>
#include <vector>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.h>
#include <wrl/client.h>
#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#endif

namespace uwp {
namespace capture_detail {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
inline void check(HRESULT result) { winrt::check_hresult(result); }

// Renderer-thread owned. The UI and recording worker never use the overlay context directly.
struct Graphics {
    ComPtr<IDXGISwapChain3> chain;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11On12Device> bridge;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2d;
    ComPtr<ID2D1DeviceContext> canvas;
    ComPtr<IDWriteFactory> fonts;
    ComPtr<IDWriteTextFormat> font;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::vector<ComPtr<ID3D11Resource>> wrapped;
    std::vector<ComPtr<ID2D1Bitmap1>> targets;

    Graphics(void *native_device, void *native_queue, void *native_chain) {
        check(static_cast<IDXGISwapChain1 *>(native_chain)->QueryInterface(IID_PPV_ARGS(&chain)));
        check(chain->GetDesc1(&desc));
        IUnknown *queues[] = {static_cast<ID3D12CommandQueue *>(native_queue)};
        check(D3D11On12CreateDevice(static_cast<ID3D12Device *>(native_device),
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0, &device,
                                    &context, nullptr));
        check(device.As(&bridge));
        ComPtr<IDXGIDevice> dxgi;
        check(device.As(&dxgi));
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
        check(factory->CreateDevice(dxgi.Get(), &d2d));
        check(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &canvas));
        canvas->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        wrapped.resize(desc.BufferCount);
        targets.resize(desc.BufferCount);
        for (UINT i = 0; i < desc.BufferCount; ++i) {
            ComPtr<ID3D12Resource> buffer;
            check(chain->GetBuffer(i, IID_PPV_ARGS(&buffer)));
            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_RENDER_TARGET;
            check(bridge->CreateWrappedResource(buffer.Get(), &flags, D3D12_RESOURCE_STATE_PRESENT,
                                                D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped[i])));
            ComPtr<IDXGISurface> surface;
            check(wrapped[i].As(&surface));
            check(canvas->CreateBitmapFromDxgiSurface(
                surface.Get(),
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                        D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE)),
                &targets[i]));
        }
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown **>(fonts.GetAddressOf())));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.f, L"",
                                      &font));
        check(font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        check(canvas->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush));
    }
    ~Graphics() {
        canvas->SetTarget(nullptr);
        targets.clear();
        wrapped.clear();
        context->ClearState();
        // Submit deferred wrapped-resource destruction before DXGI releases/resizes the chain.
        context->Flush();
    }
};

inline std::unique_ptr<Graphics> graphics;
inline uint64_t graphics_generation = 0;
inline Graphics &get_graphics(void *device, void *queue, void *chain) {
    if (graphics && graphics->chain.Get() != static_cast<IDXGISwapChain1 *>(chain))
        graphics.reset();
    if (!graphics) {
        graphics = std::make_unique<Graphics>(device, queue, chain);
        ++graphics_generation;
    }
    return *graphics;
}

struct WrappedGuard {
    Graphics &g;
    ID3D11Resource *resource;
    WrappedGuard(Graphics &graphics, UINT index) : g(graphics), resource(g.wrapped[index].Get()) {
        g.bridge->AcquireWrappedResources(&resource, 1);
    }
    WrappedGuard(const WrappedGuard &) = delete;
    WrappedGuard &operator=(const WrappedGuard &) = delete;
    ~WrappedGuard() {
        g.canvas->SetTarget(nullptr);
        g.bridge->ReleaseWrappedResources(&resource, 1);
        g.context->Flush();
    }
};
} // namespace capture_detail

inline std::atomic<uint64_t> debug_game_flips{0};
struct DebugHud {
    bool enabled = false;
    std::wstring device, version;
    uint64_t generation = 0, previous_flips = 0, presents = 0;
    capture_detail::Clock::time_point fps_time{}, memory_time{};
    std::wstring fps = L"FPS 0 · frame 0.0 ms", memory;
    std::array<std::wstring, 5> line_text;
    std::array<capture_detail::ComPtr<IDWriteTextLayout>, 5> layouts;
    float width = 0;

    void start() {
        auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        enabled = values.HasKey(L"debugHud") && winrt::unbox_value<bool>(values.Lookup(L"debugHud"));
        if (!enabled)
            return;
        // Preserve the OS's model name verbatim, including names introduced by future consoles.
        device = winrt::Windows::System::Profile::AnalyticsInfo::DeviceForm();
        auto v = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
        version = std::to_wstring(v.Major) + L"." + std::to_wstring(v.Minor) + L"." +
                  std::to_wstring(v.Build) + L"." + std::to_wstring(v.Revision);
        previous_flips = debug_game_flips.load(std::memory_order_relaxed);
        fps_time = capture_detail::Clock::now();
    }
};
inline DebugHud debug_hud;

// Apparatus controls remain visible until the player returns to normal camera controls.
inline void draw_gamepad_hint(void *device, void *queue, void *chain) {
    using namespace capture_detail;
    if (quick_menu.requested)
        return;
    std::wstring hint;
    {
        std::lock_guard lock(quick_menu.mutex);
        hint = quick_menu.motion_hint;
    }
    if (hint.empty())
        return;
    auto &g = get_graphics(device, queue, chain);
    WrappedGuard acquired(g, g.chain->GetCurrentBackBufferIndex());
    g.canvas->SetTarget(g.targets[g.chain->GetCurrentBackBufferIndex()].Get());
    g.canvas->BeginDraw();
    const float scale = g.desc.Height / 1080.f;
    const float x = (g.desc.Width / scale - 1040.f) / 2;
    g.canvas->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    g.brush->SetColor(art_style::black(art_style::quick_dim));
    g.canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, 32, x + 1040, 104), 16, 16),
                                   g.brush.Get());
    g.brush->SetColor(art_style::white());
    g.canvas->DrawText(hint.c_str(), UINT32(hint.size()), g.font.Get(),
                       D2D1::RectF(x + 20, 44, x + 1020, 100), g.brush.Get());
    check(g.canvas->EndDraw());
}

inline void draw_debug_hud(void *device, void *queue, void *chain) {
    using namespace capture_detail;
    if (!debug_hud.enabled || quick_menu.requested)
        return;
    auto &g = get_graphics(device, queue, chain);
    auto &h = debug_hud;
    auto now = Clock::now();
    if (!h.generation) {
        h.previous_flips = debug_game_flips.load(std::memory_order_relaxed);
        h.fps_time = now;
    }
    bool changed = h.generation != graphics_generation;
    ++h.presents;
    auto elapsed = std::chrono::duration<double>(now - h.fps_time).count();
    if (elapsed >= 1.) {
        auto flips = debug_game_flips.load(std::memory_order_relaxed);
        // FPS is what reaches the screen (presents); the game's own flips can run ahead of it.
        auto game = flips - h.previous_flips;
        wchar_t text[96];
        swprintf_s(text, L"FPS %.0f · frame %.1f ms · game %.0f/s", h.presents / elapsed,
                   h.presents ? elapsed * 1000 / h.presents : 0., game / elapsed);
        h.presents = 0;
        h.fps = text;
        h.previous_flips = flips;
        h.fps_time = now;
        changed = true;
    }
    if (now - h.memory_time >= std::chrono::milliseconds(500)) {
        using winrt::Windows::System::MemoryManager;
        h.memory = L"RAM " + std::to_wstring(MemoryManager::AppMemoryUsage() >> 20) + L" / " +
                   std::to_wstring(MemoryManager::AppMemoryUsageLimit() >> 20) + L" MB";
        h.memory_time = now;
        changed = true;
    }
    if (changed) {
        std::array<std::wstring, 5> lines{
            h.device + L" · Dev Mode",
            L"BotW Recompiled " + h.version + L" · static recompilation (not emulation)", h.fps, h.memory,
            L"Output " + std::to_wstring(g.desc.Width) + L"x" + std::to_wstring(g.desc.Height) + L" · D3D12"};
        h.width = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!h.layouts[i] || h.line_text[i] != lines[i]) {
                h.layouts[i].Reset();
                check(g.fonts->CreateTextLayout(lines[i].c_str(), UINT32(lines[i].size()), g.font.Get(), 1800,
                                                28, &h.layouts[i]));
                h.line_text[i] = std::move(lines[i]);
            }
            DWRITE_TEXT_METRICS metrics{};
            check(h.layouts[i]->GetMetrics(&metrics));
            h.width = std::max(h.width, metrics.width + 32);
        }
        h.generation = graphics_generation;
    }
    UINT index = g.chain->GetCurrentBackBufferIndex();
    WrappedGuard acquired(g, index);
    g.canvas->SetTarget(g.targets[index].Get());
    g.canvas->BeginDraw();
    float scale = g.desc.Height / 1080.f;
    g.canvas->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    // Top right: the game keeps hearts and stamina top left and the minimap bottom right.
    float x = g.desc.Width * .97f / scale - h.width, y = 32.4f;
    g.brush->SetColor(D2D1::ColorF(0, 0, 0, .55f));
    g.canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + h.width, y + 172), 16, 16),
                                   g.brush.Get());
    g.brush->SetColor(D2D1::ColorF(1, 1, 1));
    for (size_t i = 0; i < h.layouts.size(); ++i)
        g.canvas->DrawTextLayout(D2D1::Point2F(x + 16, y + 16 + float(i) * 28), h.layouts[i].Get(),
                                 g.brush.Get());
    check(g.canvas->EndDraw());
}
} // namespace uwp
