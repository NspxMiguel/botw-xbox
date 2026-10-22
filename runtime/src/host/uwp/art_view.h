// SPDX-License-Identifier: MIT
#pragma once
// Header-only so the host's existing source list remains sufficient.
#include "art.h"
#include "materials.h"
#include "store_view.h"
#include "ui_style.h"
#include <chrono>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <map>
#include <stdexcept>
#include <string>
#include <wrl/client.h>
#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#endif

// MinGW's effects header omits the perspective property enumeration (SDK ABI indices).
#if defined(__MINGW32__)
inline constexpr UINT32 D2D1_3DPERSPECTIVETRANSFORM_PROP_DEPTH = 2;
inline constexpr UINT32 D2D1_3DPERSPECTIVETRANSFORM_PROP_PERSPECTIVE_ORIGIN = 3;
inline constexpr UINT32 D2D1_3DPERSPECTIVETRANSFORM_PROP_LOCAL_OFFSET = 4;
inline constexpr UINT32 D2D1_3DPERSPECTIVETRANSFORM_PROP_ROTATION_ORIGIN = 6;
inline constexpr UINT32 D2D1_3DPERSPECTIVETRANSFORM_PROP_ROTATION = 7;
#endif
namespace uwp {

class ArtView {
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<IDXGISwapChain1> swapchain;
    Ptr<ID2D1Factory1> factory;
    Ptr<ID2D1Device> d2d_device;
    Ptr<ID2D1DeviceContext> target;
    Ptr<ID2D1Bitmap1> surface_bitmap, previous_frame, transition_frame;
    bool frame_saved = false;
    double page_age = 1;
    std::wstring last_page;
    Ptr<ID2D1Bitmap> sheen, silver, silver_bg, halo;
    Ptr<ID2D1Effect> perspective, gold_blur;
    Ptr<ID2D1BitmapRenderTarget> gold_target;
    Ptr<ID2D1PathGeometry> question;
    Spring pitch, yaw, icon_pop[2], disc_focus;
    std::chrono::steady_clock::time_point last_tick{};
    double elapsed = 0;
    Ptr<IDWriteFactory> fonts;
    Ptr<IDWriteTextFormat> title_font, body_font, mono_font, store_title_font, store_card_font,
        store_body_font, store_meta_font, credit_font;
    Ptr<ID2D1Bitmap> hero, disc, plateau, shrine;
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
    Wallpaper wallpaper = Wallpaper::light;
    void set_wallpapers(const std::optional<ArtImage> &meadow, const std::optional<ArtImage> &temple) {
        if (meadow)
            plateau = bitmap(*meadow);
        if (temple)
            shrine = bitmap(*temple);
    }
    StoreView store;
    std::map<std::string, Ptr<ID2D1Bitmap>> thumbnails;
    void store_image(const std::string &url, const ArtImage &image) {
        if (thumbnails.size() >= 18)
            thumbnails.erase(thumbnails.begin());
        thumbnails[url] = bitmap(image);
    }
    bool shown = false, loading = false;
    bool missing_game = false, silver_setup = false;
    double flip_time = -1;
    float stick_x = 0, stick_y = 0;
    int focus = 0, selected = -1;
    ArtColor accent;
    std::wstring heading, detail, loading_text;
    // Mods share the home surface before boot; seven visible 56px pill targets.
    bool setup_mode = false, diagnostics_mode = false;
    std::vector<int> issue_kinds;
    std::vector<std::wstring> issue_notes;
    float progress = -1;
    std::vector<std::vector<bool>> qr_modules;
    bool mods_mode = false;
    std::vector<std::wstring> rows;
    std::vector<bool> row_enabled;
    bool enabled(size_t row) const { return row >= row_enabled.size() || row_enabled[row]; }
    size_t first_row() const {
        return diagnostics_mode ? (focus < 7 ? 0 : static_cast<size_t>(focus - 6))
               : setup_mode     ? (focus < 3 ? 0 : static_cast<size_t>(focus - 2))
                                : (focus < 6 ? 0 : static_cast<size_t>(focus - 5));
    }
    int action_count() const { return (mods_mode || setup_mode) ? static_cast<int>(rows.size()) : 3; }
    void step(int delta) {
        for (int attempts = 0; attempts < action_count(); ++attempts) {
            focus = (focus + delta + action_count()) % action_count();
            if (!(mods_mode || setup_mode) || enabled(static_cast<size_t>(focus)))
                break;
        }
    }
    void navigate(int direction) {
        if (store.active) {
            store.navigate(direction, focus);
            return;
        }
        // 0 up, 1 right, 2 down, 3 left. Disc=Play, gear=Settings, boxes=Mods.
        if (mods_mode || setup_mode) {
            step(direction == 0 || direction == 3 ? -1 : 1);
            return;
        }
        focus = missing_game ? 0 : home_focus(focus, direction); // first run: only the "?" disc
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
        check(factory->CreateDevice(dxgi.Get(), &d2d_device));
        check(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &target));
        Ptr<IDXGISurface> surface;
        check(swapchain->GetBuffer(0, IID_PPV_ARGS(&surface)));
        check(target->CreateBitmapFromDxgiSurface(
            surface.Get(),
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
            &surface_bitmap));
        target->SetTarget(surface_bitmap.Get());
        auto snapshot_properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        check(target->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, snapshot_properties, &previous_frame));
        check(target->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, snapshot_properties, &transition_frame));
        check(target->CreateEffect(CLSID_D2D13DPerspectiveTransform, &perspective));
        sheen = bitmap(disc_material(false));
        silver = bitmap(disc_material(true));
        silver_bg = bitmap(silver_background());
        halo = bitmap(ring_halo());
        check(factory->CreatePathGeometry(&question));
        Ptr<ID2D1GeometrySink> q;
        check(question->Open(&q));
        q->BeginFigure(D2D1::Point2F(606, 276), D2D1_FIGURE_BEGIN_HOLLOW);
        q->AddBezier(
            D2D1::BezierSegment(D2D1::Point2F(594, 231), D2D1::Point2F(681, 231), D2D1::Point2F(674, 276)));
        q->AddBezier(
            D2D1::BezierSegment(D2D1::Point2F(673, 296), D2D1::Point2F(640, 294), D2D1::Point2F(640, 330)));
        q->EndFigure(D2D1_FIGURE_END_OPEN);
        check(q->Close());
        check(target->CreateCompatibleRenderTarget(D2D1::SizeF(96, 88), &gold_target));
        check(target->CreateEffect(CLSID_D2D1GaussianBlur, &gold_blur));
        check(gold_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 5.f));
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown **>(fonts.GetAddressOf())));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, art_style::title_size, L"", &title_font));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      art_style::body_size, L"", &body_font));
        check(target->CreateSolidColorBrush(art_style::white(), &brush));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      art_style::store_title_size, L"", &store_title_font));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      art_style::store_card_size, L"", &store_card_font));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      art_style::store_body_size, L"", &store_body_font));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      art_style::store_meta_size, L"", &store_meta_font));
        Ptr<IDWriteInlineObject> ellipsis;
        check(fonts->CreateEllipsisTrimmingSign(store_card_font.Get(), &ellipsis));
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        check(store_card_font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        check(store_card_font->SetTrimming(&trimming, ellipsis.Get()));
        check(store_meta_font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        check(store_meta_font->SetTrimming(&trimming, ellipsis.Get()));
        check(fonts->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      art_style::body_size, L"", &mono_font));
        check(fonts->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f, L"",
                                      &credit_font));
        check(credit_font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING));
        // Bound the brightest image pixel for readable light ink, then darken toward the footer.
        D2D1_GRADIENT_STOP stops[] = {{0, art_style::black(art_style::wallpaper_top)},
                                      {.48f, art_style::black(art_style::wallpaper_middle)},
                                      {1, art_style::black(art_style::wallpaper_bottom)}};
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
        if (store.active) {
            if (store.detail) {
                if (x >= 724 && y >= 430 && y < 634)
                    return std::clamp(int((y - 430) / 64) * 2 + (x >= 970 ? 1 : 0), 0, 5);
                return -1;
            }
            if (store.error && y >= 642 && y < 684 && x >= 300)
                return -70;
            if (y >= 52 && y < 102) {
                if (x >= 820)
                    return -50;
                if (x >= 300)
                    return -60 - std::clamp(int((x - 300) / 164), 0, 2);
            }
            if (y >= 126 && y < 174 && x >= 300)
                return -40 - std::clamp(int((x - 300) / 180), 0, 4);
            if (x >= 48 && x < 276 && y >= 204 && y < 636)
                return -20 - std::clamp(int((y - 204) / 48), 0, 8);
            if (x >= 300 && x < 1216 && y >= 200 && y < 640) {
                int i = int((y - 200) / 230) * 3 + int((x - 300) / 312) + int(store.first(focus));
                if (i < int(store.cards.size()))
                    return i;
            }
            return -1;
        }
        if (mods_mode || setup_mode) {
            auto first = first_row();
            for (size_t i = first; i < rows.size() && i < first + (diagnostics_mode ? 7
                                                                   : setup_mode     ? 3
                                                                                    : 7);
                 ++i) {
                float top = (diagnostics_mode ? 124.f
                             : setup_mode     ? 440.f
                                              : 160.f) +
                            (i - first) * (diagnostics_mode ? 70.f : 64.f);
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
            if ((x - 56) * (x - 56) + (y - cy) * (y - cy) <= 24 * 24)
                return i;
        }
        return -1;
    }
    void draw_gold(double time, float opacity) {
        auto triangles = triforce_geometry();
        float pulse = float(.86 + .14 * std::sin(time * 2 * pi / 1.85));
        gold_target->BeginDraw();
        gold_target->Clear(art_style::black(0));
        Ptr<ID2D1SolidColorBrush> glow_brush;
        check(
            gold_target->CreateSolidColorBrush(D2D1::ColorF(0xd6a034, opacity * .55f * pulse), &glow_brush));
        for (auto face : triangles) {
            Ptr<ID2D1PathGeometry> shape;
            Ptr<ID2D1GeometrySink> sink;
            check(factory->CreatePathGeometry(&shape));
            check(shape->Open(&sink));
            sink->BeginFigure(D2D1::Point2F(face[0].x, face[0].y), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(face[1].x, face[1].y));
            sink->AddLine(D2D1::Point2F(face[2].x, face[2].y));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            check(sink->Close());
            gold_target->SetTransform(D2D1::Matrix3x2F::Translation(-1152, -596));
            gold_target->FillGeometry(shape.Get(), glow_brush.Get());
        }
        check(gold_target->EndDraw());
        Ptr<ID2D1Bitmap> glow;
        check(gold_target->GetBitmap(&glow));
        gold_blur->SetInput(0, glow.Get());
        target->DrawImage(gold_blur.Get(), D2D1::Point2F(1152, 596));
        for (auto face : triangles) {
            Ptr<ID2D1PathGeometry> shape;
            Ptr<ID2D1GeometrySink> sink;
            check(factory->CreatePathGeometry(&shape));
            check(shape->Open(&sink));
            sink->BeginFigure(D2D1::Point2F(face[0].x, face[0].y), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(face[1].x, face[1].y));
            sink->AddLine(D2D1::Point2F(face[2].x, face[2].y));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            check(sink->Close());
            D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x784a0e)},
                                          {.48f, D2D1::ColorF(0xd6a034)},
                                          {.72f, D2D1::ColorF(0xffeca0)},
                                          {1, D2D1::ColorF(0x784a0e)}};
            Ptr<ID2D1GradientStopCollection> collection;
            Ptr<ID2D1LinearGradientBrush> gradient;
            check(target->CreateGradientStopCollection(stops, 4, &collection));
            check(target->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, face[0].y), D2D1::Point2F(0, face[1].y)),
                collection.Get(), &gradient));
            gradient->SetOpacity(opacity * pulse);
            target->FillGeometry(shape.Get(), gradient.Get());
            brush->SetColor(D2D1::ColorF(0xffeca0, opacity * .8f * pulse));
            target->DrawLine(D2D1::Point2F(face[0].x, face[0].y + 1),
                             D2D1::Point2F(face[1].x + 1, face[1].y - 1), brush.Get(), 1.5f);
            brush->SetColor(D2D1::ColorF(0x784a0e, opacity));
            target->DrawLine(D2D1::Point2F(face[0].x, face[0].y + 1),
                             D2D1::Point2F(face[2].x - 1, face[2].y - 1), brush.Get(), 1.5f);
            // Diagonal highlight is clipped to each face, with a smooth three-second sweep.
            target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), nullptr);
            float sweep = float(std::fmod(time, 3.) / 3. * 150 - 70);
            for (int i = -8; i <= 8; ++i) {
                brush->SetColor(art_style::white(opacity * .035f * float(std::exp(-i * i / 24.))));
                target->DrawLine(D2D1::Point2F(1160 + sweep + i, 604), D2D1::Point2F(1200 + sweep + i, 676),
                                 brush.Get(), 2);
            }
            target->PopLayer();
        }
    }
    void finish_frame() {
        if (page_age < .4) {
            target->SetTransform(
                D2D1::Matrix3x2F::Scale(pixel_w / art_style::width, pixel_h / art_style::height));
            target->DrawBitmap(transition_frame.Get(), D2D1::RectF(0, 0, art_style::width, art_style::height),
                               float(1 - ease(page_age / .4)));
        }
        check(target->EndDraw());
        check(previous_frame->CopyFromBitmap(nullptr, surface_bitmap.Get(), nullptr));
        frame_saved = true;
        check(swapchain->Present(1, 0));
    }
    Spring store_pop;
    int store_previous_focus = -999;
    void draw_store(double dt, bool reduced_motion) {
        using namespace art_style;
        using namespace store_layout;
        if (store_previous_focus != focus) {
            store_pop.value = 0;
            store_pop.velocity = 0;
            store_previous_focus = focus;
        }
        store_pop.step(reduced_motion ? 0 : 1, dt, 14, .48);
        brush->SetColor(D2D1::ColorF(store_silver));
        target->FillRectangle(D2D1::RectF(0, 0, 1280, 720), brush.Get());
        auto ink = D2D1::ColorF(store_ink), muted = D2D1::ColorF(store_muted);
        auto utf = [](const std::string &s) {
            int count = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring out(size_t(count), L' ');
            if (count)
                MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), count);
            return out;
        };
        auto label = [&](Text t) { return store.label(t); };
        auto write = [&](std::wstring s, float x, float y, float w, float h, bool bold = false,
                         bool card = false, bool secondary = false) {
            target->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            target->DrawText(s.c_str(), UINT32(s.size()),
                             secondary ? store_meta_font.Get()
                             : card    ? store_card_font.Get()
                             : bold    ? store_title_font.Get()
                                       : store_body_font.Get(),
                             D2D1::RectF(x, y, x + w, y + h), brush.Get());
            target->PopAxisAlignedClip();
        };
        auto pill = [&](std::wstring s, float x, float y, float w, bool selected, bool enabled = true,
                        bool focused = false) {
            D2D1_MATRIX_3X2_F previous;
            target->GetTransform(&previous);
            if (focused && !reduced_motion) {
                float scale = 1 + float(store_pop.value) * .02f;
                target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(x + w / 2, y + 24)) *
                                     previous);
            }
            brush->SetColor(selected ? ink : white(.96f));
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + 48), 24, 24),
                                         brush.Get());
            brush->SetColor(selected ? white() : black(enabled ? .92f : .38f));
            write(s, x + 18, y + 13, w - 32, 32, false, false, true);
            target->SetTransform(previous);
        };
        auto image = [&](const mods::store::Card &c, size_t index, float x, float y, float w, float h) {
            brush->SetColor(black(.06f));
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 16, 16),
                                         brush.Get());
            if (!c.images.empty()) {
                auto found = thumbnails.find(c.images[index % c.images.size()]);
                if (found != thumbnails.end()) {
                    auto size = found->second->GetSize();
                    float scale = std::max(w / size.width, h / size.height);
                    float sw = w / scale, sh = h / scale;
                    auto src = D2D1::RectF((size.width - sw) / 2, (size.height - sh) / 2,
                                           (size.width + sw) / 2, (size.height + sh) / 2);
                    Ptr<ID2D1RoundedRectangleGeometry> clip;
                    check(factory->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 15, 15), &clip));
                    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
                    target->DrawBitmap(found->second.Get(), D2D1::RectF(x, y, x + w, y + h), 1,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
                    target->PopLayer();
                    return;
                }
            }
            brush->SetColor(muted);
            write(label(Text::store_no_image), x + 20, y + h / 2 - 12, w - 40, 30);
        };
        brush->SetColor(ink);
        write(label(Text::mods), 47, 26, 215, 64, true);
        if (store.detail) {
            auto &c = *store.detail;
            image(c, size_t(store.image), hero_x, hero_y, hero_w, hero_h);
            brush->SetColor(ink);
            write(utf(c.title), 724, 140, 508, 86, true);
            write(utf(c.author + " · " + c.source), 724, 232, 508, 30);
            write(utf(c.version + " · " + c.date), 724, 268, 508, 30);
            brush->SetColor(muted);
            write(label(c.locked ? Text::recompile_apply : Text::store_compat), 724, 310, 508, 64);
            if (!c.installed_id.empty()) {
                write(label(Text::store_order) + L" · " + std::to_wstring(c.order + 1) + L" / " +
                          std::to_wstring(c.total),
                      724, 382, 508, 36);
            }
            brush->SetColor(ink);
            write(utf(c.description), 48, 478, 640, 72);
            for (size_t i = 0; i < c.images.size(); ++i) {
                brush->SetColor(i == size_t(store.image) % c.images.size() ? ink : black(.3f));
                float x = 336 + float(i) * 14;
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, 464), 3, 3), brush.Get());
            }
            if (!c.presets.empty()) {
                std::wstring presets = label(Text::store_presets) + L": ";
                for (auto &p : c.presets)
                    presets += utf(p) + L" · ";
                write(presets, 48, 550, 640, 24);
            }
            std::array<std::wstring, 6> actions = {
                label(store.detail_error ? Text::retry
                      : c.update()       ? Text::store_update
                                         : Text::store_install),
                label(c.enabled ? Text::store_disable : Text::store_enable),
                label(Text::store_remove),
                label(Text::mods_up),
                label(Text::mods_down),
                label(Text::store_files)};
            for (int i = 0; i < 6; ++i)
                pill(actions[i], 724 + float(i % 2) * 246, 430 + float(i / 2) * 64, 230,
                     focus == i && store.action_enabled(i), store.action_enabled(i),
                     focus == i && store.action_enabled(i));
            brush->SetColor(muted);
            write(label(Text::store_files), 48, 576, 640, 24);
            size_t file_first = focus >= 6 ? size_t((focus - 6) / 3) * 3 : size_t(store.file / 3) * 3;
            for (size_t i = file_first; i < c.downloads.size() && i < file_first + 3; ++i) {
                auto &f = c.downloads[i];
                brush->SetColor(int(i) == store.file ? ink : muted);
                write((int(i) == store.file ? L"› " : L"  ") + utf(f.name) + L" · " +
                          std::to_wstring(f.size / 1024) + L" KiB",
                      48, 604 + float(i - file_first) * 24, 640, 24);
            }
            if (store.progress >= 0) {
                brush->SetColor(black(.12f));
                target->FillRectangle(D2D1::RectF(724, 638, 1200, 646), brush.Get());
                brush->SetColor(ink);
                target->FillRectangle(
                    D2D1::RectF(724, 638, 724 + 476 * std::clamp(store.progress, 0.f, 1.f), 646),
                    brush.Get());
            }
            brush->SetColor(muted);
            write(store.status.empty() ? label(Text::store_detail_hints) : store.status, 48, 684, 1184, 28);
            return;
        }
        for (int i = 0; i < 3; ++i) {
            float x = i == 0 ? 267.f : i == 1 ? 447.f : 647.f;
            float w = i == 0 ? 157.f : i == 1 ? 177.f : 200.f;
            bool selected = store.tab == i || focus == -60 - i;
            if (selected) {
                brush->SetColor(ink);
                target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, 35, x + w, 82), 24, 24),
                                             brush.Get());
            }
            brush->SetColor(selected ? white() : ink);
            write(label(static_cast<Text>(int(Text::store_explore) + i)), x + 18, 44, w - 26, 34, false,
                  true);
        }
        brush->SetColor(white());
        auto search = D2D1::RoundedRect(D2D1::RectF(47, 104, 1233, 159), 28, 28);
        target->FillRoundedRectangle(search, brush.Get());
        if (focus == -50) {
            brush->SetColor(ink);
            target->DrawRoundedRectangle(search, brush.Get(), 2);
        }
        brush->SetColor(muted);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(76, 129), 9, 9), brush.Get(), 2);
        target->DrawLine(D2D1::Point2F(82, 135), D2D1::Point2F(91, 144), brush.Get(), 2);
        write(store.query.empty() ? label(Text::store_search_placeholder) : store.query, 109, 117, 1090, 32);
        const float source_x[] = {47, 234, 396, 560, 643};
        const float source_w[] = {177, 152, 154, 73, 76};
        for (int i = 0; i < 5; ++i) {
            bool selected = store.source == i || focus == -40 - i;
            brush->SetColor(selected ? ink : white());
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(source_x[i], 177, source_x[i] + source_w[i], 215), 19, 19),
                brush.Get());
            brush->SetColor(selected ? white() : ink);
            write(store.source_label(i), source_x[i] + 12, 184, source_w[i] - 24, 30);
        }
        for (int i = 0; i < 9; ++i) {
            float y = 237 + float(i) * 41;
            D2D1_MATRIX_3X2_F before;
            target->GetTransform(&before);
            if (focus == -20 - i && !reduced_motion) {
                float scale = 1 + float(store_pop.value) * .02f;
                target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(162, y + 18)) *
                                     before);
            }
            if (store.category == i || focus == -20 - i) {
                brush->SetColor(ink);
                target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(37, y, 279, y + 36), 18, 18),
                                             brush.Get());
            }
            brush->SetColor(store.category == i || focus == -20 - i ? white() : ink);
            write(store.category_label(i), 51, y + 4, 220, 32);
            target->SetTransform(before);
        }
        auto first = store.first(focus);
        for (size_t n = 0; n < 6; ++n) {
            auto i = first + n;
            if (i >= store.cards.size() && !store.busy)
                break;
            float x = grid_x + float(n % 3) * (card_w + gap), y = grid_y + float(n / 3) * (card_h + gap);
            bool selected = int(i) == focus;
            float grow = selected && !reduced_motion ? float(store_pop.value) * 4 : 0;
            D2D1_MATRIX_3X2_F transform;
            target->GetTransform(&transform);
            target->SetTransform(D2D1::Matrix3x2F::Scale(1 + grow / card_w, 1 + grow / card_w,
                                                         D2D1::Point2F(x + card_w / 2, y + card_h / 2)) *
                                 transform);
            brush->SetColor(white());
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + card_w, y + card_h), 18, 18),
                                         brush.Get());
            if (selected) {
                brush->SetColor(ink);
                target->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(x - 4, y - 4, x + card_w + 4, y + card_h + 4), 20, 20),
                    brush.Get(), 3);
            }
            if (i >= store.cards.size()) {
                brush->SetColor(black(float(.06 + .04 * std::sin(elapsed * 3))));
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(x + 3, y + 3, x + card_w - 3, y + image_h + 3), 16, 16),
                    brush.Get());
                target->FillRectangle(D2D1::RectF(x + 16, y + 136, x + 238, y + 154), brush.Get());
                target->SetTransform(transform);
                continue;
            }
            auto &c = store.cards[i];
            image(c, 0, x + 3, y + 3, card_w - 6, image_h);
            brush->SetColor(ink);
            write(utf(c.title), x + 13, y + 127, card_w - 26, 32, false, true);
            brush->SetColor(muted);
            std::wstring meta = c.source == "Graphic packs" ? L"GitHub packs" : utf(c.source);
            if (c.size)
                meta += L" · " + std::to_wstring(c.size / 1024) + L" KiB";
            else if (c.downloads_count)
                meta += L" · ↓ " + std::to_wstring(c.downloads_count);
            write(meta, x + 13, y + 160, card_w - 26, 26, false, false, true);
            if (!c.installed_id.empty()) {
                float badge_w = c.update() ? 100.f : 34.f;
                float right = x + card_w - 10;
                brush->SetColor(white(.96f));
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(right - badge_w, y + 10, right, y + 40), 15, 15),
                    brush.Get());
                brush->SetColor(ink);
                if (c.update())
                    write(label(Text::store_update_badge), right - badge_w + 10, y + 12, badge_w - 16, 26);
                else {
                    target->DrawLine(D2D1::Point2F(right - 25, y + 25), D2D1::Point2F(right - 19, y + 31),
                                     brush.Get(), 2);
                    target->DrawLine(D2D1::Point2F(right - 19, y + 31), D2D1::Point2F(right - 9, y + 19),
                                     brush.Get(), 2);
                }
            }
            target->SetTransform(transform);
        }
        if (store.cards.empty() && !store.busy) {
            brush->SetColor(ink);
            write(label(store.error ? Text::store_error : Text::store_empty), 340, 300, 820, 60, true);
            brush->SetColor(muted);
            write(label(store.source == 3   ? Text::store_link_hint
                        : store.source == 4 ? Text::store_usb_hint
                                            : Text::store_empty_hint),
                  340, 374, 820, 80);
            write(store.recent, 340, 468, 820, 120);
        }
        if (store.error && focus == -70) {
            brush->SetColor(ink);
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(300, 642, 1232, 684), 21, 21),
                                         brush.Get());
        }
        brush->SetColor(store.error && focus == -70 ? white() : muted);
        write(store.offline ? label(Text::store_offline)
              : store.busy  ? label(Text::store_busy)
                            : store.status,
              300, 650, 920, 28);
        write(label(Text::store_hints), 48, 686, 1184, 28);
    }
    void draw(double seconds, bool reduced_motion) {
        if (!shown || !target)
            return;
        using namespace art_style;
        auto now = std::chrono::steady_clock::now();
        double dt = last_tick.time_since_epoch().count()
                        ? std::chrono::duration<double>(now - last_tick).count()
                        : 1. / 60;
        last_tick = now;
        elapsed += dt;
        auto page = (setup_mode ? L"setup:" : mods_mode ? L"mods:" : L"home:") + heading;
        if (page != last_page && frame_saved && !reduced_motion) {
            check(transition_frame->CopyFromBitmap(nullptr, previous_frame.Get(), nullptr));
            page_age = 0;
        } else
            page_age += dt;
        last_page = std::move(page);
        disc_focus.step(focus == 0 && flip_time < 0 ? 1 : 0, dt, 10, .65);
        pitch.step(reduced_motion ? 0 : stick_y * 22, dt);
        yaw.step(reduced_motion ? 0 : -stick_x * 22, dt);
        for (int i = 0; i < 2; ++i)
            icon_pop[i].step(!reduced_motion && focus == i + 1 ? .18 : 0, dt, 14, .35);
        if (flip_time >= 0)
            flip_time += dt;
        auto motion = launch_motion(seconds);
        float fade = loading ? float(motion.fade) : 0;
        auto transform = D2D1::Matrix3x2F::Scale(pixel_w / width, pixel_h / height);
        target->BeginDraw();
        target->SetTransform(transform);
        target->Clear(black());
        if (loading && seconds >= 3.2) {
            draw_gold(reduced_motion ? 0 : elapsed, float(ease((seconds - 3.2) / .4)));
            finish_frame();
            return;
        }
        if (store.active) {
            draw_store(dt, reduced_motion);
            finish_frame();
            return;
        }
        auto background = wallpaper == Wallpaper::from_game ? hero.Get()
                          : wallpaper == Wallpaper::plateau ? plateau.Get()
                          : wallpaper == Wallpaper::shrine  ? shrine.Get()
                                                            : nullptr;
        const bool light = background == nullptr;
        auto ink = [&](float alpha = 1) { return light ? black(alpha) : white(alpha); };
        if (background) {
            auto size = background->GetSize();
            float scale = std::max(width / size.width, height / size.height);
            float x = (width - size.width * scale) / 2, y = (height - size.height * scale) / 2;
            target->DrawBitmap(background, D2D1::RectF(x, y, width - x, height - y));
            target->FillRectangle(D2D1::RectF(0, 0, width, height), scrim.Get());
        } else
            target->DrawBitmap(silver_bg.Get(), D2D1::RectF(0, 0, width, height));
        if (setup_mode) {
            brush->SetColor(ink(.92f));
            text(heading, D2D1::RectF(margin, 48, width - margin, 100), true);
            if (diagnostics_mode) {
                auto first = first_row();
                for (size_t i = first; i < rows.size() && i < first + 7; ++i) {
                    float y = 124 + float(i - first) * 70;
                    int kind = i < issue_kinds.size() ? issue_kinds[i] : -1;
                    if (int(i) == focus) {
                        brush->SetColor(white(.75f));
                        target->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(margin, y, width - margin, y + 66), 28, 28),
                            brush.Get());
                    }
                    if (kind >= 0) {
                        // Vector warning triangle; no font or emoji dependency.
                        Ptr<ID2D1PathGeometry> triangle;
                        check(factory->CreatePathGeometry(&triangle));
                        Ptr<ID2D1GeometrySink> sink;
                        check(triangle->Open(&sink));
                        sink->BeginFigure(D2D1::Point2F(margin + 20, y + 8), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(D2D1::Point2F(margin + 9, y + 29));
                        sink->AddLine(D2D1::Point2F(margin + 31, y + 29));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        check(sink->Close());
                        brush->SetColor(art_style::warning());
                        target->FillGeometry(triangle.Get(), brush.Get());
                        brush->SetColor(black());
                        target->DrawLine(D2D1::Point2F(margin + 20, y + 14),
                                         D2D1::Point2F(margin + 20, y + 21), brush.Get(), 2);
                        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(margin + 20, y + 25), 1, 1),
                                            brush.Get());
                        brush->SetColor(kind == 0   ? art_style::missing()
                                        : kind == 5 ? art_style::optional()
                                                    : art_style::warning());
                        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(margin + 43, y + 20), 3, 3),
                                            brush.Get());
                    }
                    brush->SetColor(int(i) == focus ? black(.92f) : ink(.92f));
                    target->DrawText(rows[i].c_str(), UINT32(rows[i].size()), mono_font.Get(),
                                     D2D1::RectF(margin + 56, y + 5, width - margin - 12, y + 37),
                                     brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    if (i < issue_notes.size())
                        text(issue_notes[i], D2D1::RectF(margin + 56, y + 32, width - margin - 12, y + 66));
                }
                brush->SetColor(ink(.8f));
                text(loading_text, D2D1::RectF(margin, 648, width - margin, 704));
                finish_frame();
                return;
            }
            text(detail, D2D1::RectF(margin, 124, qr_modules.empty() ? width - margin : 920, 420));
            if (!qr_modules.empty()) {
                float module = 5, left = 976, top = 136;
                brush->SetColor(white());
                float side = float(qr_modules.size() + 8) * module;
                target->FillRectangle(D2D1::RectF(left, top, left + side, top + side), brush.Get());
                brush->SetColor(black());
                for (size_t y = 0; y < qr_modules.size(); ++y)
                    for (size_t x = 0; x < qr_modules[y].size(); ++x)
                        if (qr_modules[y][x]) {
                            float px = left + float(x + 4) * module, py = top + float(y + 4) * module;
                            target->FillRectangle(D2D1::RectF(px, py, px + module, py + module), brush.Get());
                        }
            }
            if (progress >= 0) {
                brush->SetColor(white(.12f));
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(margin, 396, width - margin, 404), 4, 4), brush.Get());
                brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b));
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(margin, 396,
                                    margin + (width - 2 * margin) * std::clamp(progress, 0.f, 1.f), 404),
                        4, 4),
                    brush.Get());
            }
            auto first = first_row();
            for (size_t i = first; i < rows.size() && i < first + 3; ++i) {
                float top = 440 + float(i - first) * 64;
                bool focused = int(i) == focus && enabled(i);
                if (focused || light) {
                    brush->SetColor(light ? (focused ? black(.92f) : white(.94f)) : white());
                    target->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(margin, top, width - margin, top + button_h),
                                          button_h / 2, button_h / 2),
                        brush.Get());
                }
                brush->SetColor(light ? (focused ? white() : black(enabled(i) ? .92f : .4f))
                                      : (focused ? black() : white(enabled(i) ? 1.f : .4f)));
                text(rows[i], D2D1::RectF(margin + 24, top + 17, width - margin - 24, top + button_h));
            }
            brush->SetColor(light ? black(.72f) : white(.72f));
            text(loading_text, D2D1::RectF(margin, 648, width - margin, 704));
            finish_frame();
            return;
        }
        if (mods_mode) {
            brush->SetColor(ink(.92f));
            text(heading, D2D1::RectF(margin, 48, width - margin, 96), true);
            text(detail, D2D1::RectF(margin, 104, width - margin, 152));
            auto first = first_row();
            for (size_t i = first; i < rows.size() && i < first + 7; ++i) {
                float top = 160 + (i - first) * 64;
                auto rect = D2D1::RectF(margin, top, width - margin, top + button_h);
                bool focused = static_cast<int>(i) == focus && enabled(i);
                if (focused) {
                    brush->SetColor(black(.92f));
                    target->FillRoundedRectangle(D2D1::RoundedRect(rect, button_h / 2, button_h / 2),
                                                 brush.Get());
                }
                brush->SetColor(focused ? white() : ink(enabled(i) ? .92f : .4f));
                text(rows[i], D2D1::RectF(margin + 24, top + 17, width - margin - 24, top + button_h));
            }
            brush->SetColor(ink(.72f));
            text(loading_text, D2D1::RectF(margin, 632, width - margin, 700));
            finish_frame();
            return;
        }
        const auto center = D2D1::Point2F(640, 360);
        // Record the entire face (including its hole, light and focus ring) in a plane.
        Ptr<ID2D1CommandList> plane;
        check(target->CreateCommandList(&plane));
        target->SetTarget(plane.Get());
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        bool back = flip_time >= 1;
        brush->SetColor(black(.98f));
        target->FillGeometry(disc_mask.Get(), brush.Get());
        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), disc_mask.Get()), disc_layer.Get());
        if (back) {
            target->DrawBitmap(silver.Get(), D2D1::RectF(490, 210, 790, 510));
        } else if (disc && !missing_game) {
            target->SetTransform(D2D1::Matrix3x2F::Rotation(
                reduced_motion ? 0 : float(std::fmod(loading ? motion.angle : 0, 360.)), center));
            auto size = disc->GetSize();
            float side = std::min(size.width, size.height);
            target->DrawBitmap(disc.Get(), D2D1::RectF(490, 210, 790, 510), 1,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                               D2D1::RectF((size.width - side) / 2, (size.height - side) / 2,
                                           (size.width + side) / 2, (size.height + side) / 2));
        } else {
            brush->SetColor(white(.085f));
            for (int r = 60; r <= 140; r += 14)
                target->DrawEllipse(D2D1::Ellipse(center, float(r), float(r)), brush.Get(), .7f);
            brush->SetColor(white(.9f));
            target->DrawGeometry(question.Get(), brush.Get(), 15);
        }
        target->SetTransform(D2D1::Matrix3x2F::Rotation(float(yaw.value * 2.5 - pitch.value * 1.5), center));
        target->DrawBitmap(sheen.Get(), D2D1::RectF(490, 210, 790, 510));
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->PopLayer();
        brush->SetColor(white(.18f));
        target->DrawEllipse(D2D1::Ellipse(center, 149, 149), brush.Get(), 1);
        target->DrawEllipse(D2D1::Ellipse(center, 24, 24), brush.Get(), 1);
        if (disc_focus.value > .001 && flip_time < 0) {
            float alpha = reduced_motion ? .55f : float(.55 + .45 * std::sin(elapsed * 2 * pi / 2.4));
            alpha *= float(std::clamp(disc_focus.value, 0., 1.));
            target->DrawBitmap(halo.Get(), D2D1::RectF(460, 180, 820, 540), alpha);
            brush->SetColor(white(alpha));
            target->DrawEllipse(D2D1::Ellipse(center, 155, 155), brush.Get(), 1.5f);
        }
        target->SetTarget(surface_bitmap.Get());
        check(plane->Close());
        target->SetTransform(transform);
        perspective->SetInput(0, plane.Get());
        check(perspective->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_DEPTH, 1400.f));
        check(perspective->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_PERSPECTIVE_ORIGIN,
                                    D2D1::Vector2F(640, 360)));
        check(perspective->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_ROTATION_ORIGIN,
                                    D2D1::Vector3F(640, 360, 0)));
        float flip = flip_time < 0 ? 0 : float(180 * ease(flip_time / 2));
        // Mirror the back face by rotating it back into view after the edge-on midpoint.
        check(perspective->SetValue(
            D2D1_3DPERSPECTIVETRANSFORM_PROP_ROTATION,
            D2D1::Vector3F(float(std::clamp(pitch.value, -22., 22.)),
                           float(std::clamp(yaw.value, -22., 22.)) + flip - (back ? 180 : 0), 0)));
        float zoom = flip_time < 0 ? 1 : float(1 + 5 * ease((flip_time - .8) / 2.7));
        target->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom, center) * transform);
        check(perspective->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_LOCAL_OFFSET,
                                    D2D1::Vector3F(0, 0, loading ? float(motion.z) : 0)));
        target->DrawImage(perspective.Get());
        target->SetTransform(transform);
        if (flip_time > 2.7) {
            target->DrawBitmap(silver_bg.Get(), D2D1::RectF(0, 0, width, height),
                               float(ease((flip_time - 2.7) / .8)));
        }
        // Accessibility names: disc = Play, gear = Settings, puzzle piece = Mods.
        float icon_alpha = missing_game    ? 0.f // first run shows only the "?" disc
                           : loading         ? float(1 - ease(seconds / .22))
                           : flip_time < 0 ? 1
                                           : float(1 - ease(flip_time / .4));
        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                D2D1::Matrix3x2F::Identity(), icon_alpha),
                          nullptr);
        for (int i = 1; i <= 2; ++i) {
            // Top-left: the hero art's logo fills the top-right corner.
            auto c = D2D1::Point2F(56, 56 + (i - 1) * 64.f);
            target->SetTransform(D2D1::Matrix3x2F::Scale(float(1 + icon_pop[i - 1].value),
                                                         float(1 + icon_pop[i - 1].value), c) *
                                 transform);
            // Color follows the same spring as the icon, so focus never snaps.
            float mix = reduced_motion ? (focus == i ? 1.f : 0.f)
                                       : float(std::clamp(icon_pop[i - 1].value / .18, 0., 1.));
            float tone = light ? 1 - mix : mix;
            auto background = D2D1::ColorF(tone, tone, tone, .9f + .1f * mix);
            auto foreground = D2D1::ColorF(1 - tone, 1 - tone, 1 - tone);
            brush->SetColor(background);
            target->FillEllipse(D2D1::Ellipse(c, 24, 24), brush.Get());
            brush->SetColor(foreground);
            if (i == 1) { // solid gear with a hole
                Ptr<ID2D1PathGeometry> gear;
                Ptr<ID2D1GeometrySink> sink;
                check(factory->CreatePathGeometry(&gear));
                check(gear->Open(&sink));
                auto points = gear_outline();
                sink->BeginFigure(D2D1::Point2F(c.x + points[0].first, c.y + points[0].second),
                                  D2D1_FIGURE_BEGIN_FILLED);
                for (size_t p = 1; p < points.size(); ++p)
                    sink->AddLine(D2D1::Point2F(c.x + points[p].first, c.y + points[p].second));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                check(sink->Close());
                target->FillGeometry(gear.Get(), brush.Get());
                brush->SetColor(background);
                target->FillEllipse(D2D1::Ellipse(c, 4.5f, 4.5f), brush.Get());
            } else { // puzzle piece: body plus a knob on top and on the right
                target->FillRectangle(D2D1::RectF(c.x - 10, c.y - 6, c.x + 6, c.y + 10), brush.Get());
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 2, c.y - 9), 4.5f, 4.5f), brush.Get());
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + 9, c.y + 2), 4.5f, 4.5f), brush.Get());
                brush->SetColor(background); // sockets on the left and bottom
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 10, c.y + 2), 3.5f, 3.5f), brush.Get());
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 2, c.y + 10), 3.5f, 3.5f), brush.Get());
            }
        }
        target->SetTransform(transform);
        target->PopLayer();
        float label = float(1 - ease(std::max(0., flip_time) / 2));
        // Home shows no title; a faint credit sits in the bottom-right safe area.
        brush->SetColor(ink((light ? .6f : .72f) * label));
        target->DrawText(L"nspxmiguel", 10, credit_font.Get(),
                         D2D1::RectF(width - margin - 240, 680, width - margin, 700), brush.Get());
        brush->SetColor(ink(.92f * label));
        // First run shows only the "?" disc; failure messages may still accompany the title.
        if (!loading && !missing_game)
            text(detail, D2D1::RectF(margin, 674, 1150, 720));
        if (loading) {
            brush->SetColor(black(fade));
            target->FillRectangle(D2D1::RectF(0, 0, width, height), brush.Get());
            draw_gold(elapsed, float(ease((seconds - 3.2) / .4)));
        }
        finish_frame();
    }
};
} // namespace uwp
