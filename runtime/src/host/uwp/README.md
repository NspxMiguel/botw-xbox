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
- Native Direct2D pre-boot home with player-owned artwork; system MessageDialog settings/resume pages,
  folder picker, fps/volume/button-layout settings, a mods folder and persisted auto-launch setup.
  UI strings use the English/Portuguese table, selected by the system language. No CSS layer.
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
5. **Mods:** The launcher uses `mods::ModCatalog` for LocalState/mods and the compiled graphics
   plan. The pre-boot screen lists kind, enabled state and rebuild locks on a true-black surface
   with the home artwork's accent, 56px focus pills and scrolling. D-pad/arrows select; A/Enter
   toggles; X/LB and Y/RB move up/down; B/Escape saves and returns. Import and Save are focusable
   rows. In-game management uses native dialogs because the guest owns the swapchain.
   `enabled.txt` remains directly compatible with VFS auto-launch. Compiled shader/texture filtering
   is exposed for the runtime owner to wire; see `../../mods/README.md`.

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

## Player-owned launcher artwork

The pre-boot home screen reads `meta/bootTvTex.tga`, `meta/iconTex.tga` and
`meta/bootDrcTex.tga` from the **same detected game folder** selected by the launcher.
Reads use its brokered `StorageFolder`, including the remembered picker grant;
art loading does not rely on CRT access to an external path. Artwork is never
bundled, downloaded, copied into the repository or written to a cache. Each read
and decoded image is limited to 16 MiB. Missing, inaccessible or malformed images
are ignored independently; without a valid game/meta the home stays true black
with the title and a folder-selection action. DRC art is decoded and retained for
the future DRC presentation handoff; the TV hero uses only TV art.

The TV image fills the screen with a centered aspect-fill crop. A separate black
scrim has three stops (50% transparent, 82% at .82 opacity, 100% at .98). Copy sits
in its lower portion. Saturated hue buckets select the accent from the player's
art; neutral/transparent pixels do not dominate, and the accent is lifted until
its WCAG contrast on black reaches 4.5:1. Without a saturated hue it uses white.
The focused pill uses black text on this accent. Keyboard arrows/Tab/Enter,
pointer and controller D-pad/A navigate the disc and two icon actions. The disc
is the only play target: center (640, 360), radius 150 in a 1280×720 logical
surface. Settings and Mods are 48px circles at (1224, 56) and (1224, 120),
with a 32px edge margin. Up/Right from the disc focuses Settings, Down focuses
Mods, and Down/Left from Mods returns to the disc. Up from Settings stays there;
Menu opens Settings. Hover, taps, Enter and Space use the same targets. Only
the bottom-left title and missing-game/failure messages accompany home art.

