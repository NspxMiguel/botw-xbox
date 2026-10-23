// SPDX-License-Identifier: MIT
// Runtime AX format: signed PCM16, 48 kHz, 2 or 6 interleaved channels (L R C LFE Ls Rs).
#if defined(BOTW_PLATFORM_UWP)
#include "audio.h"
#include "../../audio/output.h"
#include "drive_sound.h"
#include "first_frame.h"
#include "polish.h"
#include "recording_audio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <windows.h>
#include <xaudio2.h>

namespace {
constexpr size_t frames_per_buffer = 2048;
struct Slot {
    std::array<int16_t, frames_per_buffer * 2> samples{};
    std::atomic<bool> busy{false};
};
struct Callback final : IXAudio2VoiceCallback {
    std::atomic<bool> failed{false};
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override { failed = true; }
    void STDMETHODCALLTYPE OnBufferEnd(void *context) override {
        static_cast<Slot *>(context)->busy.store(false, std::memory_order_release);
    }
};
// All voice operations are serialized. Callback only releases slots; it never takes this mutex.
std::mutex lock;
std::array<Slot, 16> slots;
Callback callback;
IXAudio2 *engine = nullptr;
IXAudio2MasteringVoice *master = nullptr;
IXAudio2SourceVoice *source = nullptr;
// Launcher voice has its own engine: menu pause must not mute the spin-up cue.
IXAudio2 *drive_engine = nullptr;
IXAudio2MasteringVoice *drive_master = nullptr;
IXAudio2SourceVoice *drive_voice = nullptr;
std::vector<float> drive_samples;
float drive_level = 1;
std::chrono::steady_clock::time_point drive_fade_start{};
IXAudio2 *tick_engine = nullptr;
IXAudio2MasteringVoice *tick_master = nullptr;
IXAudio2SourceVoice *tick_voice = nullptr;
std::array<float, 2400> tick_samples{};
std::vector<float> load_drive() {
    // Package-relative lookup works independently of launcher's working directory.
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768))
        return {};
    std::ifstream file(std::filesystem::path(path).parent_path() / L"Assets" / L"sfx" / L"drive.wav",
                       std::ios::binary);
    if (!file)
        return {};
    file.seekg(0, std::ios::end);
    auto length = file.tellg();
    if (length < 44 || length > 32 * 1024 * 1024)
        return {};
    file.seekg(0);
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return {};
    auto u16 = [&](size_t i) { return unsigned(bytes[i]) | (unsigned(bytes[i + 1]) << 8); };
    auto u32 = [&](size_t i) { return u16(i) | (uint32_t(u16(i + 2)) << 16); };
    if (bytes.size() < 44 || bytes.size() > 32 * 1024 * 1024 ||
        std::string(bytes.begin(), bytes.begin() + 4) != "RIFF" ||
        std::string(bytes.begin() + 8, bytes.begin() + 12) != "WAVE")
        return {};
    bool format = false;
    for (size_t i = 12; i + 8 <= bytes.size();) {
        size_t n = u32(i + 4), start = i + 8;
        if (n > bytes.size() - start)
            return {};
        std::string tag(bytes.begin() + i, bytes.begin() + i + 4);
        if (tag == "fmt " && n >= 16)
            format =
                u16(start) == 1 && u16(start + 2) == 1 && u32(start + 4) == 48000 && u16(start + 14) == 16;
        if (tag == "data" && format && !(n % 2)) {
            std::vector<float> samples(n / 2);
            for (size_t j = 0; j < samples.size(); ++j)
                samples[j] = int16_t(u16(start + j * 2)) / 32768.f;
            return samples;
        }
        i = start + n + (n & 1);
    }
    return {};
}
void destroy_drive_locked() {
    if (drive_voice)
        drive_voice->DestroyVoice();
    if (drive_master)
        drive_master->DestroyVoice();
    if (drive_engine)
        drive_engine->Release();
    drive_voice = nullptr;
    drive_master = nullptr;
    drive_engine = nullptr;
    drive_samples.clear();
}
uint64_t submitted = 0;
bool paused = false;
float volume = 1;
void destroy_locked() {
    // DestroyVoice waits for callbacks, so slot memory cannot be reused prematurely.
    if (source)
        source->DestroyVoice();
    if (master)
        master->DestroyVoice();
    if (engine)
        engine->Release();
    source = nullptr;
    master = nullptr;
    engine = nullptr;
    submitted = 0;
    for (auto &slot : slots)
        slot.busy = false;
}
} // namespace
namespace audio {
void init() {
    std::lock_guard guard(lock);
    if (engine)
        return;
    callback.failed = false;
    HRESULT hr = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr))
        hr = engine->CreateMasteringVoice(&master, 2, 48000);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 192000;
    if (SUCCEEDED(hr))
        hr = engine->CreateSourceVoice(&source, &format, 0, 1, &callback);
    if (SUCCEEDED(hr))
        hr = source->Start();
    if (FAILED(hr)) {
        OutputDebugStringW(L"BotW: XAudio2 initialization failed; output muted.\n");
        destroy_locked();
    } else if (paused)
        engine->StopEngine();
}
void set_volume(float v) {
    std::lock_guard guard(lock);
    volume = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}
