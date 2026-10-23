// SPDX-License-Identifier: MIT
#pragma once
#include "../../log.h"
#include "debug_hud.h"
#include "recording_audio.h"
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <d3d10.h>
#include <deque>
#include <filesystem>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <winrt/Windows.Storage.FileProperties.h>
#include <wrl/implements.h>
#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#endif

namespace uwp {
namespace capture_detail {
inline constexpr UINT video_width = 1920, video_height = 1080, video_stride = video_width * 4;
inline constexpr size_t video_queue_size = 8;
struct VideoTexture {
    ComPtr<ID3D11Texture2D> texture;
};
struct VideoFrame {
    int64_t time = 0;
    uint32_t fps = 60;
    std::shared_ptr<VideoTexture> surface;
    std::vector<BYTE> pixels;
};
// The encoder can retain a sample after WriteSample returns. Do not recycle its texture until
// IMFTrackedSample reports that every consumer has released it, even across a swap-chain resize.
struct TextureRelease final
    : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                   IMFAsyncCallback> {
    std::shared_ptr<VideoTexture> surface;
    explicit TextureRelease(std::shared_ptr<VideoTexture> value) : surface(std::move(value)) {}
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD *, DWORD *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult *) override {
        surface.reset();
        return S_OK;
    }
};

struct Sink {
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<ID3D11Device> video_device;
    UINT reset_token = 0;
    DWORD video = 0, audio = 0;
    bool begun = false;

