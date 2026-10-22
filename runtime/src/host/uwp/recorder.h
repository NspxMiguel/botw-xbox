// SPDX-License-Identifier: MIT
#pragma once
#include "../../log.h"
#include "debug_hud.h"
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <wincodec.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Storage.Streams.h>
#ifdef _MSC_VER
#pragma comment(lib, "windowscodecs.lib")
#endif

namespace uwp {
namespace capture_detail {
inline constexpr UINT video_width = 1280, video_height = 720, video_stride = video_width * 4;
inline int64_t qpc() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
inline int64_t qpc_frequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}

struct VideoFrame {
    uint64_t number;
    double milliseconds;
    std::vector<BYTE> pixels;
};

struct Recorder {
    std::atomic<bool> initialized{false}, stopping{false}, enabled{false};
    std::atomic<int64_t> deadline{0};
    std::mutex mutex, thread_mutex;
    std::condition_variable wake;
    std::deque<VideoFrame> frames;
    std::thread worker;
    uint64_t attempted = 0; // Protected by mutex, including the final summary.
    uint32_t seconds = 0;
    int64_t frequency = qpc_frequency();
    int64_t next_capture = 0;
    uint64_t sequence = 0;

    void stop() {
        if (initialized) {
            stopping = true;
            wake.notify_all();
        }
    }
    void join() {
        std::lock_guard lock(thread_mutex);
        if (worker.joinable())
            worker.join();
    }
    ~Recorder() {
        stop();
        join();
    }

    // Appends one JPEG to the open MJPEG file: one Portal download instead of thousands.
    static void encode(IWICImagingFactory *wic, std::ofstream &video, VideoFrame &image) {
        ComPtr<IStream> stream;
        check(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
        ComPtr<IWICBitmapEncoder> encoder;
        check(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        check(encoder->CreateNewFrame(&frame, &properties));
        PROPBAG2 option{};
        option.pstrName = const_cast<wchar_t *>(L"ImageQuality");
        VARIANT quality{};
        quality.vt = VT_R4;
        quality.fltVal = .85f;
        check(properties->Write(1, &option, &quality));
        check(frame->Initialize(properties.Get()));
        check(frame->SetSize(video_width, video_height));
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        check(frame->SetPixelFormat(&format));
        ComPtr<IWICBitmap> bitmap;
        check(wic->CreateBitmapFromMemory(video_width, video_height, GUID_WICPixelFormat32bppBGRA,
                                          video_stride, UINT(image.pixels.size()), image.pixels.data(),
                                          &bitmap));
        ComPtr<IWICFormatConverter> converter;
        check(wic->CreateFormatConverter(&converter));
        check(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom));
        check(frame->WriteSource(converter.Get(), nullptr));
        check(frame->Commit());
        check(encoder->Commit());
        STATSTG stat{};
        check(stream->Stat(&stat, STATFLAG_NONAME));
        std::vector<char> bytes(size_t(stat.cbSize.QuadPart));
        check(stream->Seek({}, STREAM_SEEK_SET, nullptr));
        ULONG read = 0;
        check(stream->Read(bytes.data(), ULONG(bytes.size()), &read));
        video.write(bytes.data(), read);
    }

    void run(std::filesystem::path directory) noexcept {
        uint64_t written = 0;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct ApartmentGuard {
                ~ApartmentGuard() { winrt::uninit_apartment(); }
            } apartment;
            std::filesystem::create_directories(directory);
            std::ofstream index(directory / L"index.txt", std::ios::trunc);
            index.exceptions(std::ios::badbit | std::ios::failbit);
            index << std::fixed << std::setprecision(3);
            std::ofstream video(directory / L"video.mjpeg", std::ios::binary | std::ios::trunc);
            video.exceptions(std::ios::badbit | std::ios::failbit);
            ComPtr<IWICImagingFactory> wic;
            check(
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
            for (;;) {
                VideoFrame image;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(50),
                                  [&] { return stopping.load() || !frames.empty(); });
                    auto end = deadline.load();
                    // Allow the renderer a short, bounded tail to collect the last GPU copies.
                    // Also finishes when presentation stops entirely (for example, a paused menu).
                    if (end && qpc() >= end + frequency / 4)
                        stopping = true;
                    if (frames.empty()) {
                        if (stopping)
                            break;
                        continue;
                    }
                    image = std::move(frames.front());
                    frames.pop_front();
                }
                encode(wic.Get(), video, image);
                index << image.number << ' ' << image.milliseconds << '\n';
                ++written;
            }
            index.close();
            video.close();
        } catch (...) {
            LOG("recorder: failed; disabled for this session");
        }
        uint64_t dropped;
        {
            std::lock_guard lock(mutex);
            stopping = true;
            enabled = false;
            dropped = attempted - written;
            frames.clear();
        }
        LOG("recorder: %llu frames, %llu dropped", static_cast<unsigned long long>(written),
            static_cast<unsigned long long>(dropped));
    }

