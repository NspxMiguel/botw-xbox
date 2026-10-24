// Synthetic host benchmark. AX kernels and indirect dispatch are the real
// runtime sources.
#include "guest_table.h"
#include "snd/ax_internal.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

uint8_t *g_mem = nullptr;
uint8_t *memory_base = nullptr;
void log_write(const char *, ...) {}
namespace threads {
const char *current_name() { return "benchmark"; }
} // namespace threads
extern "C" void rt_fatal_exit() { std::abort(); }
namespace snd_core {
AXVPB *__AXVPBArrayPtr = nullptr;
void AX_DecodeSamplesPCM16_Linear(AXVPBInternal_t *, float *, sint32);
void AX_DecodeSamplesADPCM_Linear(AXVPBInternal_t *, float *, sint32);
void AXVoiceMix_ApplyBiquad(AXVPBInternal_t *, float *, sint32);
} // namespace snd_core
extern "C" {
void current_mfcr(PPCContext *);
void baseline_mfcr(PPCContext *);
void current_mtcrf(PPCContext *);
void baseline_mtcrf(PPCContext *);
void current_mtcrf_partial(PPCContext *);
void baseline_mtcrf_partial(PPCContext *);
void candidate_mtcrf(PPCContext *);
void current_extract(PPCContext *);
void unfolded_extract(PPCContext *);
void guest_0(PPCContext *);
}

using Clock = std::chrono::steady_clock;
uint64_t checksum = 0;
template <typename F>
void measure(const char *name, int iterations, F function) {
  // Warm up first; report median and range of seven complete trials.
  for (int i = 0; i < 1000; ++i)
    function(i);
  std::array<double, 7> trials;
  for (auto &trial : trials) {
    auto start = Clock::now();
    for (int i = 0; i < iterations; ++i)
      function(i);
    trial =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
        iterations;
  }
  std::sort(trials.begin(), trials.end());
  printf("%s,%.3f,%.3f,%.3f\n", name, trials[3], trials.front(), trials.back());
}

// Keep context effects observable across iterations without a volatile memory
// workload.
inline void observe(void *address) {
  asm volatile("" : : "r"(address) : "memory");
}

__attribute__((noinline)) void record_compare(PPCContext *c) {
  set_cr_s(c, 0, int32_t(c->r[3]), int32_t(c->r[4]));
}
__attribute__((noinline)) void carry_add(PPCContext *c) {
  c->r[3] = adde_(c, c->r[3], c->r[4], c->ca);
}
void set_pair(uint16_t &high, uint32_t value) {
  value = botw_bswap32(value);
  memcpy(&high, &value, sizeof(value));
}

void audio_bench() {
  using namespace snd_core;
  AXVPB vpb{};
  __AXVPBArrayPtr = &vpb;
  std::fill(g_mem + 4096, g_mem + 8192, 0x11);
  vpb.offsets.samples = botw_bswap32(4096);
  vpb.offsets.loopOffset = botw_bswap32(2);
  vpb.offsets.endOffset = botw_bswap32(1023);
  AXVPBInternal_t pcm{}, adpcm{};
  pcm.playbackState = botw_bswap16(1);
  pcm.internalOffsets.loopFlag = botw_bswap16(1);
  set_pair(pcm.src.ratioHigh, 0x10000);
  set_pair(pcm.internalOffsets.currentOffsetPtrHigh, 512);
  set_pair(pcm.internalOffsets.loopOffsetPtrHigh, 512);
  set_pair(pcm.internalOffsets.endOffsetPtrHigh, 1023);
  adpcm = pcm;
  set_pair(adpcm.internalOffsets.currentOffsetPtrHigh, 8194);
  set_pair(adpcm.internalOffsets.loopOffsetPtrHigh, 8194);
  adpcm.adpcmData.scale = botw_bswap16(0x11);
  adpcm.adpcmLoop.loopScale = botw_bswap16(0x11);
  adpcm.adpcmData.coef[2] = botw_bswap16(1024);
  std::array<float, 144> output{};
  measure("AX_PCM16_linear_144_samples", 20000, [&](int) {
    auto voice = pcm;
    AX_DecodeSamplesPCM16_Linear(&voice, output.data(), 144);
    observe(output.data());
  });
  measure("AX_ADPCM_linear_144_samples", 20000, [&](int) {
    auto voice = adpcm;
    AX_DecodeSamplesADPCM_Linear(&voice, output.data(), 144);
    observe(output.data());
  });
  checksum += uint32_t(output[0]);
  auto input = output;
  AXVPBInternal_t filter{};
  filter.biquad.on = botw_bswap16(1);
  filter.biquad.b0 = botw_bswap16(8192);
  filter.biquad.b2 = botw_bswap16(2048);
  filter.biquad.a1 = botw_bswap16(4096);
  for (auto name :
       {"AX_biquad_b1_zero_144_samples", "AX_biquad_b1_nonzero_144_samples"}) {
    measure(name, 20000, [&](int) {
      auto voice = filter;
      auto samples = input;
      AXVoiceMix_ApplyBiquad(&voice, samples.data(), 144);
      observe(samples.data());
    });
    filter.biquad.b1 = botw_bswap16(2048);
  }
  __AXVPBArrayPtr = nullptr;
}

