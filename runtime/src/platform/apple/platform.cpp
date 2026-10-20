#if defined(__APPLE__)
#include "../platform.h"
#include "../../log.h"
#include <TargetConditionals.h>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <memory>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#if TARGET_OS_IOS
size_t botw_ios_stack_size(size_t);
#endif
namespace platform {
void *reserve_guest() {
  void *p = mmap(nullptr, kGuestSize, PROT_READ | PROT_WRITE,
                 MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED)
    return nullptr;
  if (mprotect(p, kNullGuard, PROT_NONE)) {
    munmap(p, kGuestSize);
    return nullptr;
  }
  return p;
}
void release_guest(void *base) { munmap(base, kGuestSize); }
size_t page_size() { return static_cast<size_t>(getpagesize()); }
bool supports_write_watch() { return true; }
bool protect_pages(void *base, size_t size, bool ro) {
  return mprotect(base, size, ro ? PROT_READ : PROT_READ | PROT_WRITE) == 0;
}
ThreadId current_thread() { return pthread_self(); }
void name_thread(const char *name) { pthread_setname_np(name); }
void configure_thread(const char *name, bool critical) {
  name_thread(name);
  pthread_set_qos_class_self_np(
      critical ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
}
namespace {
struct Entry {
  void (*function)(void *);
  void *argument;
};
std::atomic<const char *> sample_label{"?"};
void sample_signal(int) {
  void *frames[48];
  int count = backtrace(frames, 48);
  char line[2048];
  int length = snprintf(line, sizeof line,
                        "  %s host frames (base %p):", sample_label.load(),
                        static_cast<const void *>(_dyld_get_image_header(0)));
  for (int i = 2; i < count && length < static_cast<int>(sizeof line) - 24; ++i)
    length += snprintf(line + length, sizeof line - length, " %p", frames[i]);
  LOG("%s", line);
}
FaultHandler fault_handler = nullptr;
void fault_signal(int sig, siginfo_t *info, void *context) {
  if (fault_handler && fault_handler(info->si_addr, context, true))
    return;
  signal(sig, SIG_DFL);
  raise(sig);
}
} // namespace
bool launch_detached(void (*entry)(void *), void *arg, size_t stack) {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr))
    return false;
#if TARGET_OS_IOS
  stack = botw_ios_stack_size(stack);
#endif
  auto data = std::make_unique<Entry>(Entry{entry, arg});
  pthread_t thread;
  int status = pthread_attr_setstacksize(&attr, stack);
  if (!status)
    status = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (!status)
    status = pthread_create(
        &thread, &attr,
        [](void *p) -> void * {
          std::unique_ptr<Entry> data(static_cast<Entry *>(p));
          data->function(data->argument);
          return nullptr;
        },
        data.get());
  pthread_attr_destroy(&attr);
  if (!status)
    data.release();
  return status == 0;
}
void sample_thread(ThreadId thread, const char *label) {
  static const bool installed = [] {
    struct sigaction sa{};
    sa.sa_handler = sample_signal;
    return sigaction(SIGUSR2, &sa, nullptr) == 0;
  }();
  sample_label = label;
  if (installed)
    pthread_kill(thread, SIGUSR2);
}
bool install_fault_handler(FaultHandler handler) {
  fault_handler = handler;
  struct sigaction sa{};
  sa.sa_sigaction = fault_signal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  return !sigaction(SIGSEGV, &sa, nullptr) &&
         !sigaction(SIGBUS, &sa, nullptr) && !sigaction(SIGILL, &sa, nullptr);
}
uintptr_t fault_pc(void *context) {
  auto *uc = static_cast<ucontext_t *>(context);
#if defined(__aarch64__)
  return uc->uc_mcontext->__ss.__pc;
#else
  return uc->uc_mcontext->__ss.__rip;
#endif
}
void log_watch_fault(void *context, uint32_t address) {
  auto *uc = static_cast<ucontext_t *>(context);
#if defined(__aarch64__)
  uint64_t lr = uc->uc_mcontext->__ss.__lr, fp = uc->uc_mcontext->__ss.__fp;
#else
  uint64_t lr = 0, fp = uc->uc_mcontext->__ss.__rbp;
#endif
  char line[1024];
  int length = snprintf(
      line, sizeof line, "watch: write at %08x, host frames (base %p): %p %p",
      address, static_cast<const void *>(_dyld_get_image_header(0)),
      reinterpret_cast<void *>(fault_pc(context)),
      reinterpret_cast<void *>(lr));
  for (int i = 0; i < 16 && fp && length < static_cast<int>(sizeof line) - 24;
       ++i) {
    auto *frame = reinterpret_cast<const uint64_t *>(fp);
    length += snprintf(line + length, sizeof line - length, " %p",
                       reinterpret_cast<void *>(frame[1]));
    fp = frame[0];
  }
  LOG("%s", line);
}
uint64_t monotonic_ticks() { return mach_absolute_time(); }
uint64_t monotonic_frequency() {
  mach_timebase_info_data_t info{};
  mach_timebase_info(&info);
  return 1000000000ull * info.denom / info.numer;
}
void *open_library(const char *name) {
  return dlopen(name, RTLD_NOW | RTLD_LOCAL);
}
void *library_symbol(void *lib, const char *name) { return dlsym(lib, name); }
void close_library(void *lib) {
  if (lib)
    dlclose(lib);
}
} // namespace platform
#endif
