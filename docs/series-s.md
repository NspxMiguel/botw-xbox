# Automatic Series S settings

The UWP host queries `GetGamingDeviceModelInformation` once and logs its HRESULT,
vendor/device IDs and resolved model. It recognizes retail Series S, retail
Series X and Series X dev kits using the public Windows SDK constants. It also
logs `AnalyticsInfo` family/form strings. An older/emulated Microsoft console or
an Xbox family string without an identified model receives the conservative
Series S policy. Simulation changes the policy without changing the logged
hardware identity.

Microsoft documents the gaming-device API for both Win32 and UWP and notes that
it can return the **emulated** model:
[API reference](https://learn.microsoft.com/en-us/windows/win32/api/gamingdeviceinformation/nf-gamingdeviceinformation-getgamingdevicemodelinformation).
The analytics family string is only a best-effort fallback; Microsoft warns
that its values can change in the
[DeviceFamily reference](https://learn.microsoft.com/en-us/uwp/api/windows.system.profile.analyticsversioninfo.devicefamily).
If both queries fail, the host cannot identify an
Xbox and retains the desktop profile. Inspect the query logs during validation.

| Automatic setting               | Series S / unidentified Xbox                     | Series X            |
| ------------------------------- | ------------------------------------------------ | ------------------- |
| Native internal resolution cap  | 1600 x 900 (1.25x)                               | 2560 x 1440 (2x)    |
| Output buffer cap               | 2560 x 1440                                      | 3840 x 2160         |
| HDMI preference                 | Highest supported SDR mode within cap at >=59 Hz | Same, within 4K cap |
| FSR / frame generation defaults | Off / Off                                        | Off / Off           |

At 720p output the automatic internal size remains 1280 x 720. Manual scale
preferences still win, up to the existing Series S 1080p manual cap. FSR remains
an explicit spatial-upscaling opt-in. The automatic 900p cap applies to native
rendering; explicit FSR quality choices retain their own existing ratios.
Output buffers are capped even if HDMI mode selection is unavailable/rejected;
the OS may then scale them to a higher-resolution physical signal. No guarantee
of a 1440p HDMI signal is made when the mode request fails.

The [official Series S specifications](https://www.xbox.com/en-US/consoles/xbox-series-s)
list 10 GB GDDR6 and a 4 TFLOPS GPU. **Assumption:** 900p is a prudent starting
point for this port, not a measured performance result or a 60 fps guarantee.
Relative to 1080p, 900p has about 31% fewer pixels per internal surface. Capping
the output at 1440p also avoids 4K swap-chain/compositor targets. Actual savings
depend on surface lifetimes, formats and opaque driver allocations.

## Process memory

Microsoft's [UWP Xbox resource allocation documentation](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
applies to Series X and S and lists foreground **Games: 5 GB**, **Apps: 1 GB**.
It does not promise access to all 10 GB, or a larger Series S Developer Mode
budget. The [Developer Mode compatibility guidance](https://blogs.windows.com/windowsdeveloper/2020/09/17/app-compatibility-for-xbox-series-x-and-xbox-series-s-consoles/)
describes parity with Xbox One UWP. Native GDK memory entitlements do not apply.

**Assumption:** the project's existing 5120 MiB game ceiling is usable on Series
S when deployed as a Game, just as expected on Series X. This is unverified on
Series S. The live `MemoryManager::AppMemoryUsageLimit()` value, in bytes, always
wins if smaller; a larger/debugger limit is capped at 5 GiB by the startup check.
The [AppMemoryUsageLimit reference](https://learn.microsoft.com/en-us/uwp/api/windows.system.memorymanager.appmemoryusagelimit)
defines this value in bytes.
Game classification is a deployment prerequisite, not a player graphics setting;
the runtime cannot turn a 1 GB App allowance into a 5 GiB Game allowance.

The current Windows backend reserves 4 GiB plus a 64 KiB tail, leaving the first
64 KiB inaccessible. With its existing demand-commit handler it initially
commits `[0x10000, 0x50000000)` and `[0xA0000000, 0x100010000)`: **2816 MiB**.
The reserved gap can later add **1280 MiB**. If the existing handler lookup fails,
the backend initially commits **4096 MiB**. That handler mechanism is pre-existing
and is not a supported UWP API guarantee; this change does not expand it.

Before either commit path, the backend now requires:

```text
current usage + planned initial guest commit + 256 MiB
    <= min(live process limit, 5120 MiB)
```

The reserve is an **assumed minimum startup margin**, not a measured renderer
budget. Insufficient/missing budget readings fail startup with a diagnostic
before attempting the guest commit. The existing periodic process-memory log
continues during gameplay. This admission check does not bound future guest,
CPU, texture-cache or driver growth and is not an OOM prevention system. The
renderer pressure signal currently reacts to allocation failures, rather than
continuously tracking this process budget. Long-session console measurement is
still required.

## Validation still required on hardware

Host tests cover model-to-policy selection, simulation, 720p/1080p/1440p/4K
inputs, output caps and mode fallback, native scale defaults, FSR opt-in, and
memory arithmetic at exact boundaries for both commit paths. A cross-build
checks the real SDK/API declarations and import resolution. These checks cannot
establish that a particular console reports the expected model or grants the
expected process limit.

On a Series S, a human must verify a fresh launch with automatic settings and
no simulation, the logged IDs, 900p internal rendering, HDMI mode negotiation
on 1080p/1440p/4K displays, rejected-mode fallback, FSR off, and suspend/resume.
Measure process peaks and frame times during cold precompile, loading, traversal,
combat and repeated area transitions, including a long session. Require at least
256 MiB below the **live** limit at peak; reduce defaults further if needed.
Check App-mode rejection and a normal Game-mode launch outside a debugger, then
verify manual scale and FSR opt-in still work. Series X must retain its current
native defaults and 4K output. No Series S runtime result is claimed here.
