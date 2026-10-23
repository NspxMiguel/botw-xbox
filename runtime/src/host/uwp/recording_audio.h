// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace uwp::capture_detail {
inline constexpr uint32_t audio_rate = 48000, audio_chunk_frames = 2048;
inline constexpr int64_t media_second = 10000000;
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
struct AudioChunk {
    int64_t time = 0;
    uint32_t frames = 0;
    std::array<int16_t, audio_chunk_frames * 2> samples;
};
// Single producer (push_tv holds the voice mutex), single consumer (the MF worker).
// Storage outlives both threads, including a producer racing with suspend/finalization.
struct AudioRing {
    std::array<AudioChunk, 256> chunks;
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    std::atomic<int64_t> epoch{0}, end{0};
    std::atomic<bool> producing{false};
    std::atomic<uint64_t> dropped_frames{0};
};
inline AudioRing recording_audio;
} // namespace uwp::capture_detail

namespace uwp {
// One atomic load while inactive; no QPC, allocation, lock, or MF calls on the audio thread.
inline int64_t recording_audio_time() {
    return capture_detail::recording_audio.epoch.load(std::memory_order_acquire) ? capture_detail::qpc() : 0;
}
inline void record_audio(const int16_t *stereo, uint32_t frames, int64_t time, uint32_t offset) {
    using namespace capture_detail;
    auto &ring = recording_audio;
    if (!time || !ring.epoch.load(std::memory_order_acquire))
        return;
    if (offset)
        time += int64_t(offset) * qpc_frequency() / audio_rate;
    if (time >= ring.end.load())
        return;
    ring.producing = true;
    struct ProducerGuard {
        ~ProducerGuard() { recording_audio.producing = false; }
    } producer;
    if (!ring.epoch.load())
        return;
    auto head = ring.head.load(std::memory_order_relaxed);
    if (frames > audio_chunk_frames ||
        head - ring.tail.load(std::memory_order_acquire) >= ring.chunks.size()) {
        ring.dropped_frames.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    auto &chunk = ring.chunks[head % ring.chunks.size()];
    chunk.time = time;
    chunk.frames = frames;
    std::memcpy(chunk.samples.data(), stereo, frames * 2 * sizeof(int16_t));
    ring.head.store(head + 1, std::memory_order_release);
}
} // namespace uwp
