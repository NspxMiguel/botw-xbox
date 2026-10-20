# W3: UWP CoreWindow host

This directory owns the Xbox/UWP host. It contains no game files or generated guest code.
Only this directory and the marked W3 blocks in the root CMakeLists.txt are changed.

## Implemented

- C++/WinRT `IFrameworkViewSource` / `IFrameworkView`, `CoreApplication::Run`, a CoreWindow dispatcher
  pump, DPI/resize handling, visibility, activation, suspension/resume and event cleanup.
- Windows.Gaming.Input Gamepad polling and 60 Hz rumble. Disconnection, deactivation and menu entry
  clear input and motors. Shortcuts latch until all buttons are released.
- XAudio2 output implementing the existing `audio/output.h` API: PCM16, 48 kHz, mono/stereo or
  L/R/C/LFE/Ls/Rs downmixed to stereo with the upstream coefficients, clipping and volume. Sixteen
  fixed buffers bound memory and queue latency; buffers remain alive through OnBufferEnd. No callback
  allocation, callback locks, or SDL dependency. Suspended/menu audio stops and incoming mixes drop.
- Native UWP MessageDialog launcher pages (system theme, controller focus and accessibility), folder
  picker, fps/volume/button-layout settings, a mods folder and persisted auto-launch setup. UI strings
  use the small English/Portuguese table, selected by the system language. No custom CSS/theme layer.
- LocalSettings persistence and FutureAccessList token `game`; each launch revalidates code/U-King.rpx,
  content/ and meta/. Priority is LocalState/game, LocalState itself, remembered folder, removable
  devices. Removable roots checked: game/, BotW/, BotWRecomp/game/, and the device root. Scanning and
  RPX loading run off the UI thread. Missing/stale/denied folders do not force auto-launch.
- Manifest, original geometric package icons and Visual Studio deployment properties. The manifest
  Publisher is a development placeholder: packaging must use the owner's package identity/certificate.

## Controls

| Xbox | Wii U GamePad (default positions) |
| --- | --- |
| A / B / X / Y | B / A / Y / X |
| D-pad, sticks, stick clicks | Same position; radial 18% stick deadzone, positive Y up |
| LB / RB | L / R |
| LT / RT above 50% | ZL / ZR |
| View / Menu | Minus / Plus |
| View + Menu (Select + Start) | Return to launcher; Resume preserves the existing guest |
| View + Y | Toggle TV / DRC GamePad scanout request |

The labels layout maps A/B/X/Y by letter instead. Xbox's system/Home button is not intercepted.
There is no gyro or touchscreen emulation. DRC display selection is an explicit renderer handoff,
not synthetic screen taps. `pro_buttons()` also converts every physical button to the Wii U Pro
Controller ABI, independently tested. The existing shared padscore HLE still reports no controller;
announcing a Pro Controller needs a change to `runtime/src/input/pad_hle.cpp`, outside W3 ownership.

## Integration boundaries (required before a playable Xbox package)

1. **Build/platform owner:** the marked CMake blocks select the replacement host/audio/settings/main
   and exclude W3 from the Apple executable. The root build is still the upstream Apple build around
   those blocks: its unconditional Objective-C languages, Apple flags/frameworks, SDL dependency,
   Cemu Metal sources and guest flags must be gated by the platform build owner. Compile this host
   with C++20, C++/WinRT SDK projections, `BOTW_PLATFORM_UWP=1`, `NOMINMAX`, the UWP API family and
   `windowsapp`/`xaudio2`. Never compile original main.cpp, settings.cpp, host/window.mm,
   host/menu.cpp, host/touch.cpp or audio/output.cpp alongside these replacements. Tests use `.cc`
   so the untouched iOS recursive `.cpp` glob cannot import a second main. The compatibility
   TargetConditionals.h include directory is UWP-only.
2. **W2 renderer:** `latte::create_renderer(void*, width, height)` receives a borrowed **CoreWindow
   IUnknown**, suitable for CreateSwapChainForCoreWindow, never an HWND/NSView/SDL_Window. Consult
   `host.h`: poll `gamepad_screen_requested()` to select DRC scanout, and `presentation_allowed()`
   before presenting. This directory does not implement a D3D12 renderer or modify Metal.
