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
- Native Direct2D pre-boot home and first-run setup with player-owned artwork; system MessageDialog
  settings/resume pages, fps/volume/button-layout settings and a mods folder. UI strings use the
  English/Portuguese table, selected by the system language. No CSS layer.
- LocalSettings preferences plus `setup_done.txt`, `usb_mode.txt` and a relative `game_source.txt` in
  LocalState. Removable discovery enumerates through three folder levels and explicitly probes common game
  roots, never storing drive letters. A system folder picker persists a FutureAccessList grant
  as a fallback. Complete sources take priority, and a single valid source advances automatically.
  Identical scan results preserve focus and scroll; drive/candidate verdicts are flushed to runtime.log. Scanning, extraction,
  verification and RPX loading run off the UI thread. Missing sources return to setup detection.
- Manifest, original geometric package icons and Visual Studio deployment properties. The manifest
  Publisher is a development placeholder: packaging must use the owner's package identity/certificate.

Settings → Wallpaper selects From your game (the player's `meta/bootTvTex.tga`), Plateau,
Shrine, or the original Light background. LocalSettings `wallpaper` stores 0=Light,
1=Plateau, 2=Shrine, 3=From your game. The default prefers dump art, otherwise Plateau;
failed image loads use Light. Changes repaint immediately, with cover-fit images,
a readability scrim and light ink. Only the two supplied landscape JPGs are packaged.

## Controls

| Xbox | Wii U GamePad (default positions) |
| --- | --- |
| A / B / X / Y | B / A / Y / X |
| D-pad, sticks, stick clicks | Same position; radial 18% stick deadzone, positive Y up |
| LB / RB | L / R |
| LT / RT above 50% | ZL / ZR |
| View / Menu | Minus / Plus |
| Hold View + Menu for 0.5 s | Quick menu; Resume preserves the existing guest |
| Hold View for 0.5 s / View + Y | Toggle TV / DRC GamePad full screen |
| View + X | Tap the GamePad screen center |
| View + R3 | Toggle motion mode: right stick tilts, LT/RT turn, L3 recenters |

The labels layout maps A/B/X/Y by letter instead. Xbox's system/Home button is not intercepted.
The quick menu selects GamePad screen Off (default), Corner or Swap. Screen selection and
synthetic touch are separate actions. Motion mode supplies three-axis virtual GamePad motion and
shows an EN/PT controller reminder; normal stick aiming remains available outside it. See
[the display/input path and console acceptance checklist](../../../../docs/gamepad-screen.md).
`pro_buttons()` also converts every physical button to the Wii U Pro Controller ABI, independently tested. The existing shared padscore HLE still reports no controller;
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
   `gamepad_screen.h`: D3D12 composes the retained TV/DRC images according to the atomic screen mode.
   `host.h` exposes lifecycle/presentation callbacks. Metal uses its existing display path.
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
  while holding inputs, Resume, all GamePad display modes, View+X touch and View+R3 motion puzzles.
- Audio stereo/5.1 channel order, clipping/volume, starvation/queue pacing, HDMI/headset routing and
  device loss. Voice errors safely mute output; automatic XAudio2 device recreation is not implemented.
- Suspend/guide/dashboard/visibility transitions and resize/DPI with W1/W2 quiescing installed;
  verify guest/GPU lifecycle, memory, frame pacing and long-run behavior on Series S and Series X.

## Player-owned launcher artwork

The pre-boot home screen reads `meta/bootTvTex.tga`, `meta/iconTex.tga` and
`meta/bootDrcTex.tga` from the **same detected game folder** selected by the launcher.
Reads use its brokered `StorageFolder`, including the remembered picker grant;
art loading does not rely on CRT access to an external path. Artwork is never bundled or copied
into the repository. The launcher optionally downloads disc art from GameTDB into a per-title
LocalState cache. Each read and decoded image is limited to 16 MiB. Missing, inaccessible or malformed images
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
surface. Settings and Mods are 48px circles at (56, 56) and (56, 120).
Up/Left from the disc focuses Settings; Down from Settings focuses Mods;
Down/Right from Mods returns to the disc. Menu opens Settings.

`meta/meta.xml` product and company codes form the six-character GameTDB ID.
Regions E/P/J map to US/EN/JA. HTTPS requests try `wiiu/discM`, then `wiiu/disc`;
PNG decoding and downloads are bounded to 16 MiB. Successful downloads are cached
as `LocalState/art/disc.png` with `disc-id.txt` preventing stale cross-title art.
Missing metadata, network failure or bad PNGs retain the dump's `iconTex.tga`.

The Direct2D 1.1 device context draws a transparent-holed disc plane through the
3D perspective effect. Right-stick pitch/yaw use damped springs (10 rad/s,
0.45 damping ratio), capped at 22 degrees. A cos^16 specular band follows tilt;
the focus ring breathes over 2.4 seconds with a pre-blurred procedural halo.
Icon focus scales spring toward 1.18, with a 50 ms tick and a low 40 ms rumble.

A starts the press and drive cue immediately, accelerates rotation to 2.2 rev/s,
adds camera depth, and fades the scene over one second. Loading uses only the
66px metallic gold triforce centered at (1200, 640): gradient faces, bevel edges,
a three-second diagonal sweep, 1.85-second brightness pulse and Gaussian glow.
The missing-game disc has faint grooves and a path-drawn question hook whose dot
is its transparent hole. A flips it through 180 degrees over two seconds and
zooms to silver over 3.5 seconds, blending into the manual setup background.
The setup uses dark text, white pills and a dark focused pill.
Windows `UISettings.AnimationsEnabled` disables tilt, spin and focus popping.
Portable physics, material masks, geometry, metadata and audio loop/fade behavior
are covered by `tests/polish_tests.cc`. The launcher releases its CoreWindow swapchain before starting the D3D12
renderer; a running game still uses the system menu overlay. `check-syntax.sh`
checks app.cpp and launcher.cpp with clang-cl /Zs and the xwin SDK when installed.
Device appearance, 60 fps timing and WinRT
network/audio/controller behavior still require an Xbox run.

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
`art_view_syntax.cc`. The xwin clang-cl gate now syntax-checks app.cpp and its C++/WinRT import
and coroutine code; execution still requires Windows/Xbox. On Xbox, verify empty/long lists, locked packs, USB disconnect,
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
The xwin SDK gate checks WinRT syntax on this Mac. Xbox/native-keyboard behavior,
HTTP/cancellation/MD5 and the native-dialog fallback require the Windows SDK
build and on-device checks. A portable or MinGW pass does not verify those APIs.

## Interactive first-run setup (W10)

`setup.h` owns portable transitions, XML title/version parsing, capacity arithmetic, discovery
predicates and the 400/120 ms directional repeat policy. `setup_storage.h` adapts it to brokered
StorageFolder operations. `app.cpp` renders setup through ArtView on the UI dispatcher; workers
publish only atomic progress/cancellation state. Screen text and button hints are in `strings.h`.
The four-page guide describes using files the player has already extracted and decrypted from their
own Wii U, the required 1.5.0 (v208) update, optional DLC, merging and USB preparation. It contains no
external guide links or QR. The generic MIT Nayuki QR implementation remains available in
`qr.h`/`qr.inc`, with a neutral URL fixture in the host tests.

Portable readers in `formats/` implement already decrypted ZArchive v1 WUA with length-prefixed
names, 64 KiB zstd blocks and archive integrity, plus streamed stored/deflated ZIP with CRC and path
checks. ZIP folders are merged on Xbox. `.7z` and archives containing `.wua` are recognized and must
be extracted on a PC first. Only decrypted folders and already decrypted archives are accepted.
Split decrypted folder title sets are paired by title ID and highest update version, then merged as
logical entries (base < update; DLC content -> aoc/) before materializing on the source USB.
The numeric identifiers of retained formats are stable so existing extraction journals remain valid.

Extraction reserves BotW/ (or a unique generated folder when BotW already exists), without
replacing player files. A source ownership marker and a relative LocalState location let it resume.
It writes brokered final filenames on the USB, with completed-entry SHA-256 records in a
LocalState append journal. Resume rehashes finished files; source size/timestamps identify the job.
Unfinished generated folders are excluded from discovery until all files and the v208 RPX pass.
No USB rename or USB temporary file is needed. Internal import reserves a unique staging folder,
checks live free space plus a 2 GiB margin, verifies the copied RPX, and publishes with a brokered
LocalState folder rename; cancellation/failure deletes staging. An existing LocalState/game is
preserved. Removal is limited to copies bearing the LocalState import ownership marker.

The shared incremental `packs/sha256.h` is the original packs SHA-256 moved out of packs.cpp;
existing `packs::sha256` forwards to it. Setup streams the broker-granted RPX handle when available,
falling back to IRandomAccessStream/DataReader. Only a verified v208 reaches the storage/automatic
steps. External launches also revalidate before the irreversible runtime boot attempt.

Additional Mac validation: `uwp_setup_tests` uses only synthesized bytes and temporary fixture
trees, including streaming SHA-256, compressed WUA, ZIP corruption/traversal, title layering,
QR module payload decoding, setup transitions and capacity boundaries. `check-syntax.sh` checks these headers with AppleClang
and real MinGW Direct2D headers. It explicitly skips **app.cpp, setup_storage.h and
mod_network.h** without Microsoft SDK/C++/WinRT; its cl.exe path includes them. Portable passes do
not establish Xbox broker permissions, hardware UX, hot-plug timing or a successful Windows package.
On-device validation must cover those paths, actual dump content extensions, low/unknown disk space,
long copies, cancellation, resumed extraction, wrong version, and the renderer handoff.

### Drive recording

`Assets/sfx/drive.wav`: [Disc Drive Open & Spinning](https://freesound.org/people/sammerz_/sounds/843008/) by sammerz_, Freesound 843008, CC0. Converted to 48 kHz mono PCM16 and trimmed by 350 ms. Playback crossfades the final 100 ms into the steady region starting at original 9 s; it loops until the renderer's first successful present, then fades out over 250 ms. `drive_sound.h` supplies the synthesized fallback if the package asset cannot be decoded. No game assets are bundled.