    static ComPtr<IMFMediaType> video_type(bool encoded) {
        ComPtr<IMFMediaType> type;
        check(MFCreateMediaType(&type));
        check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        check(type->SetGUID(MF_MT_SUBTYPE, encoded ? MFVideoFormat_H264 : MFVideoFormat_RGB32));
        check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, video_width, video_height));
        check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 60, 1));
        check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        if (encoded)
            check(type->SetUINT32(MF_MT_AVG_BITRATE, 16000000));
        else {
            // BGRA staging rows and DXGI surfaces both have a top-down, positive stride.
            check(type->SetUINT32(MF_MT_DEFAULT_STRIDE, video_stride));
            check(type->SetUINT32(MF_MT_SAMPLE_SIZE, video_stride * video_height));
            check(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
        }
        return type;
    }
    static ComPtr<IMFMediaType> audio_type(bool encoded) {
        ComPtr<IMFMediaType> type;
        check(MFCreateMediaType(&type));
        check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        check(type->SetGUID(MF_MT_SUBTYPE, encoded ? MFAudioFormat_AAC : MFAudioFormat_PCM));
        check(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
        check(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio_rate));
        check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        check(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, encoded ? 20000 : audio_rate * 4));
        check(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, encoded ? 1 : 4));
        if (encoded)
            check(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0));
        return type;
    }
    void open(const std::filesystem::path &path, ID3D11Device *device) {
        ComPtr<IMFAttributes> attributes;
        check(MFCreateAttributes(&attributes, 3));
        check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
        // Only the worker calls WriteSample; retain MF's throttle to bound encoder buffering.
        if (device) {
            check(MFCreateDXGIDeviceManager(&reset_token, &manager));
            check(manager->ResetDevice(device, reset_token));
            video_device = device;
            check(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager.Get()));
        }
        check(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer));
        auto video_output = video_type(true), video_input = video_type(false);
        check(writer->AddStream(video_output.Get(), &video));
        check(writer->SetInputMediaType(video, video_input.Get(), nullptr));
        auto audio_output = audio_type(true), audio_input = audio_type(false);
        check(writer->AddStream(audio_output.Get(), &audio));
        check(writer->SetInputMediaType(audio, audio_input.Get(), nullptr));
        check(writer->BeginWriting());
        IMFActivate **found = nullptr;
        UINT32 hardware = 0;
        MFT_REGISTER_TYPE_INFO h264{MFMediaType_Video, MFVideoFormat_H264};
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                nullptr, &h264, &found, &hardware))) {
            for (UINT32 i = 0; i < hardware; ++i)
                found[i]->Release();
            CoTaskMemFree(found);
        }
        LOG("recorder: %u hardware H.264 encoders", hardware);
        // Log which encoder MF picked: a software MFT cannot sustain 1080p60.
        ComPtr<IMFSinkWriterEx> ex;
        ComPtr<IMFTransform> transform;
        GUID category{};
        for (DWORD i = 0; SUCCEEDED(writer.As(&ex)) && SUCCEEDED(ex->GetTransformForStream(video, i, &category, &transform)); ++i) {
            ComPtr<IMFAttributes> info;
            wchar_t name[128] = L"?";
            if (SUCCEEDED(transform->GetAttributes(&info))) {
                info->GetString(MFT_FRIENDLY_NAME_Attribute, name, 128, nullptr);
            }
            LOG("recorder: video transform %lu %ls%s", static_cast<unsigned long>(i), name,
                category == MFT_CATEGORY_VIDEO_ENCODER ? " (encoder)" : "");
            transform.Reset();
        }
        begun = true;
    }
    void finish() noexcept {
        if (begun) {
            begun = false;
            auto hr = writer->Finalize();
            if (FAILED(hr))
                LOG("recorder: Finalize failed (0x%08lx)", static_cast<unsigned long>(hr));
        }
    }
    void reset() noexcept {
        finish();
        writer.Reset();
        manager.Reset();
        video_device.Reset();
    }
    ~Sink() { finish(); }

    static ComPtr<IMFMediaBuffer> memory_buffer(const void *data, DWORD bytes) {
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateMemoryBuffer(bytes, &buffer));
        BYTE *target = nullptr;
        check(buffer->Lock(&target, nullptr, nullptr));
        std::memcpy(target, data, bytes);
        check(buffer->Unlock());
        check(buffer->SetCurrentLength(bytes));
        return buffer;
    }
    void write_video(const VideoFrame &frame, int64_t time, int64_t duration) {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        if (frame.surface) {
            ComPtr<ID3D11Device> device;
            frame.surface->texture->GetDevice(&device);
            if (device.Get() != video_device.Get()) {
                check(manager->ResetDevice(device.Get(), reset_token));
                video_device = device;
            }
            ComPtr<IMFTrackedSample> tracked;
            check(MFCreateTrackedSample(&tracked));
            check(tracked.As(&sample));
            check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), frame.surface->texture.Get(), 0, FALSE,
                                            &buffer));
            check(buffer->SetCurrentLength(video_stride * video_height));
            check(sample->AddBuffer(buffer.Get()));
            auto release = Microsoft::WRL::Make<TextureRelease>(frame.surface);
            if (!release)
                throw std::bad_alloc();
            check(tracked->SetAllocator(release.Get(), nullptr));
        } else {
            check(MFCreateSample(&sample));
            buffer = memory_buffer(frame.pixels.data(), DWORD(frame.pixels.size()));
            check(sample->AddBuffer(buffer.Get()));
        }
        check(sample->SetSampleTime(time));
        check(sample->SetSampleDuration(duration));
        check(writer->WriteSample(video, sample.Get()));
    }
    void write_audio(const AudioChunk &chunk, uint32_t first, uint32_t count, int64_t time) {
        auto buffer = memory_buffer(chunk.samples.data() + first * 2, count * 4);
        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample));
        check(sample->AddBuffer(buffer.Get()));
        check(sample->SetSampleTime(time));
        check(sample->SetSampleDuration(int64_t(count) * media_second / audio_rate));
        check(writer->WriteSample(audio, sample.Get()));
    }
};

struct Recorder {
    std::atomic<bool> initialized{false}, stopping{false}, enabled{false}, cpu{false}, presenting{false};
    std::atomic<int64_t> epoch{0}, deadline{0};
    std::atomic<uint64_t> attempted{0}, no_surface{0}, queue_full{0};
    std::atomic<uint32_t> fps{60};
    std::mutex mutex, thread_mutex;
    std::condition_variable wake;
    std::deque<VideoFrame> frames;
    std::thread worker;
    uint32_t seconds = 0;
    const int64_t frequency = qpc_frequency();
    // Renderer-thread only, including the CPU throughput window.
    int64_t next_capture = 0, window_start = 0, readback_ticks = 0;
    uint32_t window_attempts = 0, window_drops = 0;

