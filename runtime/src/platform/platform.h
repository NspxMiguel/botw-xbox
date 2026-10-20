#pragma once
#include <cstddef>
#include <cstdint>
#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace platform {
constexpr uint64_t kGuestSize = (1ull << 32) + 0x10000;
constexpr size_t kNullGuard = 0x10000;
#if defined(__APPLE__)
using ThreadId = pthread_t;
#else
using ThreadId = uint32_t;
#endif
// A single stable base, zero-filled pages, inaccessible first 64 KB. Null on
// failure.
void *reserve_guest();
void release_guest(void *base);
size_t page_size();
bool supports_write_watch();
bool protect_pages(void *base, size_t size, bool read_only);
ThreadId current_thread();
void configure_thread(const char *name, bool critical);
void name_thread(const char *name);
// Owns its entry argument only after success. Preserves the guest's large host
// stack.
bool launch_detached(void (*entry)(void *), void *argument, size_t stack_size);
void sample_thread(ThreadId thread, const char *label);
using FaultHandler = bool (*)(void *address, void *context, bool write);
// Called by the host before guest execution. False leaves the OS fault chain
// intact.
bool install_fault_handler(FaultHandler handler);
uintptr_t fault_pc(void *context);
void log_watch_fault(void *context, uint32_t guest_address);
uint64_t monotonic_ticks();
uint64_t monotonic_frequency();
// Espresso ticks elapsed since first call, for rt_timebase / timer consumers.
uint64_t guest_timebase();
// Packaged libraries only on UWP; name is UTF-8, no downloaded modules.
void *open_library(const char *name);
void *library_symbol(void *library, const char *name);
void close_library(void *library);
} // namespace platform
