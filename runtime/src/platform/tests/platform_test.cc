#include "../logic.h"
#include "../memory_policy.h"
#include "../platform.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

void log_write(const char *, ...) {}
int g_log_level = 0;

void test_paths() {
  using namespace platform;
  auto local = local_roots("C:/LocalState");
  assert(local.game == "C:/LocalState/game" && local.user == "C:/LocalState");
  auto roots = local_roots("C:/LocalState", "E:/Zelda 日本語");
  assert(guest_path("/vol/content/Pack/Bootup.pack", "", roots) ==
         "E:/Zelda 日本語/content/Pack/Bootup.pack");
  assert(guest_path("file", "/vol/save/common", roots) ==
         "C:/LocalState/save/common/file");
  assert(guest_path("//vol///code/./x/../file", "", roots) ==
         "E:/Zelda 日本語/code/file");
  assert(guest_path("/vol/meta", "", roots) == "E:/Zelda 日本語/meta");
  assert(guest_path("/vol/aoc/1", "", roots) == "E:/Zelda 日本語/aoc/1");
  for (const char *bad : {"/vol/content/../../save/x", "/vol/contentious/x",
                          "/vol/save/../code/x", "/vol/content/C:/secret",
                          "/vol/content/..\\secret", "/other/file", "/vol"})
    assert(guest_path(bad, "", roots).empty());
  assert(guest_path(std::string("/vol/content/a\0b", 16), "", roots).empty());
}
void test_commit_policy() {
  constexpr uintptr_t base = 0x100000000ull;
  using platform::guest_commit_page;
  assert(!guest_commit_page(0, base, 0));
  assert(!guest_commit_page(base, base - 1, 0));
  assert(!guest_commit_page(base, base + 0xFFFF, 1));
  assert(guest_commit_page(base, base + 0x10000, 0) == base + 0x10000);
  assert(guest_commit_page(base, base + 0x12345, 1) == base + 0x10000);
  assert(guest_commit_page(base, base + platform::kGuestSize - 1, 1) ==
         base + (1ull << 32));
  assert(!guest_commit_page(base, base + platform::kGuestSize, 0));
  assert(!guest_commit_page(base, base + 0x10000, 8));
}
void test_uwp_memory_policy() {
  using namespace platform;
  constexpr uint64_t mib = 1ull << 20;
  assert(guest_initial_commit_bytes(true) == 2816 * mib);
  assert(guest_initial_commit_bytes(false) == 4096 * mib);
  assert(uwp_memory_limit(10ull << 30) == 5120 * mib);
  assert(uwp_memory_limit(1ull << 30) == 1024 * mib);
  assert(uwp_memory_admits_guest(5120 * mib, 256 * mib, true));
  assert(uwp_memory_admits_guest(5120 * mib, 256 * mib, false));
  assert(!uwp_memory_admits_guest(1024 * mib, 0, true)); // App classification.
  assert(!uwp_memory_admits_guest(1024 * mib, 0, false));
  assert(!uwp_memory_admits_guest(0, 0, true));
  for (bool lazy : {false, true}) {
    const auto remaining =
        5120 * mib - guest_initial_commit_bytes(lazy) - kUwpStartupReserve;
    assert(uwp_memory_admits_guest(5120 * mib, remaining, lazy));
    assert(!uwp_memory_admits_guest(5120 * mib, remaining + 1, lazy));
    assert(!uwp_memory_admits_guest(10ull << 30, remaining + 1, lazy));
    assert(!uwp_memory_admits_guest(5120 * mib, UINT64_MAX, lazy));
  }
}
void test_clock() {
  using namespace platform;
  assert(scale_ticks(0, 24000000, 62156250) == 0);
  assert(scale_ticks(24000000, 24000000, 62156250) == 62156250);
  assert(scale_ticks(10000001, 10000000, 62156250) == 62156256);
  assert(scale_ticks(62156250, 62156250, 1000000000) == 1000000000);
  // Longer than the old multiply-before-divide implementation can hold in 64
  // bits.
  for (uint64_t frequency : {10000000ull, 24000000ull, 1000000000ull}) {
    for (uint64_t ticks : {0ull, 1ull, 999999999ull, 123456789012345ull}) {
      auto expected = static_cast<uint64_t>(static_cast<__uint128_t>(ticks) *
                                            62156250 / frequency);
      assert(scale_ticks(ticks, frequency, 62156250) == expected);
    }
  }
  uint64_t before = guest_timebase();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(guest_timebase() > before);
}
void test_mapping() {
  using namespace platform;
  auto *base = static_cast<volatile unsigned char *>(reserve_guest());
  assert(base);
  assert(base[kNullGuard] == 0 && base[0xFFFFFFFFull] == 0);
  base[kNullGuard] = 42;
  base[0xFFFFFFFFull] = 7;
  assert(base[kNullGuard] == 42 && base[0xFFFFFFFFull] == 7);
  auto expect_fault = [&](uint64_t offset, bool write) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      if (write)
        base[offset] = 1;
      else {
        volatile auto value = base[offset];
        (void)value;
      }
      _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) &&
           (WTERMSIG(status) == SIGBUS || WTERMSIG(status) == SIGSEGV));
  };
  expect_fault(0, false);
  auto *page = const_cast<unsigned char *>(base) + kNullGuard;
  assert(protect_pages(page, page_size(), true));
  expect_fault(kNullGuard, true);
  assert(protect_pages(page, page_size(), false));
  release_guest(const_cast<unsigned char *>(base));
}
int main() {
  test_paths();
  test_clock();
  test_commit_policy();
  test_uwp_memory_policy();
  test_mapping();
  std::atomic<bool> finished{false};
  assert(platform::launch_detached(
      [](void *p) {
        platform::configure_thread("platform test", true);
        static_cast<std::atomic<bool> *>(p)->store(true);
      },
      &finished, 64u << 20));
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!finished.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  assert(finished.load());
  void *library = platform::open_library("/usr/lib/libSystem.B.dylib");
  assert(library && platform::library_symbol(library, "malloc"));
  platform::close_library(library);
  std::puts("platform tests passed");
}