    int64_t media_time(int64_t ticks) const {
        // Split the conversion to avoid overflowing QPC * 10,000,000 on long recordings.
        return ticks / frequency * media_second + ticks % frequency * media_second / frequency;
    }
    void stop() {
        stopping = true;
        recording_audio.epoch = 0;
        wake.notify_all();
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
    bool enqueue(VideoFrame frame) {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock || frames.size() >= video_queue_size || stopping) {
            ++queue_full;
            return false;
        }
        frames.push_back(std::move(frame));
        wake.notify_one();
        return true;
    }
    void cpu_drop() {
        if (cpu)
            ++window_drops;
    }
    void pace_cpu(int64_t now) {
        if (!cpu || fps == 30)
            return;
        if (!window_start)
            window_start = now;
        if (now - window_start < frequency)
            return;
        // Sustained queue pressure or a readback taking a frame budget cannot sustain 60 Hz.
        if (window_attempts >= 30 &&
            (window_drops >= 4 || readback_ticks > int64_t(window_attempts) * frequency / 60)) {
            fps = 30;
            wake.notify_one();
        }
        window_start = now;
        window_attempts = window_drops = 0;
        readback_ticks = 0;
    }

    void run(std::filesystem::path directory) noexcept {
        uint64_t written = 0, audio_frames = 0;
        int64_t write_ticks = 0;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct ApartmentGuard {
                ~ApartmentGuard() { winrt::uninit_apartment(); }
            } apartment;
            check(MFStartup(MF_VERSION));
            struct FoundationGuard {
                ~FoundationGuard() { MFShutdown(); }
            } foundation;
            std::filesystem::create_directories(directory);
            auto path = directory / L"video.mp4";
            Sink sink;
            struct CaptureGuard {
                Recorder &owner;
                ~CaptureGuard() { owner.stop(); }
            } capture{*this};
            int64_t audio_end = 0;
            bool logged_30 = false;
            for (;;) {
                VideoFrame image;
                bool have_video = false;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(5), [&] {
                        return !frames.empty() ||
                               ((sink.writer || stopping) &&
                                recording_audio.head.load(std::memory_order_acquire) !=
                                    recording_audio.tail.load()) ||
                               (stopping && !presenting && !recording_audio.producing);
                    });
                    auto end = deadline.load();
                    // Bound the tail even when no further presents arrive to collect staging copies.
                    if (end && qpc() >= end + frequency / 4)
                        stop();
                    auto tail = recording_audio.tail.load(std::memory_order_relaxed);
                    bool have_audio = tail != recording_audio.head.load(std::memory_order_acquire);
                    // Keep both streams moving in QPC order, without copying audio under this lock.
                    if (!frames.empty() &&
                        (!have_audio || !sink.writer ||
                         frames.front().time <=
                             recording_audio.chunks[tail % recording_audio.chunks.size()].time)) {
                        image = std::move(frames.front());
                        frames.pop_front();
                        have_video = true;
                    } else if (stopping && frames.empty() && !have_audio && !presenting &&
                               !recording_audio.producing)
                        break;
                }
                if (!logged_30 && fps == 30) {
                    LOG("recorder: CPU readback cannot sustain 60 fps; capturing at 30 fps");
                    logged_30 = true;
                }
                if (have_video) {
                    // Queued GPU frames from a rejected path are drops, never CPU-mapped here.
                    if (cpu && image.surface)
                        continue;
                    if (!sink.writer) {
                        try {
                            ComPtr<ID3D11Device> device;
                            if (image.surface)
                                image.surface->texture->GetDevice(&device);
                            sink.open(path, device.Get());
                            LOG("recorder: 1920x1080 60 fps H.264, %s input, AAC 48000 Hz stereo 160 kbps",
                                image.surface ? "GPU" : "CPU");
                        } catch (const winrt::hresult_error &error) {
                            if (!image.surface)
                                throw;
                            LOG("recorder: GPU setup failed (0x%08lx); falling back to CPU readback",
                                static_cast<unsigned long>(error.code().value));
                            sink.reset();
                            cpu = true;
                            continue;
                        }
                    }
                    auto time = media_time(image.time - epoch.load());
                    auto duration =
                        std::min<int64_t>(media_second / image.fps, int64_t(seconds) * media_second - time);
                    if (time < 0 || duration <= 0)
                        continue;
                    try {
                        auto started = qpc();
                        sink.write_video(image, time, duration);
                        write_ticks += qpc() - started;
                        if (++written % 600 == 0) {
                            LOG("recorder: %llu written, WriteSample avg %.2f ms, no surface %llu, queue full %llu",
                                static_cast<unsigned long long>(written), write_ticks * 1000.0 / frequency / 600,
                                static_cast<unsigned long long>(no_surface.load()),
                                static_cast<unsigned long long>(queue_full.load()));
                            write_ticks = 0;
                        }
                    } catch (const winrt::hresult_error &error) {
                        if (!image.surface)
                            throw;
                        LOG("recorder: GPU sample failed (0x%08lx); falling back to CPU readback",
                            static_cast<unsigned long>(error.code().value));
                        cpu = true;
                        if (!written && !audio_frames) {
                            // A first-sample rejection can also require a writer without a D3D manager.
                            sink.reset();
                        }
                        // Both paths use RGB32. Subsequent frames supply memory buffers to the
                        // existing converter without renegotiating an active stream or losing MP4 data.
                    }
                } else if (sink.writer) {
                    auto &ring = recording_audio;
                    auto tail = ring.tail.load(std::memory_order_relaxed);
                    if (tail == ring.head.load(std::memory_order_acquire))
                        continue;
                    const auto &chunk = ring.chunks[tail % ring.chunks.size()];
                    auto time = media_time(chunk.time - epoch.load());
                    uint32_t first = 0;
                    // Trim samples crossing the first video frame or the N-second endpoint.
                    if (time < 0)
                        first = uint32_t(std::min<int64_t>(
                            chunk.frames, (-time * audio_rate + media_second - 1) / media_second));
                    // QPC anchors each block; preserve all PCM when callbacks arrive in a burst.
                    time = std::max(audio_end, time + int64_t(first) * media_second / audio_rate);
                    auto remaining = int64_t(seconds) * media_second - time;
                    uint32_t count = remaining > 0
                                         ? uint32_t(std::min<int64_t>(chunk.frames - first,
                                                                      remaining * audio_rate / media_second))
                                         : 0;
                    if (count) {
                        sink.write_audio(chunk, first, count, time);
                        audio_frames += count;
                        audio_end = time + int64_t(count) * media_second / audio_rate;
                    }
                    ring.tail.store(tail + 1, std::memory_order_release);
                } else if (stopping) {
                    // No usable video reached MF (for example suspend during encoder setup).
                    recording_audio.tail.store(recording_audio.head.load(std::memory_order_acquire),
                                               std::memory_order_release);
                }
            }
            sink.finish();
        } catch (const winrt::hresult_error &error) {
            LOG("recorder: failed (0x%08lx); disabled for this session",
                static_cast<unsigned long>(error.code().value));
        } catch (...) {
            LOG("recorder: failed; disabled for this session");
        }
        stop();
        enabled = false;
        {
            std::lock_guard lock(mutex);
            frames.clear();
        }
        LOG("recorder: %llu video frames, %llu dropped, %llu audio ms",
            static_cast<unsigned long long>(written),
            static_cast<unsigned long long>(attempted.load() - written),
            static_cast<unsigned long long>(audio_frames * 1000 / audio_rate));
        if (auto dropped = recording_audio.dropped_frames.load())
            LOG("recorder: audio ring dropped %llu ms",
                static_cast<unsigned long long>(dropped * 1000 / audio_rate));
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
        try {
            worker = std::thread([this, directory] { run(directory); });
        } catch (...) {
            enabled = false;
            throw;
        }
    }
};
inline Recorder recorder;

