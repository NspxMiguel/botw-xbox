# Remaster build and validation

The Series X Remaster selection compiles all matching patches from the official,
player-supplied Cemu packs. No pack category is removed to make it compile.
Game executables, packs, generated C++, snapshots and local build artifacts stay
private and are never committed.

## Active Series X selection

| Pack | Category | Resolved preset |
| --- | --- | --- |
| FPS++ | Mode | Normal Settings |
| FPS++ | FPS Limit | 60FPS Limit (Default) |
| Graphics | Aspect Ratio | 16:9 (Default) |
| Graphics | Resolution | 1280x720 (HD, Default) |
| Graphics | Anti-Aliasing | Normal FXAA (Default) |
| Graphics | Shadows | Extreme (400%, Unstable) |
| Graphics | Shadow Draw Distance | Very High (Recommended) |
| Enhancements | Clarity | t3nka Neutral+ |
| Enhancements | Depth Of Field | Enabled |
| Enhancements | Reflections | Normal Reflections |
| Enhancements | Reflection Range | Medium (8 samples) |
| Enhancements | Anisotropic Filtering | Ultra (8x, Default) |
| Draw Distance | NPC, Enemies And Other Entities | High (1.25x) |
| Draw Distance | Terrain, Buildings, Bushes And Other Objects | High (1.25x) |
| Draw Distance | Trees (2D Billboards) | High |
| Draw Distance | Grass Blades Density | High |
| Draw Distance | Texture Distance Detail (LOD) | Normal (Default) |

Unspecified categories resolve through the pack's defaults. In particular,
leaving anisotropy and LOD unspecified does not remove their unconditional rules.
No Workarounds are selected: those supplied target other renderer/vendor paths.
Native resolution keeps ordinary render targets under built-in FSR scaling;
shadow-map dimensions and the GUI precision upgrade still apply independently.
Upstream marks the selected shadow tier **Unstable**; compilation does not
establish its performance or stability.

## Compiler and runtime behavior

- Codecaves start at `0x01800000`, with a maximum of 1 MiB per pack and an
  8 MiB total window ending at `0x02000000`. Original section overlap is rejected
  before applying any writes. Constants and the hole below original text are
  excluded from function discovery.
- Absolute `ba`/`bla` into that window retain AA. Relative branches retain
  range/alignment checks. Assembly also handles byte `.align`, `@lo`/`@hi`,
  case-insensitive instructions/registers, implicit `cr0` floating comparisons,
  and condition-bit expressions.
- Cave translation units use `cave_*.cpp`, have nonnegative bucket indexes,
  participate in stale-source cleanup and are included by CMake's source glob.
- Dispatch keeps the original text lookup first and allocates a second table
  only through the highest cave entry. It does not allocate a table over the
  hole. Writable cave validation rejects any original section overlap.
- Windows/UWP's eager path commits everything except the null guard; its lazy
  path also commits the cave window up front, below `0x50000000`. Apple's
  read/write anonymous mapping covers the cave window outside the null guard.
  Neither platform needs a mapping change.
- FPS++ LR redirects resume at labels in their caller. Constant CTR thunks
  returning to instructions immediately after a displaced original branch run
  in the existing guest stack frame. The guest instruction bytes retain their
  original LK semantics. Unsupported conditional calls with LR redirects fail
  explicitly rather than generating an unchecked transfer.
- D3D12 applies LOD biases in signed 1/64 units and anisotropy as sampler
  exponents. It supports the Remaster RGB10A2 (`0x19`) to RGBA16 UNORM (`0x1f`)
  conversion, including resource/view format selection and CPU upload expansion.
  Other format conversions remain explicitly unsupported. CPU readback of
  alternate host formats retains the backend's existing unsupported restriction.

## Timing ownership

FPS++ owns guest frame deltas, physics scaling, cutscene limits and its guest
synchronization patches. The generator omits the independent timing/scaling
hooks from `runtime/src/game/fps.cpp`, preventing two implementations from
scaling the same frame. The per-frame hook remains after FPS++'s replacement
call to preserve UWP pause and CPU telemetry. Its host delta/scaling path is
explicitly disabled for a compiled timing pack. The snapshot records timing
ownership explicitly, including custom packs selected with `timing: true`.

Host `g_target_fps` controls presentation only; it does not reinstate omitted
physics hooks or change the compiled FPS++ limit. Pack changes require
recompilation. Launcher selection cannot undo compiled code writes.

## Validation

The requested Remaster recompile command exits successfully and emits the
normal generated sources plus `packs_generated.inc` and `packs_report.json`.
Its final analysis reports no unsupported instructions, undefined callees or
control-flow problems. The generated snapshot also compiles with the portable
runtime and passes D3D12 renderer-field validation.

`python3 -m pytest tools/packs/tests -q` passes with 35 tests and 17 subtests
(using the local Python environment). `sh runtime/src/packs/tests/run.sh`
passes under ASan/UBSan. Coverage includes alignment, relocation aliases,
absolute and backwards branches, cave bounds/overlap, data exclusion, bucket
cleanup, dispatch with and without caves, texture overrides, and native execution
of authored C++ generated from both continuation conventions.

The UWP build configuration was copied from the base worktree and all generated
text references were rewritten to this worktree, including terminal root paths.
Copied dependency and PCH caches were invalidated so headers from the base
worktree could not be reused accidentally. After waiting for the other Ninja,
`nice ninja -C build/local-uwp-game -j4` exited 0 and linked
`build/local-uwp-game/BotWRecomp.exe` as a PE32+ x86-64 Windows executable.
The updated renderer, dispatch, FPS and loader sources were all rebuilt. The
private build log is `generated/uwp-build.log`. The compiler reported the existing
unused `/await:strict` option warning; it did not prevent the build.

## Console verification still required

- Gameplay pacing at the compiled 60 fps limit, including low frame rates,
  physics, arrows, stamina, ragdolls, menus and camera scrolling.
- Cutscene transitions and 30 fps limits, guest fence/vsync continuations, host
  presentation choices, pause/resume and frame-clock reporting.
- All replacement shaders on the Xbox GPU, shadow stability and memory cost,
  FSR with per-texture overrides, anisotropic filtering and LOD quality.
- GUI precision conversion through uploads, render targets, views and copies;
  confirm that affected surfaces never require unsupported alternate-format
  CPU readback.

Synthetic execution and a linked UWP executable do not establish these device
results.

## Changed files

- `CMakeLists.txt`
- `cafe/analyze.py`
- `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12PackHooks.h`
- `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.cpp`
- `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.cpp`
- `runtime/src/dispatch.cpp`
- `runtime/src/game/fps.cpp`
- `runtime/src/mem.h`
- `runtime/src/packs/README.md`
- `runtime/src/packs/host_install.h`
- `runtime/src/packs/loaded_rpx.cpp`
- `runtime/src/packs/packs.cpp`
- `runtime/src/packs/packs.h`
- `runtime/src/packs/tests/dispatch_test.cc`
- `runtime/src/packs/tests/hooks_test.cc`
- `runtime/src/packs/tests/run.sh`
- `tools/packs/README.md`
- `tools/packs/build.py`
- `tools/packs/patches.py`
- `tools/packs/preset.py`
- `tools/packs/recompile.py`
- `tools/packs/rules.py`
- `tools/packs/tests/test_packs.py`
- `tools/packs/tests/test_preset.py`
- `tools/recompile.py`
- `docs/remaster-validation.md`
