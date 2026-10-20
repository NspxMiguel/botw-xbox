// SPDX-License-Identifier: MIT
#pragma once
// Header-only so the host's existing source list remains sufficient.
#include "art.h"
#include <d2d1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <stdexcept>
#include <string>
#include <wrl/client.h>
#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#endif

namespace uwp {
namespace art_style {
inline constexpr float width = 1280, height = 720, button_y = 624, button_h = 56;
inline constexpr float margin = 64, button_w = 352, gap = 24;
inline constexpr float title_size = 34, body_size = 17;
inline D2D1_COLOR_F black(float alpha = 1) { return D2D1::ColorF(0, 0, 0, alpha); }
inline D2D1_COLOR_F white(float alpha = 1) { return D2D1::ColorF(1, 1, 1, alpha); }
} // namespace art_style
class ArtView {
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<IDXGISwapChain1> swapchain;
    Ptr<ID2D1Factory> factory;
    Ptr<ID2D1RenderTarget> target;
    Ptr<IDWriteFactory> fonts;
    Ptr<IDWriteTextFormat> title_font, body_font;
    Ptr<ID2D1Bitmap> hero, disc;
    Ptr<ID2D1PathGeometry> disc_mask;
    Ptr<ID2D1Layer> disc_layer;
    Ptr<ID2D1SolidColorBrush> brush;
    Ptr<ID2D1LinearGradientBrush> scrim;
    unsigned pixel_w = 0, pixel_h = 0;
    static void check(HRESULT hr) {
        if (FAILED(hr))
            throw std::runtime_error("Launcher graphics operation failed");
    }
    Ptr<ID2D1Bitmap> bitmap(const ArtImage &image) {
        std::vector<uint8_t> bgra(image.rgba.size());
        for (size_t i = 0; i < bgra.size(); i += 4) {
            auto alpha = image.rgba[i + 3];
            bgra[i] = uint8_t(image.rgba[i + 2] * alpha / 255);
            bgra[i + 1] = uint8_t(image.rgba[i + 1] * alpha / 255);
            bgra[i + 2] = uint8_t(image.rgba[i] * alpha / 255);
            bgra[i + 3] = alpha;
        }
        Ptr<ID2D1Bitmap> result;
        check(target->CreateBitmap(D2D1::SizeU(image.width, image.height), bgra.data(), image.width * 4,
                                   D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                            D2D1_ALPHA_MODE_PREMULTIPLIED)),
                                   &result));
        return result;
    }
    void text(const std::wstring &value, D2D1_RECT_F rect, bool title = false) {
        target->DrawText(value.c_str(), UINT32(value.size()), title ? title_font.Get() : body_font.Get(),
                         rect, brush.Get());
    }

  public:
    bool shown = false, loading = false;
    int focus = 0, selected = -1;
    ArtColor accent;
    std::wstring heading, detail, loading_text;
    std::array<std::wstring, 3> buttons;
    bool ready() const { return target != nullptr; }
    void close() {
        // Release the CoreWindow swap chain now: the D3D12 renderer creates its own on the same window.
        if (device) {
            Ptr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            context->ClearState();
            context->Flush();
        }
        *this = ArtView{};
    }
    void open(IUnknown *window, unsigned w, unsigned h, const GameArt &art) {
        close();
        pixel_w = w;
        pixel_h = h;
        accent = art.accent();
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
        Ptr<IDXGIDevice> dxgi;
        check(device.As(&dxgi));
        Ptr<IDXGIAdapter> adapter;
        check(dxgi->GetAdapter(&adapter));
        Ptr<IDXGIFactory2> dxgi_factory;
        check(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = w;
        desc.Height = h;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        check(dxgi_factory->CreateSwapChainForCoreWindow(device.Get(), window, &desc, nullptr, &swapchain));
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
        Ptr<IDXGISurface> surface;
        check(swapchain->GetBuffer(0, IID_PPV_ARGS(&surface)));
        auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        check(factory->CreateDxgiSurfaceRenderTarget(surface.Get(), properties, &target));
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown **>(fonts.GetAddressOf())));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, art_style::title_size, L"", &title_font));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      art_style::body_size, L"", &body_font));
        check(target->CreateSolidColorBrush(art_style::white(), &brush));
        // Three stops; the upper half stays untouched and the footer closes to near-black.
        D2D1_GRADIENT_STOP stops[] = {
            {.5f, art_style::black(0)}, {.82f, art_style::black(.82f)}, {1, art_style::black(.98f)}};
        Ptr<ID2D1GradientStopCollection> collection;
        check(target->CreateGradientStopCollection(stops, 3, &collection));
        check(target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, art_style::height)),
            collection.Get(), &scrim));
        if (art.tv)
            hero = bitmap(*art.tv);
        if (art.icon) {
            disc = bitmap(*art.icon);
            Ptr<ID2D1EllipseGeometry> outer, hole;
            auto center = D2D1::Point2F(1112, 512);
            check(factory->CreateEllipseGeometry(D2D1::Ellipse(center, 74, 74), &outer));
            check(factory->CreateEllipseGeometry(D2D1::Ellipse(center, 10, 10), &hole));
            check(factory->CreatePathGeometry(&disc_mask));
            Ptr<ID2D1GeometrySink> sink;
            check(disc_mask->Open(&sink));
            check(outer->CombineWithGeometry(hole.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get()));
            check(sink->Close());
            check(target->CreateLayer(&disc_layer));
        }
        shown = true;
    }
    int hit(float x, float y) const {
        x *= art_style::width / pixel_w;
        y *= art_style::height / pixel_h;
        for (int i = 0; i < 3; ++i) {
            float left = art_style::margin + i * (art_style::button_w + art_style::gap);
            if (x >= left && x <= left + art_style::button_w && y >= art_style::button_y &&
                y <= art_style::button_y + art_style::button_h)
                return i;
        }
        return -1;
    }
    void draw(double seconds, bool reduced_motion) {
        if (!shown || !target)
            return;
        using namespace art_style;
        auto transform = D2D1::Matrix3x2F::Scale(pixel_w / width, pixel_h / height);
        target->BeginDraw();
        target->SetTransform(transform);
        target->Clear(black());
        if (hero) {
            // Centered aspect-fill; never stretch the player's art.
            auto size = hero->GetSize();
            float scale = std::max(width / size.width, height / size.height);
            float x = (width - size.width * scale) / 2, y = (height - size.height * scale) / 2;
            target->DrawBitmap(hero.Get(), D2D1::RectF(x, y, width - x, height - y));
            target->FillRectangle(D2D1::RectF(0, 0, width, height), scrim.Get());
        }
        if (disc) {
            const auto center = D2D1::Point2F(1112, 512);
            target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), disc_mask.Get()), disc_layer.Get());
            target->SetTransform(
                D2D1::Matrix3x2F::Rotation(disc_angle(seconds, loading, reduced_motion), center) * transform);
            auto size = disc->GetSize();
            float side = std::min(size.width, size.height);
            auto source = D2D1::RectF((size.width - side) / 2, (size.height - side) / 2,
                                      (size.width + side) / 2, (size.height + side) / 2);
            target->DrawBitmap(disc.Get(), D2D1::RectF(1038, 438, 1186, 586), 1,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
            target->SetTransform(transform);
            target->PopLayer();
            brush->SetColor(white(.18f));
            target->DrawEllipse(D2D1::Ellipse(center, 73, 73), brush.Get(), 1);

            target->DrawEllipse(D2D1::Ellipse(center, 14, 14), brush.Get(), 1);
        }
        // Backplate is unnecessary: all copy lives in the closed part of the scrim.
        brush->SetColor(white());
        text(heading, D2D1::RectF(margin, 540, 1000, 586), true);
        text(loading ? loading_text : detail, D2D1::RectF(margin, 588, 1000, 620));
        for (int i = 0; i < 3 && !loading; ++i) {
            float left = margin + i * (button_w + gap);
            auto rect = D2D1::RectF(left, button_y, left + button_w, button_y + button_h);
            auto pill = D2D1::RoundedRect(rect, button_h / 2, button_h / 2);
            brush->SetColor(i == focus ? D2D1::ColorF(accent.r, accent.g, accent.b) : black(.94f));
            target->FillRoundedRectangle(pill, brush.Get());
            brush->SetColor(i == focus ? black() : white());
            text(buttons[i],
                 D2D1::RectF(left + 24, button_y + 17, left + button_w - 24, button_y + button_h));
        }
        check(target->EndDraw());
        check(swapchain->Present(1, 0));
    }
};
} // namespace uwp