// Renderer-owned copies. CPU staging is allocated only if the GPU encoder rejects DXGI input.
struct Readbacks {
    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
        int64_t time = 0;
        bool pending = false;
        uint32_t fps = 60;
    };
    ComPtr<ID3D11Texture2D> source, scaled;
    ComPtr<ID2D1Bitmap1> source_bitmap, scaled_bitmap;
    std::array<std::shared_ptr<VideoTexture>, 12> surfaces;
    std::array<Slot, 3> slots;
    D3D11_TEXTURE2D_DESC output{};
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
        output = desc;
        check(g.device->CreateTexture2D(&desc, nullptr, &scaled));
        surface.Reset();
        check(scaled.As(&surface));
        check(g.canvas->CreateBitmapFromDxgiSurface(
            surface.Get(),
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                    D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE)),
            &scaled_bitmap));
        // MF's D3D manager and the overlay share this immediate context.
        ComPtr<ID3D10Multithread> multithread;
        auto hr = g.device.As(&multithread);
        if (SUCCEEDED(hr))
            multithread->SetMultithreadProtected(TRUE);
        else {
            recorder.cpu = true;
            LOG("recorder: D3D multithread protection unavailable; using CPU readback");
        }
    }
    void create_staging(Graphics &g) {
        if (slots[0].texture)
            return;
        auto desc = output;
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
                if (candidate.pending && (!oldest || candidate.time < oldest->time))
                    oldest = &candidate;
            if (!oldest)
                return;
            auto &slot = *oldest;
            auto start = qpc();
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
            VideoFrame frame;
            frame.time = slot.time;
            frame.fps = slot.fps;
            frame.pixels.resize(video_stride * video_height);
            for (UINT y = 0; y < video_height; ++y)
                std::memcpy(frame.pixels.data() + y * video_stride,
                            static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch, video_stride);
            if (!recorder.enqueue(std::move(frame)))
                recorder.cpu_drop();
            slot.pending = false;
            recorder.readback_ticks += qpc() - start;
        }
    }
    bool pending() const {
        return std::any_of(slots.begin(), slots.end(), [](auto const &slot) { return slot.pending; });
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
    r.presenting = true;
    struct PresentGuard {
        ~PresentGuard() { recorder.presenting = false; }
    } present;
    if (r.stopping)
        return;
    auto now = qpc();
    auto end = r.deadline.load();
    if (end && now >= end) {
        if (readbacks && graphics && readbacks->generation == graphics_generation) {
            readbacks->collect(*graphics);
            if (readbacks->pending())
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
    if (end && now >= end)
        return;
    r.pace_cpu(now);
    // Tolerate present jitter instead of accidentally skipping every other frame at 60 Hz.
    const auto rate = r.fps.load();
    auto period = std::max<int64_t>(1, r.frequency / rate);
    // Half a frame of tolerance: presents jitter by a few ms, and a tighter window skipped a third of them.
    if (r.next_capture && now + period / 2 < r.next_capture)
        return;
    r.next_capture = std::max(r.next_capture + period, now + period / 2);
    ++r.attempted;
    if (r.cpu)
        ++r.window_attempts;
    Readbacks::Slot *slot = nullptr;
    std::shared_ptr<VideoTexture> surface;
    if (r.cpu) {
        readbacks->create_staging(g);
        for (auto &candidate : readbacks->slots)
            if (!candidate.pending) {
                slot = &candidate;
                break;
            }
        if (!slot) {
            r.cpu_drop();
            return;
        }
    } else {
        for (auto &candidate : readbacks->surfaces) {
            if (!candidate) {
                candidate = std::make_shared<VideoTexture>();
                auto hr = g.device->CreateTexture2D(&readbacks->output, nullptr, &candidate->texture);
                if (FAILED(hr)) {
                    r.cpu = true;
                    LOG("recorder: GPU texture allocation failed (0x%08lx); using CPU readback",
                        static_cast<unsigned long>(hr));
                    return;
                }
            }
            if (candidate.use_count() == 1) {
                surface = candidate;
                break;
            }
        }
        if (!surface) {
            ++r.no_surface;
            return;
        }
    }
    if (!end) {
        r.epoch = now;
        end = now + int64_t(r.seconds) * r.frequency;
        r.deadline = end;
        recording_audio.end = end;
        if (!r.stopping)
            recording_audio.epoch = now;
        if (r.stopping)
            recording_audio.epoch = 0;
    }
    auto index = g.chain->GetCurrentBackBufferIndex();
    {
        WrappedGuard acquired(g, index);
        g.context->CopyResource(readbacks->source.Get(), acquired.resource);
        g.canvas->SetTarget(readbacks->scaled_bitmap.Get());
        g.canvas->SetTransform(D2D1::Matrix3x2F::Identity());
        g.canvas->BeginDraw();
        g.canvas->DrawBitmap(readbacks->source_bitmap.Get(), D2D1::RectF(0, 0, video_width, video_height), 1,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        check(g.canvas->EndDraw());
        g.canvas->SetTarget(nullptr);
        g.context->CopyResource(slot ? slot->texture.Get() : surface->texture.Get(), readbacks->scaled.Get());
    }
    if (slot) {
        slot->time = now;
        slot->fps = rate;
        slot->pending = true;
    } else {
        VideoFrame frame;
        frame.time = now;
        frame.fps = rate;
        frame.surface = std::move(surface);
        r.enqueue(std::move(frame));
    }
}
} // namespace uwp
