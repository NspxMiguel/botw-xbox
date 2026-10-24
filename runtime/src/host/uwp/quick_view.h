// SPDX-License-Identifier: MIT
#pragma once
#include "host.h"
#include "polish.h"
#include "quick_menu.h"
#include "ui_style.h"
#include <d2d1_1.h>
#include <d3d11on12.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl/client.h>
#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif
namespace uwp {
// Executed only on the renderer thread, after its command list has been submitted.
// Wrapped resources enter and leave PRESENT; no second CoreWindow swap chain is created.
inline void draw_quick_menu(void *native_device, void *native_queue, void *native_chain) {
    if (!quick_menu.requested)
        return;
    using Microsoft::WRL::ComPtr;
    auto check = [](HRESULT result) {
        if (FAILED(result))
            throw std::runtime_error("Quick menu graphics failed");
    };
    struct PauseGuard {
        PauseGuard() { quick_menu.paused = true; }
        ~PauseGuard() {
            quick_menu.paused = false;
            quick_menu.requested = false;
        }
    } pause;
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ApartmentGuard {
        bool owned;
        ~ApartmentGuard() {
            if (owned)
                CoUninitialize();
        }
    } apartment_guard{SUCCEEDED(apartment)};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    IUnknown *queues[] = {static_cast<ID3D12CommandQueue *>(native_queue)};
    check(D3D11On12CreateDevice(static_cast<ID3D12Device *>(native_device), D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                nullptr, 0, queues, 1, 0, &device, &context, nullptr));
    ComPtr<ID3D11On12Device> bridge;
    check(device.As(&bridge));
    ComPtr<IDXGISwapChain3> chain;
    check(static_cast<IDXGISwapChain1 *>(native_chain)->QueryInterface(IID_PPV_ARGS(&chain)));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    check(chain->GetDesc1(&desc));
    std::vector<ComPtr<ID3D11Resource>> wrapped(desc.BufferCount);
    std::vector<ComPtr<ID2D1Bitmap1>> targets(desc.BufferCount);
    ComPtr<IDXGIDevice> dxgi;
    check(device.As(&dxgi));
    ComPtr<ID2D1Factory1> factory;
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
    ComPtr<ID2D1Device> d2d;
    check(factory->CreateDevice(dxgi.Get(), &d2d));
    ComPtr<ID2D1DeviceContext> canvas;
    check(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &canvas));
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
    // Snapshot is independent of rotating backbuffers; captures exclude the overlay.
    ComPtr<ID3D11Texture2D> original;
    check(wrapped[chain->GetCurrentBackBufferIndex()].As(&original));
    D3D11_TEXTURE2D_DESC texture{};
    original->GetDesc(&texture);
    texture.BindFlags = 0;
    texture.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> snapshot;
    check(device->CreateTexture2D(&texture, nullptr, &snapshot));
    struct WrappedGuard {
        ID3D11On12Device *bridge;
        ID3D11DeviceContext *context;
        ID3D11Resource *resource;
        ID2D1DeviceContext *canvas = nullptr;
        WrappedGuard(ID3D11On12Device *b, ID3D11DeviceContext *c, ID3D11Resource *r,
                     ID2D1DeviceContext *target = nullptr)
            : bridge(b), context(c), resource(r), canvas(target) {
            bridge->AcquireWrappedResources(&resource, 1);
        }
        WrappedGuard(const WrappedGuard &) = delete;
        WrappedGuard &operator=(const WrappedGuard &) = delete;
        ~WrappedGuard() {
            if (canvas)
                canvas->SetTarget(nullptr);
            bridge->ReleaseWrappedResources(&resource, 1);
            context->Flush();
        }
    };
    {
        auto source = wrapped[chain->GetCurrentBackBufferIndex()].Get();
        WrappedGuard acquired(bridge.Get(), context.Get(), source);
        context->CopyResource(snapshot.Get(), source);
    }
    ComPtr<IDWriteFactory> fonts;
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown **>(fonts.GetAddressOf())));
    ComPtr<IDWriteTextFormat> font, title_font;
    check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, art_style::body_size, L"", &font));
    check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, art_style::title_size, L"", &title_font));
    ComPtr<ID2D1SolidColorBrush> brush;
    check(canvas->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush));
    Spring entrance;
    auto last = std::chrono::steady_clock::now();
    while (quick_menu.requested) {
        if (!presentation_allowed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        std::vector<std::wstring> rows;
        std::wstring title, hints, path;
        int focus;
        bool reduced;
        {
            std::lock_guard lock(quick_menu.mutex);
            rows = quick_menu.rows;
            title = quick_menu.title;
            hints = quick_menu.hints;
            focus = quick_menu.focus;
            reduced = quick_menu.reduced_motion;
            path = quick_menu.screenshot_path;
        }
        if (quick_menu.capture.exchange(false)) {
            try {
                auto staging_desc = texture;
                staging_desc.Usage = D3D11_USAGE_STAGING;
                staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;
                check(device->CreateTexture2D(&staging_desc, nullptr, &staging));
                context->CopyResource(staging.Get(), snapshot.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                struct Unmap {
                    ID3D11DeviceContext *c;
                    ID3D11Resource *r;
                    ~Unmap() { c->Unmap(r, 0); }
                } unmap{context.Get(), staging.Get()};
                ComPtr<IWICImagingFactory> wic;
                check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&wic)));
                ComPtr<IWICStream> stream;
                check(wic->CreateStream(&stream));
                check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
                ComPtr<IWICBitmapEncoder> encoder;
                check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
                check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
                ComPtr<IWICBitmapFrameEncode> frame;
                check(encoder->CreateNewFrame(&frame, nullptr));
                check(frame->Initialize(nullptr));
                check(frame->SetSize(desc.Width, desc.Height));
                WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
                check(frame->SetPixelFormat(&format));
                ComPtr<IWICBitmap> image;
                check(wic->CreateBitmapFromMemory(desc.Width, desc.Height, GUID_WICPixelFormat32bppRGBA,
                                                  mapped.RowPitch, mapped.RowPitch * desc.Height,
                                                  static_cast<BYTE *>(mapped.pData), &image));
                ComPtr<IWICFormatConverter> converter;
                check(wic->CreateFormatConverter(&converter));
                check(converter->Initialize(image.Get(), format, WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom));
                check(frame->WriteSource(converter.Get(), nullptr));
                check(frame->Commit());
                check(encoder->Commit());
                quick_menu.capture_result = 1;
            } catch (...) {
                quick_menu.capture_result = -1;
            }
        }
        auto now = std::chrono::steady_clock::now();
        entrance.step(1, std::chrono::duration<double>(now - last).count(), 12, .6);
        last = now;
        float opacity = reduced ? 1.f : float(std::clamp(entrance.value, 0., 1.));
        float offset = reduced ? 0.f : float(24 * (1 - entrance.value));
        UINT index = chain->GetCurrentBackBufferIndex();
        auto resource = wrapped[index].Get();
        {
            WrappedGuard acquired(bridge.Get(), context.Get(), resource, canvas.Get());
            context->CopyResource(resource, snapshot.Get());
            canvas->SetTarget(targets[index].Get());
            canvas->BeginDraw();
            canvas->SetTransform(D2D1::Matrix3x2F::Scale(desc.Width / 1280.f, desc.Height / 720.f));
            brush->SetColor(art_style::black(art_style::quick_dim * opacity));
            canvas->FillRectangle(D2D1::RectF(0, 0, 1280, 720), brush.Get());
            auto text = [&](const std::wstring &s, float x, float y, bool heading = false) {
                canvas->DrawText(s.c_str(), UINT32(s.size()), heading ? title_font.Get() : font.Get(),
                                 D2D1::RectF(x, y, 1220, y + 44), brush.Get());
            };
            brush->SetColor(art_style::white(opacity));
            text(title, 64 + offset, 40, true);
            for (size_t i = 0; i < rows.size(); ++i) {
                const float row_step = std::min(56.f, 532.f / std::max(1.f, float(rows.size())));
                float y = 108 + float(i) * row_step;
                bool selected = int(i) == focus;
                brush->SetColor(art_style::white((selected ? 1.f : art_style::quick_surface) * opacity));
                canvas->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(64 + offset, y, 650 + offset, y + row_step - 6),
                                      (row_step - 6) / 2, (row_step - 6) / 2),
                    brush.Get());
                brush->SetColor(selected ? art_style::black(opacity) : art_style::white(opacity));
                text(rows[i], 88 + offset, y + (row_step - 32) / 2);
            }
            brush->SetColor(art_style::white(opacity));
            text(hints, 64, 650);
            check(canvas->EndDraw());
        }
        check(chain->Present(1, 0));
    }
    canvas->SetTarget(nullptr);
    context->ClearState();
    context->Flush();
}
} // namespace uwp