    // Called once on the launch worker, before the game starts. No file I/O on the renderer.
    void start() {
        if (initialized.exchange(true))
            return;
        using namespace winrt::Windows::Storage;
        auto local = ApplicationData::Current().LocalFolder();
        auto item = local.TryGetItemAsync(L"record.txt").get();
        if (!item || stopping)
            return;
        auto file = item.as<StorageFile>();
        if (file.GetBasicPropertiesAsync().get().Size() > 64)
            throw std::runtime_error("record.txt exceeds 64 bytes");
        auto text = winrt::to_string(FileIO::ReadTextAsync(file).get());
        auto first = text.find_first_not_of(" \t\r\n");
        auto last = text.find_last_not_of(" \t\r\n");
        if (first == std::string::npos)
            throw std::runtime_error("record.txt must contain seconds");
        auto result = std::from_chars(text.data() + first, text.data() + last + 1, seconds);
        if (result.ec != std::errc{} || result.ptr != text.data() + last + 1 || !seconds)
            throw std::runtime_error("record.txt must contain a positive integer");
        file.RenameAsync(L"record.started.txt", NameCollisionOption::ReplaceExisting).get();
        std::filesystem::path directory = std::wstring(local.Path()) + L"\\rec";
        std::lock_guard lock(thread_mutex);
        if (stopping)
            return;
        enabled = true;
        worker = std::thread([this, directory] { run(directory); });
    }
};
inline Recorder recorder;

// Three GPU staging slots decouple capture from GPU completion. Mapping never waits for the GPU.
struct Readbacks {
    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
        bool pending = false;
        uint64_t number = 0;
        double milliseconds = 0;
    };
    ComPtr<ID3D11Texture2D> source, scaled;
    ComPtr<ID2D1Bitmap1> source_bitmap, small_bitmap;
    std::array<Slot, 3> slots;
    uint64_t generation = 0;

    explicit Readbacks(Graphics &g) : generation(graphics_generation) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = g.desc.Width;
        desc.Height = g.desc.Height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = g.desc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(g.device->CreateTexture2D(&desc, nullptr, &source));
        ComPtr<IDXGISurface> surface;
        check(source.As(&surface));
        check(g.canvas->CreateBitmapFromDxgiSurface(
            surface.Get(),
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                    D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE)),
            &source_bitmap));
        desc.Width = video_width;
        desc.Height = video_height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        check(g.device->CreateTexture2D(&desc, nullptr, &scaled));
        surface.Reset();
        check(scaled.As(&surface));
        check(g.canvas->CreateBitmapFromDxgiSurface(
            surface.Get(),
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                    D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE)),
            &small_bitmap));
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto &slot : slots)
            check(g.device->CreateTexture2D(&desc, nullptr, &slot.texture));
    }

    void collect(Graphics &g) {
        for (;;) {
            Slot *oldest = nullptr;
            for (auto &candidate : slots)
                if (candidate.pending && (!oldest || candidate.number < oldest->number))
                    oldest = &candidate;
            if (!oldest)
                return;
            auto &slot = *oldest;
            // A busy encoder must never hold up presentation, even through mutex contention.
            std::unique_lock lock(recorder.mutex, std::try_to_lock);
            if (!lock || recorder.frames.size() >= 8 || recorder.stopping)
                return;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            auto result =
                g.context->Map(slot.texture.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (result == DXGI_ERROR_WAS_STILL_DRAWING)
                return;
            check(result);
            struct Unmap {
                ID3D11DeviceContext *context;
                ID3D11Resource *resource;
                ~Unmap() { context->Unmap(resource, 0); }
            } unmap{g.context.Get(), slot.texture.Get()};
            VideoFrame frame{slot.number, slot.milliseconds, std::vector<BYTE>(video_stride * video_height)};
            for (UINT y = 0; y < video_height; ++y)
                std::memcpy(frame.pixels.data() + y * video_stride,
                            static_cast<BYTE const *>(mapped.pData) + y * mapped.RowPitch, video_stride);
            recorder.frames.push_back(std::move(frame));
            slot.pending = false;
            recorder.wake.notify_one();
        }
    }
};
inline std::unique_ptr<Readbacks> readbacks;
} // namespace capture_detail

