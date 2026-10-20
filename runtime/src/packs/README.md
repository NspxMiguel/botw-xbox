# Host and renderer installation

Both hosts load through `load_rpx` before OS initialization and call
`install_loaded` after relocation and renderer initialization, before `start_main`.
An empty compiled plan loads the original RPX directly and installs nothing.
For nonempty plans, an exclusive temporary file in the user directory contains
exactly the bytes hashed and passed to the unchanged path-based loader. It is
removed after loading. Snapshot failure disables packs and loads the original.
Install failures are logged and guest startup continues, as required by W7.

Writable range validation uses loaded ELF section flags, loader module bounds
and the `mem.h` address layout. Executable sections are writable guest data in
this static recompilation. Read-only sections, holes, runtime areas and address
wraparound are rejected. Appended codecaves are bounded to 1 MiB per selected
pack below data imports and cannot overlap original allocated sections.

D3D12 validates all fields before compiling replacement shaders on the CPU.
Commit publishes the static plan. Shader lookup uses original hashes/stage;
texture lookup matches original guest descriptions. Width/height/depth overrides
update allocation, effective sizes and mip bounds. The shared texture machinery
uses effective sizes for views, viewport and scissor scaling. Accepted texture
plans disable built-in scaling before any guest textures are created. Format,
LOD and anisotropy overrides are currently rejected explicitly. Shader and PSO
persistent caches are bypassed for pack sessions to prevent reuse across pack
fingerprints; sessions without packs retain their existing cache behavior.

The small Metal adapter supports memory-only and advisory timing plans. It
rejects texture/shader replacements because Metal exposes no publication hook
outside its owned subtree. No files under `Renderer/Metal/` are changed.

UWP selects raw HDMI output dimensions where available, falling back to current
CoreWindow bounds times RawPixelsPerViewPixel. The same dimensions determine
render scale and renderer/swap-chain creation. The renderer clamps scale to
[1, 3], unless an accepted texture pack supplies resolution itself.

The recomp D3D12 present path always synchronizes: interval 1 for the 60 fps
setting, 2 for 30 fps on the game's 60 Hz clock; frame interpolation is disabled.
FPS++ names and `vsyncFrequency` controls trigger one host message and do not
create a second host pacer. `g_target_fps` remains the host authority. Note that
`tools/packs/recompile.py` currently omits `game/fps.cpp` hooks when an external
timing pack is compiled. This host change cannot restore hooks omitted from
already generated code; the generator is outside W7's allowed paths.

## Synthetic verification

```sh
sh runtime/src/packs/tests/run.sh
python3 -m pytest tools/packs/tests -q
sh runtime/src/host/uwp/tests/check-syntax.sh
```

The new C++ test uses ASan/UBSan, four SHA-256 vectors, a fake guest address
space, actual portable D3D12/Metal hook validation, and authored ELF section
metadata. It verifies snapshot/hash identity even if the source changes, snapshot
cleanup, range rejection and empty-plan/failure fallbacks. No game assets are
needed. See the D3D12 `validation/syntax_check.py` for the Windows-target check;
that controlled header check does not run a Windows GPU or validate the WinRT
HDMI branch. Full app linking needs private `generated/` sources. No Xbox/device
rendering or real FPS++ gameplay validation is claimed.