3. **W1 filesystem:** LocalState paths work without a broker. A FutureAccessList picker token is
   persisted, but its Path string alone does **not** guarantee CRT/Win32 filesystem access. External
   game content needs W1's brokered StorageFolder/FromApp access or a complete import to LocalState.
   `removableStorage` is filtered by declared file associations; the `.rpx` declaration supports
   detection, not blanket access to all game-content extensions. No wildcard file association or
   broadFileSystemAccess is claimed. The latter is unsupported on Xbox. Until this integration is
   complete, use LocalState/game for gameplay, and validate external discovery/picker grants separately.
4. **Lifecycle:** W1/W2 can install a nonblocking visibility callback to quiesce guest/GPU work when
   hidden/suspended; the host itself stops input/audio and exposes presentation state. Menu entry
   preserves the live guest and suppresses input/audio; it does not suspend every guest thread.
   A cooperative whole-runtime pause requires scheduler/renderer ownership. No unsafe thread killing
   or second guest boot is attempted. A failed RPX load requires restarting the app before another
   guest boot, because guest-memory initialization is not reversible.
5. **Mods:** LocalState/mods is created and its path shown. Applying mod files is the runtime/mod
   loader's responsibility; no arbitrary DLL loading or unimplemented mod-enable control is exposed.

File access reference: [Microsoft UWP file permissions](https://learn.microsoft.com/en-us/windows/uwp/files/file-access-permissions).
Button ABI references: [Windows.Gaming.Input](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input.gamepadbuttons)
and [devkitPro wut WPAD](https://github.com/devkitPro/wut/blob/master/include/padscore/wpad.h).

## Local validation

```sh
cmake -S runtime/src/host/uwp/tests -B /tmp/botw-w3-tests -G Ninja
cmake --build /tmp/botw-w3-tests
ctest --test-dir /tmp/botw-w3-tests --output-on-failure
runtime/src/host/uwp/tests/check-syntax.sh
```

The standalone tests need no game, generated sources, SDL, renderer or Windows SDK. They cover all
launcher setup/auto-launch states, invalid/missing folders, source priority, every button, both
layouts, analog trigger thresholds, edge-triggered consumed host shortcuts, disconnect/reconnect
release gating, invalid floating-point input, a 40,401-position stick sweep and both string tables.

Verified on the Mac: portable tests with AppleClang; runtime/settings syntax with AppleClang and
UWP-only compatibility header; audio/controller/launcher syntax with LLVM clang targeting Windows
x86-64 GNU ABI and real MinGW Windows/XAudio2 headers; audio syntax also with cross GCC.
The MSVC target was attempted with MinGW headers, but MinGW's standard library is incompatible with
that target. No Microsoft SDK/C++/WinRT projection headers are installed, so app.cpp has been
reviewed by inspection, **not** compiled against real WinRT projections. Downloading the NuGet
C++/WinRT package was blocked by this session's network DNS restrictions. No Windows execution or
full Apple/game build was performed. Full builds need generated guest code, absent by design.

## On-device acceptance

- Build/link with the real Windows SDK and merged W1/W2 implementations; MakeAppx schema/signing
  validation, package identity, installation in Xbox Developer Mode and native dialog focus/navigation.
- LocalState valid/missing/partial game, initial setup yes/no, second launch, removed USB, stale picker
  token, denied storage and hot-plug. Prove complete external content reads through W1, not just RPX
  detection. Confirm folder picker availability on the target Xbox OS; LocalState remains the fallback.
- Both face-button layouts, trigger thresholds, disconnect with held buttons, reconnect, View+Menu
  while holding inputs, Resume, View+Y DRC display and any game screens that need actual touch.
- Audio stereo/5.1 channel order, clipping/volume, starvation/queue pacing, HDMI/headset routing and
  device loss. Voice errors safely mute output; automatic XAudio2 device recreation is not implemented.
- Suspend/guide/dashboard/visibility transitions and resize/DPI with W1/W2 quiescing installed;
  verify guest/GPU lifecycle, memory, frame pacing and long-run behavior on Series S and Series X.
