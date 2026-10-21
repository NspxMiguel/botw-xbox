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
inline constexpr float width = 1280, height = 720, button_h = 56;
inline constexpr float margin = 64;
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
    // Mods share the home surface before boot; seven visible 56px pill targets.
    bool mods_mode = false;
    std::vector<std::wstring> rows;
    std::vector<bool> row_enabled;
    bool enabled(size_t row) const { return row >= row_enabled.size() || row_enabled[row]; }
    size_t first_row() const { return focus < 6 ? 0 : static_cast<size_t>(focus - 5); }
    int action_count() const { return mods_mode ? static_cast<int>(rows.size()) : 3; }
    void step(int delta) {
        for (int attempts = 0; attempts < action_count(); ++attempts) {
            focus = (focus + delta + action_count()) % action_count();
            if (!mods_mode || enabled(static_cast<size_t>(focus)))
                break;
        }
    }
    void navigate(int direction) {
        // 0 up, 1 right, 2 down, 3 left. Disc=Play, gear=Settings, boxes=Mods.
        if (mods_mode) {
            step(direction == 0 || direction == 3 ? -1 : 1);
            return;
        }
        focus = home_focus(focus, direction);
    }
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
        if (art.icon)
            disc = bitmap(*art.icon);
        {
            Ptr<ID2D1EllipseGeometry> outer, hole;
            auto center = D2D1::Point2F(640, 360);
            check(factory->CreateEllipseGeometry(D2D1::Ellipse(center, 150, 150), &outer));
            check(factory->CreateEllipseGeometry(D2D1::Ellipse(center, 20, 20), &hole));
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
        if (mods_mode) {
            auto first = first_row();
            for (size_t i = first; i < rows.size() && i < first + 7; ++i) {
                float top = 160 + (i - first) * 64;
                if (x >= art_style::margin && x <= art_style::width - art_style::margin && y >= top &&
                    y <= top + art_style::button_h && enabled(i))
                    return static_cast<int>(i);
            }
            return -1;
        }
        if ((x - 640) * (x - 640) + (y - 360) * (y - 360) <= 150 * 150)
            return 0;
        for (int i = 1; i <= 2; ++i) {
            float cy = 56 + (i - 1) * 64;
            if ((x - 1224) * (x - 1224) + (y - cy) * (y - cy) <= 24 * 24)
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
        if (mods_mode) {
            brush->SetColor(white());
            text(heading, D2D1::RectF(margin, 48, width - margin, 96), true);
            text(detail, D2D1::RectF(margin, 104, width - margin, 152));
            auto first = first_row();
            for (size_t i = first; i < rows.size() && i < first + 7; ++i) {
                float top = 160 + (i - first) * 64;
                auto rect = D2D1::RectF(margin, top, width - margin, top + button_h);
                bool focused = static_cast<int>(i) == focus && enabled(i);
                if (focused) {
                    brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b));
                    target->FillRoundedRectangle(D2D1::RoundedRect(rect, button_h / 2, button_h / 2),
                                                 brush.Get());
                }
                brush->SetColor(focused ? black() : white(enabled(i) ? 1.f : .4f));
                text(rows[i], D2D1::RectF(margin + 24, top + 17, width - margin - 24, top + button_h));
            }
            brush->SetColor(white());
            text(loading_text, D2D1::RectF(margin, 632, width - margin, 700));
            check(target->EndDraw());
            check(swapchain->Present(1, 0));
            return;
        }
        if (hero) {
            // Centered aspect-fill; never stretch the player's art.
            auto size = hero->GetSize();
            float scale = std::max(width / size.width, height / size.height);
            float x = (width - size.width * scale) / 2, y = (height - size.height * scale) / 2;
            target->DrawBitmap(hero.Get(), D2D1::RectF(x, y, width - x, height - y));
            target->FillRectangle(D2D1::RectF(0, 0, width, height), scrim.Get());
        }
        const auto center = D2D1::Point2F(640, 360);
        brush->SetColor(black(.85f));
        target->FillGeometry(disc_mask.Get(), brush.Get());
        if (disc) {
            target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), disc_mask.Get()), disc_layer.Get());
            target->SetTransform(
                D2D1::Matrix3x2F::Rotation(
                    static_cast<float>(std::fmod(disc_angle(seconds, loading, reduced_motion), 360.)),
                    center) *
                transform);
            auto size = disc->GetSize();
            float side = std::min(size.width, size.height);
            auto source = D2D1::RectF((size.width - side) / 2, (size.height - side) / 2,
                                      (size.width + side) / 2, (size.height + side) / 2);
            target->DrawBitmap(disc.Get(), D2D1::RectF(490, 210, 790, 510), 1,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
            target->SetTransform(transform);
            target->PopLayer();
        }
        // The center hole reveals the artwork rather than a painted spindle.
        brush->SetColor(white(.18f));
        target->DrawEllipse(D2D1::Ellipse(center, 149, 149), brush.Get(), 1);
        target->DrawEllipse(D2D1::Ellipse(center, 24, 24), brush.Get(), 1);
        if (focus == 0 || loading) {
            float alpha = loading && reduced_motion ? float(.55 + .25 * std::sin(seconds * 2.1)) : 1.f;
            brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, alpha));
            target->DrawEllipse(D2D1::Ellipse(center, 155, 155), brush.Get(), 3);
        }
        // Accessibility names: disc = Play, gear = Settings, stacked boxes = Mods.
        for (int i = 1; i <= 2 && !loading; ++i) {
            auto c = D2D1::Point2F(1224, 56 + (i - 1) * 64.f);
            brush->SetColor(focus == i ? D2D1::ColorF(accent.r, accent.g, accent.b) : black(.9f));
            target->FillEllipse(D2D1::Ellipse(c, 24, 24), brush.Get());
            brush->SetColor(focus == i ? black() : white());
            if (i == 1) {
                target->DrawEllipse(D2D1::Ellipse(c, 9, 9), brush.Get(), 2);
                target->DrawEllipse(D2D1::Ellipse(c, 3, 3), brush.Get(), 2);
                for (int tooth = 0; tooth < 8; ++tooth) {
                    double angle = tooth * 3.141592653589793 / 4;
                    target->DrawLine(
                        D2D1::Point2F(c.x + 9 * float(std::cos(angle)), c.y + 9 * float(std::sin(angle))),
                        D2D1::Point2F(c.x + 14 * float(std::cos(angle)), c.y + 14 * float(std::sin(angle))),
                        brush.Get(), 3);
                }
            } else {
                target->DrawRectangle(D2D1::RectF(c.x - 11, c.y - 6, c.x + 5, c.y + 10), brush.Get(), 2);
                target->DrawLine(D2D1::Point2F(c.x - 5, c.y - 11), D2D1::Point2F(c.x + 11, c.y - 11),
                                 brush.Get(), 2);
                target->DrawLine(D2D1::Point2F(c.x + 11, c.y - 11), D2D1::Point2F(c.x + 11, c.y + 5),
                                 brush.Get(), 2);
            }
        }
        brush->SetColor(white());
        text(heading, D2D1::RectF(margin, 626, 1100, 672), true);
        // Only missing-game/failure messages may accompany the title on home.
        if (!loading)
            text(detail, D2D1::RectF(margin, 674, 1150, 720));
        check(target->EndDraw());
        check(swapchain->Present(1, 0));
    }
};
} // namespace uwp