void wake_bench(int microseconds, int count) {
  std::mutex mutex;
  std::condition_variable condition;
  std::unique_lock lock(mutex);
  std::vector<double> late;
  auto deadline = Clock::now();
  for (int i = 0; i < count; ++i) {
    deadline += std::chrono::microseconds(microseconds);
    condition.wait_until(lock, deadline, [] { return false; });
    late.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - deadline)
            .count());
  }
  std::sort(late.begin(), late.end());
  printf("wake_%d_us_late,%.3f,%.3f,%.3f\n", microseconds, late[count / 2],
         late[count * 95 / 100], late.back());
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::vector<uint8_t> memory(65536);
  g_mem = memory_base = memory.data();
  PPCContext context{};
  context.r[3] = 12345;
  context.r[4] = 0xfedcba98;
  dispatch::init();
  puts("kernel,median_ns,min_ns,max_ns");
  for (auto [name, fn] :
       {std::pair{"baseline_mfcr", baseline_mfcr},
        std::pair{"current_mfcr", current_mfcr},
        std::pair{"baseline_mtcrf", baseline_mtcrf},
        std::pair{"current_mtcrf", current_mtcrf},
        std::pair{"baseline_mtcrf_partial", baseline_mtcrf_partial},
        std::pair{"current_mtcrf_partial", current_mtcrf_partial},
        std::pair{"candidate_mtcrf", candidate_mtcrf},
        std::pair{"current_extract", current_extract},
        std::pair{"unfolded_extract", unfolded_extract},
        std::pair{"CR_compare", record_compare},
        std::pair{"XER_carry", carry_add}}) {
    measure(name, 1000000, [&](int i) {
      context.r[3] += uint32_t(i) | 1;
      context.cr[i & 31] ^= 1;
      fn(&context);
      observe(&context);
    });
  }
  measure("direct_call", 1000000, [&](int) { guest_0(&context); });
  measure("rt_call_monomorphic", 1000000,
          [&](int) { rt_call(&context, 0x02000000); });
  measure("rt_call_64_targets", 1000000, [&](int i) {
    uint32_t index = (uint32_t(i) * 1664525u + 1013904223u) >> 26;
    rt_call(&context, 0x02000000 + index * 4);
  });
  measure("guest_callback_save_restore", 100000,
          [&](int) { guest_call(&context, 0x02000000, {}); });
  // Pointer chasing prevents vectorization/hoisting; compare equal working
  // sets.
  for (uint32_t i = 0; i < 4096; ++i)
    st32_(g_mem, i * 4, ((i + 1) & 4095) * 4);
  uint32_t address = 0;
  measure("LD32_BE_dependent_L1", 1000000,
          [&](int) { address = ld32_(g_mem, address); });
  checksum += address;
  for (uint32_t i = 0; i < 4096; ++i)
    st32r_(g_mem, i * 4, ((i + 1) & 4095) * 4);
  address = 0;
  measure("LD32_native_dependent_L1", 1000000,
          [&](int) { address = ld32r_(g_mem, address); });
  checksum += address;
  measure("ST32_BE_L1", 1000000, [&](int i) {
    st32_(g_mem, (uint32_t(i) & 4095) * 4, uint32_t(i));
    observe(g_mem);
  });
  measure("lfs_bit_exact", 1000000, [&](int i) {
    context.f[0] = f32_to_f64(ld32r_(g_mem, (uint32_t(i) & 4095) * 4));
    observe(&context);
  });
  audio_bench();
  // Recorder CPU fallback: copy mapped 1080p BGRA rows, including padded pitch.
  constexpr size_t stride = 1920 * 4, pitch = stride + 256, height = 1080;
  std::vector<uint8_t> captured(pitch * height, 0x5a), pixels(stride * height);
  measure("recorder_row_copy_1080p_BGRA", 500, [&](int) {
    for (size_t row = 0; row < height; ++row)
      memcpy(pixels.data() + row * stride, captured.data() + row * pitch,
             stride);
    observe(pixels.data());
  });
  checksum += pixels[0];
  checksum += context.r[3];
  printf("checksum,%llu\n", static_cast<unsigned long long>(checksum));
  puts("wake_test,median_us,p95_us,max_us");
  wake_bench(3000, 100);
  wake_bench(16667, 60);
}
