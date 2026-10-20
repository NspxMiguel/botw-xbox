# W1 platform boundary

`platform.h` is the OS boundary; `logic.h` contains portable guest-path, clock,
and demand-commit policy. Select `apple/platform.cpp` or `win32/platform.cpp`,
plus `clock.cpp`. The root build has a W1 source-selection block. The existing
iOS recursive source glob also works: the Windows implementation is preprocessor
guarded, and test entry points deliberately use `.cc`, outside that glob.
Windows builds require x64, Windows 10 1803 or later (`VirtualAlloc2FromApp`),
C++20, and the SDK's `WindowsApp.lib` for UWP (desktop uses its usual SDK imports).
Use Clang/clang-cl for existing runtime compiler builtins.

## Memory and exception constraints

The entire 4 GB guest space plus the existing 64 KB tail is reserved once; all
addresses are offsets from that single stable base. The first 64 KB remains
inaccessible. Apple retains its zero-fill, no-reserve mmap behavior. Desktop
Windows commits 64 KB on data faults through VEH and never handles execute
faults or changes protection of already committed watch pages.

**UWP does not expose AddVectoredExceptionHandler.** Microsoft's SDK declares it
only for Desktop/System/Games partitions, not APP:
[SDK errhandlingapi.h](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/errhandlingapi.h).
Do not hide this by adding a private declaration, dynamic lookup, or changing the
app family. The UWP backend reserves the same range, explicitly commits all but
the null guard with `VirtualAllocFromApp`, and uses `VirtualProtectFromApp`.
Commit failure releases the reservation and fails startup. This incurs a 4 GB
commit charge, though physical pages are populated on access; check Xbox process
memory limits before treating this as a shippable allocation strategy. A lower
commit budget needs explicit commits in every guest allocation/loading path or
a different supported exception strategy; it cannot depend on UWP VEH.

UWP uses a diagnostic top-level exception filter and polling-only BOTW_WATCH;
it cannot report the writer's fault PC for polling changes. Win32 retains the
VEH write watcher. Other faults continue through the OS exception chain.
Windows remote thread stack sampling logs its lack of support rather than
suspending threads that could hold runtime locks. Apple uses its signal sampler.
Detached guest threads preserve a 64 MB host stack reservation (iOS keeps its
existing stack-size adjustment), with Win32 priorities instead of Apple QoS.

## Integration outside W1 ownership

The runtime-owned `mem.*`, `thread.*`, `debug_watch.cpp`, and FS path code use
these interfaces. The following consumers are outside W1's contracted paths
and have intentionally not been edited:

- W3: before FS/settings/loader startup, call `platform::local_roots(localState,
  externalGame)` and assign `.user`/`.game` to `g_user_root`/`g_game_root`.
  `localState` is the UTF-8 LocalFolder path from ApplicationData; `externalGame`
  must be an authorized filesystem path with code/content/meta. A StorageFolder
  token alone is not a CRT-readable path; permissions/capabilities belong to
  the host. Call `debug_watch_init()` before guest execution. The Apple
  `main.cpp` signal setup is unchanged and must not be compiled as a UWP host.
- CPU owner: replace the body of `rt_timebase()` in `cpu.cpp` with
  `return platform::guest_timebase();` and include `platform/platform.h`.
  The QPC implementation and portable 62,156,250 Hz conversion are ready; the
  existing Apple CPU clock remains unchanged until that integration.
- Cemu integration owner: replace `mach_absolute_time()` in
  `cemu_shims/cemu_misc_shims.cpp` with `platform::monotonic_ticks()` and ensure
  consumers use `monotonic_frequency()` (do not assume raw ticks are nanoseconds).
  GPU thread identifiers should use `platform::ThreadId/current_thread()`;
  the existing Apple overlay/window pair remains compatible with pthread_t.
- Audio/coreinit owners: `snd/ax_ist.cpp` still uses pthreads, and
  `coreinit/time_alarm.cpp` uses Apple/POSIX calendar helpers. Their remaining
  port is separate from the guest scheduler changed here. Renderer helper
  pthread/QoS calls and Metal-only clocks are also outside W1 ownership.
- Build owner: the root project still enables Objective-C and Apple dependencies.
  Platform/renderer/host selection is required before a full Windows build.

No game files or generated guest code are used by the tests.

## Verification

On a Mac:

```sh
cmake -S runtime/src/platform/tests -B /tmp/botw-w1-platform-tests -G Ninja
cmake --build /tmp/botw-w1-platform-tests
ctest --test-dir /tmp/botw-w1-platform-tests --output-on-failure
```

The harness checks LocalState/external roots, UTF-8 paths, mount boundaries,
traversal rejection, tick conversion against a 128-bit test oracle, demand-commit
bounds and execute/null-guard rejection, a real contiguous Apple allocation
including its last guest byte, guard/read-only faults in subprocesses, detached
thread launch, clock progression, and dynamic-symbol lookup. Runtime checks stay
enabled in release builds. No simulators, dev servers, or heavy builds are used.

Syntax checks: Apple Clang on all changed runtime units; Clang targeting
`x86_64-w64-windows-gnu` with installed MinGW headers, plus MinGW GCC with
`WINAPI_FAMILY=WINAPI_FAMILY_APP` on the Windows backend. The MSVC target cannot
consume this MinGW C++ library cleanly; a genuine Windows SDK/MSVC standard
library build and link still need Windows CI.

On Xbox: validate SDK imports/app certification, the explicit commit budget,
null guards, actual guest recursion/stack growth, scheduler/audio timing, QPC
pacing across suspend/resume, external-drive permissions and Unicode filenames,
LocalState saves, watcher diagnostics, and packaged-library loading. Full game
boot/rendering validation requires the owner's game dump outside the repo.