inline void stop_recording() { capture_detail::recorder.stop(); }
inline void wait_for_recording() { capture_detail::recorder.join(); }

// Called by the swap chain before releasing its own buffer references, including ResizeBuffers.
inline void release_capture_graphics(void *chain) {
    using namespace capture_detail;
    if (graphics && graphics->chain.Get() == static_cast<IDXGISwapChain1 *>(chain)) {
        readbacks.reset();
        graphics.reset();
    }
}

inline void record_frame(void *device, void *queue, void *chain) {
    using namespace capture_detail;
    auto &r = recorder;
    if (!r.enabled || r.stopping) {
        readbacks.reset();
        return;
    }
    auto now = qpc();
    auto end = r.deadline.load();
    if (end && now >= end) {
        if (readbacks && graphics && readbacks->generation == graphics_generation) {
            readbacks->collect(*graphics);
            bool pending = std::any_of(readbacks->slots.begin(), readbacks->slots.end(),
                                       [](auto const &slot) { return slot.pending; });
            if (pending)
                return;
        }
        r.stop();
        return;
    }
    if (quick_menu.requested)
        return;
    auto &g = get_graphics(device, queue, chain);
    if (!readbacks || readbacks->generation != graphics_generation)
        readbacks = std::make_unique<Readbacks>(g);
    readbacks->collect(g);
    now = qpc();
    if (now < r.next_capture)
        return;
    if (!end)
        r.deadline = now + int64_t(double(r.frequency) * r.seconds);
    // Wall-clock pacing records every other present at 60 Hz and every present at 30 Hz.
    // Missed intervals are skipped; never burst or duplicate a frame to catch up.
    auto period = std::max<int64_t>(1, r.frequency / 30);
    if (!r.next_capture)
        r.next_capture = now;
    r.next_capture += ((now - r.next_capture) / period + 1) * period;
    {
        std::unique_lock lock(r.mutex, std::try_to_lock);
        if (!lock || r.stopping)
            return;
        ++r.attempted;
        if (r.frames.size() >= 8)
            return;
    }
    Readbacks::Slot *slot = nullptr;
    for (auto &candidate : readbacks->slots)
        if (!candidate.pending) {
            slot = &candidate;
            break;
        }
    if (!slot)
        return;
    auto index = g.chain->GetCurrentBackBufferIndex();
    WrappedGuard acquired(g, index);
    g.context->CopyResource(readbacks->source.Get(), acquired.resource);
    g.canvas->SetTarget(readbacks->small_bitmap.Get());
    g.canvas->SetTransform(D2D1::Matrix3x2F::Identity());
    g.canvas->BeginDraw();
    g.canvas->DrawBitmap(readbacks->source_bitmap.Get(), D2D1::RectF(0, 0, video_width, video_height), 1,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    check(g.canvas->EndDraw());
    g.canvas->SetTarget(nullptr);
    g.context->CopyResource(slot->texture.Get(), readbacks->scaled.Get());
    slot->number = r.sequence++;
    slot->milliseconds = double(now) * 1000 / r.frequency;
    slot->pending = true;
}
} // namespace uwp