Optional personal disc art lives in **LocalState/art/**, beside LocalState/game
and LocalState/mods. Create `art/disc.png` or `art/cover.png` there; priority is a
valid `disc.png`, then a valid `cover.png`, then the dump's `iconTex.tga`. WIC
decodes only PNG containers, with the same memory bounds. Overrides are read only
when a valid game is detected. Restart the launcher to reload personal artwork.
The disc is a circular centered crop with a transparent center hole and subtle
rings. During runtime preparation its angular velocity follows smoothstep from
zero to 2.5 revolutions/s over 1.2s, then stays constant. `disc_angle` integrates
velocity without wrapping, keeping position and velocity continuous. Only the
Direct2D drawing angle wraps. Windows `UISettings.AnimationsEnabled` is checked
each frame; disabled animations replace rotation with a slow accent-ring pulse.
Failed loads and leaving the home stop animation.

The launch cue is synthesized by portable `make_drive_spinup`: 1.6s of mono
48kHz float PCM, a 70–380Hz motor with harmonics, low-pass noise, three short
seek transients and amplitude fades. A separate XAudio2 engine/voice plays it
once at the saved user volume, independently of the paused game mixer. Buffer
storage outlives playback and is released after DestroyVoice on shutdown.

The home owns a Direct2D/D3D11 CoreWindow swapchain **only before guest boot**;
it releases all graphics resources before handing the window to W2. While a
live guest retains its swapchain, View+Menu continues to use the existing native
MessageDialog overlay for Resume/settings/mods. Showing the art home over an
already-running guest requires a renderer-owned shared presentation surface,
which is outside this directory's ownership. Graphics/device failures fall back
to the system dialog. The new header-only graphics code uses MSVC library
pragmas for d3d11, d2d1, dwrite and windowscodecs; non-MSVC Windows builds must
link those libraries explicitly.

`uwp_art_tests` uses exclusively synthetic byte arrays (no fixture images):
24/32-bit raw and RLE TGAs, all four origins, image IDs, alpha/unused attributes,
row-crossing packets, truncated prefixes, corrupt/oversize headers and packet
overflow, synthetic TV/icon dimensions, dominant hue selection, a 4,096-color
contrast sweep and loading/reduced-motion behavior. `check-syntax.sh` also checks
the new graphics/PNG headers and art tests with the real MinGW Windows headers.
The existing SDK limitation still applies: **app.cpp has not been compiled
against real C++/WinRT projections on this Mac**. Windows/Xbox validation must
check brokered meta reads, optional PNG decoding, resize/DPI/device loss,
controller/keyboard/pointer focus, reduced-motion toggling and renderer handoff.

## USB mod import

Import scans each removable drive's `mods/` folder (or drive root) for immediate
folders containing `content/` or `aoc/`. If no folder is copied, a folder picker
accepts a mod folder or a parent containing mod folders. All copy operations use
brokered StorageFolder/StorageFile APIs on a background thread, never a USB CRT
path. Copies go to LocalState/mods with unique names; installed mods and enabled.txt
are preserved. Incomplete copies are removed when an import fails. Imported mods
start disabled so the player can review and enable them. Errors offer a retry path.
The removable manifest permissions still filter accessible file types: use the
picker fallback when direct enumeration cannot see all files. Validate complete
translation/voice imports on Xbox with the real SDK and storage permissions.

The portable catalog tests exercise saved selection against the real VFS reader.
The existing syntax script compiles the updated Direct2D screen through
`art_view_syntax.cc`. Its existing limitation remains: app.cpp's C++/WinRT import
and coroutine code requires the Microsoft SDK and was reviewed, not compiled or
run on this Mac. On Xbox, verify empty/long lists, locked packs, USB disconnect,
name collisions, permission/space failures, controller release gating, and the
live-guest dialog fallback. No artwork assets are added by this feature.

## Online mods

The Mods list includes **Get mods** and **Add from link** in both system-selected
languages. GameBanana browsing uses the public v11 API for Wii U game 5866,
15-record index pages, search, and per-mod download files. LB/RB (PageUp/PageDown)
change pages. Records display name/category/author; download choices display
file size and explicitly disable unsupported formats. Text entry uses a native
CoreWindow dialog with CoreTextEditContext and the system InputPane, supporting
Xbox keyboard text updates, selection and Unicode. The live-guest fallback uses
MessageDialog because the renderer retains the CoreWindow surface.

HttpClient streams downloads into unique files in LocalState/mods/.downloads.
B/Escape cancels the current request/read. Known API and HTTP lengths must match;
MD5 is checked with Windows.Security.Cryptography.Core when supplied. Direct
links must use HTTPS and a .zip path (query strings are allowed). Progress reports
received and total KiB, or an unknown total. No remote art is cached.

Portable `mods::install_zip` accepts stored/deflate ZIPs with CRC checks, at most
20,000 entries and 4GiB total expanded size. It rejects encryption, ZIP64 size
sentinels, symlinks, absolute/traversal/drive/alternate-stream paths, duplicate
case-folded names and Windows reserved names. Extraction is streamed in 64KiB
chunks to an isolated staging directory. It searches through three nested levels
for content/ or aoc/, then rules.txt, flattens the selected root into a uniquely
reserved mod directory and removes staging/partial output on exceptions.
Downloaded graphics packs receive `.imported-graphics` and appear locked with
“Recompile to apply”. New file mods remain disabled. Existing installed folders
are never replaced. Compiled graphics behavior and enabled.txt remain compatible.

Validation adds `mods_online_tests` (handwritten JSON plus synthetic zlib-built
ZIPs) and `uwp_drive_sound_tests` (length, finite samples, peak, spectral centroid).
No network, game data or media fixtures are required. `check-syntax.sh` checks
portable/Direct2D/audio code on the Mac and explicitly reports the SDK gate for
app.cpp/mod_network.h. From a VS developer shell it also invokes cl.exe /Zs on
app.cpp and its WinRT headers. Set UWP_REQUIRE_SDK=1 to make a missing SDK fail.
This Mac cannot validate WinRT projections: Xbox/native-keyboard behavior,
HTTP/cancellation/MD5 and the native-dialog fallback require the Windows SDK
build and on-device checks. A portable or MinGW pass does not verify those APIs.
