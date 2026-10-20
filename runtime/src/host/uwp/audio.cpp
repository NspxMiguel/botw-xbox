// SPDX-License-Identifier: MIT
// Runtime AX format: signed PCM16, 48 kHz, 2 or 6 interleaved channels (L R C LFE Ls Rs).
#if defined(BOTW_PLATFORM_UWP)
#include "audio.h"
#include "../../audio/output.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
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
    destroy_locked();
}
} // namespace uwp
#endif