void push_tv(const int16_t *input, int frames, int channels) {
    if (!input || frames <= 0 || (channels != 1 && channels != 2 && channels != 6))
        return;
    std::lock_guard guard(lock);
    if (!source || paused || callback.failed)
        return;
    const auto recording_time = uwp::recording_audio_time();
    // Bounded queue: drop excess samples rather than allocate or block the AX callback.
    for (int offset = 0; offset < frames;) {
        Slot *available = nullptr;
        for (auto &slot : slots) {
            if (!slot.busy.load(std::memory_order_acquire)) {
                available = &slot;
                break;
            }
        }
        if (!available)
            return;
        const int count = std::min(frames - offset, static_cast<int>(frames_per_buffer));
        for (int i = 0; i < count; ++i) {
            const int16_t *s = input + static_cast<size_t>(offset + i) * channels;
            float l = s[0], r = channels == 1 ? s[0] : s[1];
            if (channels == 6) {
                l += 0.707f * (s[2] + s[4]);
                r += 0.707f * (s[2] + s[5]);
            }
            available->samples[i * 2] = static_cast<int16_t>(std::clamp(l * volume, -32768.f, 32767.f));
            available->samples[i * 2 + 1] = static_cast<int16_t>(std::clamp(r * volume, -32768.f, 32767.f));
        }
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = count * 4;
        buffer.pAudioData = reinterpret_cast<const BYTE *>(available->samples.data());
        buffer.pContext = available;
        available->busy.store(true, std::memory_order_release);
        if (FAILED(source->SubmitSourceBuffer(&buffer))) {
            available->busy.store(false, std::memory_order_release);
            return;
        }
        uwp::record_audio(available->samples.data(), count, recording_time, offset);
        submitted += count;
        offset += count;
    }
}
int queued_frames() {
    std::lock_guard guard(lock);
    if (!source || paused || callback.failed)
        return 0;
    XAUDIO2_VOICE_STATE state{};
    source->GetState(&state);
    return static_cast<int>(submitted > state.SamplesPlayed ? submitted - state.SamplesPlayed : 0);
}
} // namespace audio
namespace uwp {
void play_drive_spinup(float level) {
    std::lock_guard guard(lock);
    destroy_drive_locked();
    drive_samples = load_drive();
    size_t loop_start = prepare_drive_loop(drive_samples);
    if (drive_samples.empty()) {
        drive_samples = make_drive_spinup(48000);
        loop_start = drive_samples.size() * 3 / 4;
        // Smooth fallback tail into its steady head as well.
        size_t overlap = 4800, end = drive_samples.size() - overlap;
        for (size_t i = 0; i < overlap; ++i) {
            float t = float(ease(double(i) / (overlap - 1)));
            drive_samples[end + i] = drive_samples[end + i] * (1 - t) + drive_samples[loop_start + i] * t;
        }
        loop_start += overlap;
    }
    drive_fade_start = {};
    first_frame_presented = false;
    drive_level = std::isfinite(level) ? std::clamp(level, 0.f, 1.f) : 0.f;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 192000;
    HRESULT hr = XAudio2Create(&drive_engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr))
        hr = drive_engine->CreateMasteringVoice(&drive_master, 2, 48000);
    if (SUCCEEDED(hr))
        hr = drive_engine->CreateSourceVoice(&drive_voice, &format);
    XAUDIO2_BUFFER buffer{};
    buffer.LoopBegin = static_cast<UINT32>(loop_start);
    buffer.LoopLength = static_cast<UINT32>(drive_samples.size() - loop_start);
    buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    buffer.AudioBytes = static_cast<UINT32>(drive_samples.size() * sizeof(float));
    buffer.pAudioData = reinterpret_cast<const BYTE *>(drive_samples.data());
    if (SUCCEEDED(hr))
        hr = drive_voice->SetVolume(std::isfinite(level) ? std::clamp(level, 0.f, 1.f) : 0.f);
    if (SUCCEEDED(hr))
        hr = drive_voice->SubmitSourceBuffer(&buffer);
    if (SUCCEEDED(hr))
        hr = drive_voice->Start();
    if (FAILED(hr))
        destroy_drive_locked();
}

