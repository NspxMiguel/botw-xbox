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
wraparound are rejected. Codecaves occupy 0x01800000–0x02000000, bounded to 1 MiB per selected
pack and 8 MiB total, and cannot overlap original allocated sections. Dispatch
uses a separate table sized only through the highest cave function; ordinary
text keeps its existing fast lookup. Windows/UWP commits this area even in
the lazy-commit path; Apple maps it read/write outside the null guard.

D3D12 validates all fields before compiling replacement shaders on the CPU.
Commit publishes the static plan. Shader lookup uses original hashes/stage;
texture lookup matches original guest descriptions. Width/height/depth overrides
update allocation, effective sizes and mip bounds. The shared texture machinery
uses effective sizes for views, viewport and scissor scaling. The build drops
identity dimensions when exact filters prove there is no earlier overlapping
size override to reset. Texture lookup merges remaining matching rules before
dropping dimension overrides that truncate to the
original size (depth is at least one). This preserves later-rule precedence,
including an identity that cancels an earlier size override. Remaster explicitly
selects the native 1280x720 Resolution preset so its resolution rules are identity.
Built-in scaling remains enabled for the session: identity-only and unmatched
textures retain it, while nonidentity pack dimensions replace scaling only for
that matching texture. Shadow-map overrides therefore coexist with FSR and other
render targets keep the selected scale. LOD bias values are converted to signed 1/64 units; anisotropy values 1/2/4/8/16
map to the existing sampler exponents. The RGB10A2-to-RGBA16 UNORM GUI precision
upgrade selects the new resource/view format and expands original guest uploads.
Other format conversions are rejected explicitly. CPU readback of alternate
formats retains D3D12's existing unsupported restriction; this requires particular
attention when checking GUI surfaces on console. Shader and PSO
persistent caches are bypassed for pack sessions to prevent reuse across pack
fingerprints; sessions without packs retain their existing cache behavior.

The small Metal adapter supports memory-only and advisory timing plans. It
rejects texture/shader replacements because Metal exposes no publication hook
outside its owned subtree. No files under `Renderer/Metal/` are changed.

UWP selects raw HDMI output dimensions where available, falling back to current
CoreWindow bounds times RawPixelsPerViewPixel. The same dimensions determine
render scale and renderer/swap-chain creation. The renderer clamps scale to
[1, 3]; an accepted pack can replace effective dimensions per texture.

The recomp D3D12 present path always synchronizes: interval 1 for the 60 fps
setting, 2 for 30 fps on the game's 60 Hz clock; frame interpolation is disabled.
FPS++ names and `vsyncFrequency` controls trigger one host message and do not
create a second host pacer. `g_target_fps` controls host presentation only.
`tools/packs/recompile.py` omits the host timing/physics hooks for external
timing packs. It retains the frame pause/telemetry hook, including after
FPS++'s replacement call, with host delta/scaling disabled. Pack LR redirects
return to labels in their caller; constant CTR continuations back to displaced
original instructions execute in the same generated stack frame. Guest bytes
remain unchanged. Synthetic tests execute both continuation conventions.

## Synthetic verification

```sh
sh runtime/src/packs/tests/run.sh
python3 -m pytest tools/packs/tests -q
sh runtime/src/host/uwp/tests/check-syntax.sh
```

The new C++ test uses ASan/UBSan, four SHA-256 vectors, a fake guest address
space, actual portable D3D12/Metal hook validation, and authored ELF section
metadata. It verifies snapshot/hash identity even if the source changes, snapshot
cleanup, range rejection and empty-plan/failure fallbacks. Texture tests cover
identity removal after truncation and ordered merging, shadow overrides alongside
scale 1.5, and explicit render-target precedence without changing other surfaces. No game assets are
needed. See the D3D12 `validation/syntax_check.py` for the Windows-target check;
that controlled header check does not run a Windows GPU or validate the WinRT
HDMI branch. Full app linking needs private `generated/` sources. No Xbox/device
rendering or real FPS++ gameplay validation is claimed. See
[Remaster validation](../../../docs/remaster-validation.md).
