#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "../platform.h"
#include "../../log.h"
#include "../logic.h"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <windows.h> // must precede the *api.h headers: it defines the target architecture
#include <memoryapi.h>
#include <processthreadsapi.h>
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#define BOTW_HAS_VEH 1
#else
#define BOTW_HAS_VEH 0
#endif
namespace platform {
namespace {
std::atomic<uintptr_t> guest_base{0};
FaultHandler fault_handler = nullptr;
#if BOTW_HAS_VEH
PVOID exception_cookie = nullptr;
#endif
LONG CALLBACK exception_handler(EXCEPTION_POINTERS *info) {
  const auto &record = *info->ExceptionRecord;
  if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      record.NumberParameters < 2)
    return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t address = record.ExceptionInformation[1];
  uintptr_t base = guest_base.load();
  // Only data accesses inside the reservation may commit pages. Guards and
  // execute faults remain faults. Never turn a write-protected watch page
  // writable here.
  uintptr_t commit_page =
      guest_commit_page(base, address, record.ExceptionInformation[0]);
  if (BOTW_HAS_VEH && commit_page) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void *>(address), &region,
                     sizeof(region)) &&
        region.State == MEM_RESERVE) {
      void *page = reinterpret_cast<void *>(commit_page);
      if (VirtualAlloc2FromApp(GetCurrentProcess(), page, 0x10000, MEM_COMMIT,
                               PAGE_READWRITE, nullptr, 0))
        return EXCEPTION_CONTINUE_EXECUTION;
      // Commit exhaustion is not recoverable by the write watcher.
      return EXCEPTION_CONTINUE_SEARCH;
    }
  }
  if (fault_handler &&
      fault_handler(reinterpret_cast<void *>(address), info->ContextRecord,
                    record.ExceptionInformation[0] == 1))
    return EXCEPTION_CONTINUE_EXECUTION;
  return EXCEPTION_CONTINUE_SEARCH;
}
bool ensure_exceptions() {
#if BOTW_HAS_VEH
  if (!exception_cookie)
    exception_cookie = AddVectoredExceptionHandler(1, exception_handler);
  return exception_cookie != nullptr;
#else
  // VEH is Desktop/System/Games, not APP, in the Microsoft SDK. A UWP top-level
  // filter is diagnostic only; guest memory must be committed before execution.
  SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *info) -> LONG {
    exception_handler(info);
    return EXCEPTION_CONTINUE_SEARCH;
  });
  return true;
#endif
}
std::wstring wide(const char *text) {
  int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
  if (!size)
    return {};
  std::wstring result(size, L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                           result.data(), size))
    return {};
  result.pop_back();
  return result;
}
struct Entry {
  void (*function)(void *);
  void *argument;
};
DWORD WINAPI thread_entry(void *p) {
  std::unique_ptr<Entry> data(static_cast<Entry *>(p));
  data->function(data->argument);
  return 0;
}
} // namespace
void *reserve_guest() {
  static_assert(sizeof(void *) == 8, "Guest mapping requires a 64-bit host");
  if (guest_base.load() || !ensure_exceptions())
    return nullptr;
  void *p = VirtualAlloc2FromApp(GetCurrentProcess(), nullptr, kGuestSize,
                                 MEM_RESERVE, PAGE_NOACCESS, nullptr, 0);
#if !BOTW_HAS_VEH
  if (p && !VirtualAllocFromApp(static_cast<char *>(p) + kNullGuard,
                                kGuestSize - kNullGuard, MEM_COMMIT,
                                PAGE_READWRITE)) {
    VirtualFree(p, 0, MEM_RELEASE);
    return nullptr;
  }
#endif
  if (p)
    guest_base = reinterpret_cast<uintptr_t>(p);
  return p;
}
void release_guest(void *p) {
  if (VirtualFree(p, 0, MEM_RELEASE))
    guest_base = 0;
}
size_t page_size() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  return info.dwPageSize;
}
bool supports_write_watch() { return BOTW_HAS_VEH; }
bool protect_pages(void *p, size_t size, bool ro) {
  // Watching an untouched page must first commit it; VirtualProtect cannot do
  // so.
  if (!VirtualAllocFromApp(p, size, MEM_COMMIT, PAGE_READWRITE))
    return false;
  ULONG old;
  return VirtualProtectFromApp(p, size, ro ? PAGE_READONLY : PAGE_READWRITE,
                               &old) != 0;
}
ThreadId current_thread() { return GetCurrentThreadId(); }
void name_thread(const char *name) {
  auto value = wide(name);
  SetThreadDescription(GetCurrentThread(), value.c_str());
}
void configure_thread(const char *name, bool critical) {
  name_thread(name);
  SetThreadPriority(GetCurrentThread(), critical ? THREAD_PRIORITY_ABOVE_NORMAL
                                                 : THREAD_PRIORITY_NORMAL);
}
bool launch_detached(void (*entry)(void *), void *arg, size_t stack) {
  auto data = std::make_unique<Entry>(Entry{entry, arg});
  // CreateThread is permitted by UWP. Reserve, rather than commit, the 64 MB
  // guest recursion stack. Windows grows it through its normal stack guard
  // mechanism.
  HANDLE thread = CreateThread(nullptr, stack, thread_entry, data.get(),
                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!thread)
    return false;
  data.release();
  CloseHandle(thread);
  return true;
}
void sample_thread(ThreadId thread, const char *label) {
  // Do not suspend arbitrary UWP threads (could hold allocator/loader locks).
  LOG("%s host thread %lu: remote stack sampling unavailable on UWP", label,
      static_cast<unsigned long>(thread));
}
bool install_fault_handler(FaultHandler handler) {
  fault_handler = handler;
  return ensure_exceptions();
}
uintptr_t fault_pc(void *context) {
  return static_cast<CONTEXT *>(context)->Rip;
}
void log_watch_fault(void *context, uint32_t address) {
  LOG("watch: write at %08x, host pc %p", address,
      reinterpret_cast<void *>(fault_pc(context)));
}
uint64_t monotonic_ticks() {
  LARGE_INTEGER value{};
  if (!QueryPerformanceCounter(&value))
    std::abort();
  return value.QuadPart;
}
uint64_t monotonic_frequency() {
  LARGE_INTEGER value{};
  if (!QueryPerformanceFrequency(&value))
    std::abort();
  return value.QuadPart;
}
void *open_library(const char *name) {
  auto value = wide(name);
  if (value.empty())
    return nullptr;
#if BOTW_HAS_VEH
  return LoadLibraryExW(value.c_str(), nullptr,
                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
  return LoadPackagedLibrary(value.c_str(), 0);
#endif
}
void *library_symbol(void *lib, const char *name) {
  return lib ? reinterpret_cast<void *>(
                   GetProcAddress(static_cast<HMODULE>(lib), name))
             : nullptr;
}
void close_library(void *lib) {
  if (lib)
    FreeLibrary(static_cast<HMODULE>(lib));
}
} // namespace platform
#endif
