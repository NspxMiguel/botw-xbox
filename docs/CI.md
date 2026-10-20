# Continuous integration

CI builds the runtime and launcher only. It never receives an RPX, a game dump, keys, or
`generated/`, and never invokes `tools/recompile.py`. Do not add game inputs to Actions secrets,
caches or artifacts. A successful CI package opens the launcher; it cannot run the game by itself.
The workflows run on pushes, pull requests and manual dispatches, with read-only repository access.

The checked-in configure/build presets are:

| Preset     | Host/toolchain                                    | Platform | Renderer |
| ---------- | ------------------------------------------------- | -------- | -------- |
| `ci-macos` | Apple Silicon macOS, Ninja/AppleClang             | `apple`  | `metal`  |
| `ci-uwp`   | Windows, Visual Studio 2022 MSVC x64, Windows SDK | `uwp`    | `d3d12`  |

All three set `BOTW_BUILD_GAME=OFF`, build the real `BotWRecomp` runtime/launcher target in Release,
and limit compilation to two parallel jobs. On a suitable host, use:

```sh
python .github/workflows/ci-build.py check-contract
cmake --preset ci-macos
cmake --build --preset ci-macos
```

Substitute `ci-uwp` on Windows. Windows presets use the vcpkg toolchain under
`VCPKG_INSTALLATION_ROOT` and the `x64-windows` or `x64-uwp` triplet. The workflows list the native
dependencies explicitly; UWP shader translator tools use the separate `x64-windows` host triplet.
CMake integration must discover these host tools, including DXC. The renderer owner must supply
UWP-compatible runtime shader dependencies; the desktop DXC package is a host tool, not a UWP DLL.

### CMake contract and remaining integration

The top-level cache options are:

- `BOTW_BUILD_GAME` (boolean, default `ON`): `OFF` omits the guest object library and all
  `generated/` inputs. The actual runtime, HLE, platform layer, GPU and launcher still compile
  and link. Guest dispatch tables and RPX loading are compiled only into gameplay builds;
  a runtime-only build rejects game execution. It never synthesizes guest functions or tables.
- `BOTW_PLATFORM` (`apple`, `uwp`; `win32` has no host yet): defaults to Apple on macOS and UWP
  with the WindowsStore toolchain.
- `BOTW_RENDERER` (`metal`, `d3d12`): defaults to Metal for Apple and D3D12 for Windows/UWP.
  Unsupported platform/renderer combinations are rejected at configure time.

`BotWRecomp` remains the executable target. Apple builds enable Objective-C, SDL3 and Apple
frameworks only on Apple. The macOS runtime-only entry opens the existing settings menu without
loading an RPX; `--headless` performs a game-free startup check and exits.

For UWP, `cmake --install build/ci-uwp --config Release --component Runtime --prefix <stage>`
installs `BotWRecomp.exe`, the original host manifest renamed to `AppxManifest.xml`, its four
`Assets/` images and CMake-discovered runtime DLL dependencies. The manifest declares x64,
an explicit Publisher, the Universal device family and a UWP entry point. The package script
checks the staged executable and rejects unresolved paths or desktop full-trust entry points.

Windows/D3D12 renderer and runtime portability work remains owned by the platform and renderer
integrations. Adding presets and install rules does not supply that implementation. The Windows
jobs surface those compile/link failures until those integrations land; macOS validation does
not establish a working Xbox package.

The preflight rejects game inputs and reports missing CMake declarations. Subsequent configure,
compile, install, manifest, pack and signing errors fail the job; nothing is silently skipped.
Declaring options alone does not establish a working port: all three builds must compile/link,
and UWP staging and SDK package validation must pass before an artifact is uploaded.

### UWP test artifact

`build-uwp.yml` follows NXbox's Windows build/staging/package/artifact pattern from
`build-nxbox.yml` and `package-nxbox.yml`, but builds and packages the same checkout in one job.
Windows jobs use `windows-2022` to match the Visual Studio 2022 preset generator.
It installs `Runtime` into `build/ci-uwp/package-root`, audits the payload, and uses the newest
complete installed x64 Windows SDK tool set to run MakeAppx and SignTool. Unlike NXbox's release
certificate secrets, this job generates a disposable RSA/SHA256 code-signing certificate whose
subject matches the manifest Publisher. It verifies the signature on the runner and deletes the
private key afterward. No certificate password or long-lived signing secret is required.

The seven-day artifact contains only the signed `.appx`, its public `.cer`, and `build-info.json`
(revision, package hash, SDK version and certificate expiry). The certificate expires after seven
days and changes each run: these are sideloading test packages, not stable releases or automatic
updates. Trust the matching public certificate on the test deployment device as required by its
deployment tools. The private key is never exported or uploaded. The packaging rules follow
[Microsoft's Appx signing requirements](https://learn.microsoft.com/en-us/windows/win32/appxpkg/how-to-sign-a-package-using-signtool).

### Player-owned game build

Static recompilation happens **on the player's computer**, using the player's own supported Wii U
dump (USA v208/1.6.0; see README for the hash and dumping instructions):

```sh
uv run python -I tools/recompile.py /path/to/game/code/U-King.rpx generated
```

The generated C++ stays on that computer. The player then configures a separate local build with
`BOTW_BUILD_GAME=ON`, compiles the generated guest code with the runtime, and signs/packages the
result locally for their device. The default ON path retains the guest object library; the existing upstream Mac build in README
remains the reference for local gameplay.
The CI presets are specifically for game-free builds. Do not use their `ci-*` output directories
for local game builds, or upload a locally built gameplay executable/package to CI.

For Xbox, recompilation cannot happen as executable code generation inside the UWP app: after
generating C++ on their computer the player must locally compile/package the UWP gameplay build
with MSVC and sign it with their own deployment certificate. Copy the legally dumped game data
to the location supported by the launcher (LocalState or external storage) separately. Game
selection on the console supplies data; it does not turn the CI runtime-only Appx into a gameplay
build. The local UWP guest-link and packaging path remains pending until the platform, renderer,
host and CMake integrations above are complete. Neither generated C++ nor the resulting game-bearing
binary/package belongs in this repository or its Actions artifacts.