void stop_drive() {
    std::lock_guard guard(lock);
    destroy_drive_locked();
}
void update_launcher_audio() {
    std::lock_guard guard(lock);
    if (!drive_voice || !first_frame_presented.load(std::memory_order_acquire))
        return;
    auto now = std::chrono::steady_clock::now();
    if (!drive_fade_start.time_since_epoch().count())
        drive_fade_start = now;
    double elapsed = std::chrono::duration<double>(now - drive_fade_start).count();
    drive_voice->SetVolume(drive_level * drive_fade(elapsed));
    if (elapsed >= .25)
        destroy_drive_locked();
}
void play_ui_tick() {
    std::lock_guard guard(lock);
    if (!tick_engine) {
        for (size_t i = 0; i < tick_samples.size(); ++i) {
            double t = double(i) / 48000;
            tick_samples[i] = float(.055 * std::sin(2 * pi * 1700 * t) * std::exp(-t * 130) *
                                    std::sin(pi * i / tick_samples.size()));
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 1;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = 192000;
        if (FAILED(XAudio2Create(&tick_engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
            FAILED(tick_engine->CreateMasteringVoice(&tick_master, 2, 48000)) ||
            FAILED(tick_engine->CreateSourceVoice(&tick_voice, &format)))
            return;
    }
    if (!tick_voice)
        return;
    tick_voice->Stop();
    tick_voice->FlushSourceBuffers();
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = sizeof(tick_samples);
    buffer.pAudioData = reinterpret_cast<const BYTE *>(tick_samples.data());
    tick_voice->SetVolume(volume);
    tick_voice->SubmitSourceBuffer(&buffer);
    tick_voice->Start();
}

void pause_audio(bool value) {
    std::lock_guard guard(lock);
    if (paused == value)
        return;
    paused = value;
    if (!engine)
        return;
    if (paused)
        engine->StopEngine();
    else if (FAILED(engine->StartEngine()))
        callback.failed = true;
}
void shutdown_audio() {
    std::lock_guard guard(lock);
    destroy_drive_locked();
    if (tick_voice)
        tick_voice->DestroyVoice();
    if (tick_master)
        tick_master->DestroyVoice();
    if (tick_engine)
        tick_engine->Release();
    tick_voice = nullptr;
    tick_master = nullptr;
    tick_engine = nullptr;
    destroy_locked();
}
} // namespace uwp
#endif
