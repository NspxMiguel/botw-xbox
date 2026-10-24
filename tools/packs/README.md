# Graphic packs for static recompilation (W5)

Players supply their own Cemu packs. This directory contains tools and synthetic tests,
not packs, game binaries, patches copied from a game, or generated game code. The pack compiler integrates with the shared analyzer, code generator, runtime
dispatch, loader and D3D12 renderer.

## Compile with packs

Install LLVM (`brew install llvm` on macOS). Python has no additional dependencies.
Keep packs and the selection manifest outside the repository. For example:

```json
{
  "version": 1,
  "title_id": "00050000101c9400",
  "packs": [
    {
      "path": "./graphicPacks/FPS++",
      "enabled": true,
      "timing": true,
      "presets": {"Mode": "Normal Settings", "FPS Limit": "60FPS Limit (Default)"}
    },
    {
      "path": "./graphicPacks/Graphics",
      "presets": {"Resolution": "1920x1080 (Full HD)"}
    }
  ]
}
```

Paths are relative to the manifest. Names must match the player's pack revision exactly.
`rpx_sha256` may optionally pin the input file. `title_id` selects eligible packs; the
actual executable match is the Cemu module CRC calculated from the input RPX, not a
caller-supplied CRC. Disabled entries are never read. An explicit empty `packs` array
produces a fresh empty snapshot.

```sh
python3 -I tools/packs/recompile.py /external/game/code/U-King.rpx generated \
  --packs /external/mods/selection.json
# Optional: --llvm-mc /path/to/llvm-mc
```

## Remaster selection

UWP builds offer **Visuals: Remaster / Original** in Settings and the quick menu,
applied at the next launch. Original suppresses compiled shader replacements and
texture rules while retaining FPS++ and all static PPC patches. See
[Visuals toggle](../../docs/visuals-toggle.md) for the exact categories, build
alternatives and binary size measurement.

Generate a console-specific selection from a player-supplied
`graphicPacks/BreathOfTheWild` directory:

```sh
python3 -I tools/packs/preset.py remaster --console series_s \
  /external/graphicPacks/BreathOfTheWild -o /external/selection.json
# Use --console series_x for Xbox Series X.
```

The version 1 manifest references the original folders with paths relative to the
manifest. No pack, shader or patch files are copied. Every selected category and
preset is validated against `rules.txt`, including preset visibility/conditions.
A changed or missing name is an error naming the pack and selection. Missing
optional pack folders produce warnings and are skipped; `Mods/FPS++` is required.
A present pack with missing or invalid rules is an error.

The single selection table lives in `preset.py` and is shared by the build entry
point. Choices match the inspected official pack revision exactly:

| Setting | Series S | Series X |
| --- | --- | --- |
| FPS++ Mode / FPS Limit | Normal Settings / 60FPS Limit (Default) | Same |
| Graphics Resolution | 1280x720 (HD, Default) | Same |
| Graphics Shadows | Ultra (300%) | Extreme (400%, Unstable) |
| Graphics Shadow Draw Distance | High (Default) | Very High (Recommended) |
| Graphics Anti-Aliasing | Normal FXAA (Default) | Same |
| Enhancements Clarity | t3nka Neutral+ | Same |
| Enhancements Reflection Range | Pack default (4 samples) | Medium (8 samples) |
| Draw Distance NPCs / objects | Medium (1x) | High (1.25x) |
| Draw Distance Trees (2D Billboards) | High | High |
| Draw Distance Grass Blades Density | Medium (Default) | High |

FPS++ explicitly owns timing (`timing: true`). Neutral+ keeps the color treatment
close to natural; Series S uses lower shadow and scene detail to reduce cost.
Series X takes the highest shadow tier not labelled "Not Recommended" in this
revision; upstream labels that tier **Unstable**, so this is not a stability or
60 fps performance claim. Both consoles explicitly select native 1280x720
Resolution so its size expressions resolve to identity; FSR owns render resolution. Texture Distance Detail (LOD) and Anisotropic Filtering retain the pack defaults
(Normal LOD and Ultra 8x anisotropy in the inspected revision). D3D12 accepts
these fields, including their unconditional rules. "High" tree billboards
extend geometric detail without selecting texture LOD bias.

No Workarounds are selected. AMDShaderCrash is explicitly OpenGL-only;
GrassWorkaround is Vulkan-only. The other supplied workarounds target
OpenGL, NVIDIA or Vulkan/ReShade, rather than the Xbox D3D12/AMD path.

Generate and consume a temporary manifest directly during recompilation:

```sh
python3 -I tools/packs/recompile.py /external/game/code/U-King.rpx generated \
  --preset remaster --console series_x \
  --graphic-packs /external/graphicPacks/BreathOfTheWild
```

`--preset` and `--packs` are mutually exclusive. `--console` and `--graphic-packs`
are required with `--preset` and cannot accompany `--packs`. Temporary manifests
are removed after loading; resolved pack paths continue to reference the player's
original directory.

Native resolution rules no longer disable FSR: effective identity dimensions are
removed at build time when exact dimension filters prove they are no-ops.
Wildcard matches and later rules that can reset earlier overrides are resolved
after merging against each original guest texture (including integer truncation). A remaining shadow-map size override coexists with built-in
scaling. An explicit render-target size override replaces built-in scaling only
for that matching texture; other surfaces retain the selected scale.

Selection validation does not prove a complete pack build or rendering. Omitting
a category does not suppress default presets or unconditional `[TextureRedefine]`
sections. D3D12 supports LOD and anisotropy overrides and the GUI precision conversion
from RGB10A2 (0x19) to RGBA16 UNORM (0x1f). Other format conversions fail
validation explicitly. The preset generator does not rewrite the player's rules.

This entry point installs an invocation-local `Image` hook into `tools/recompile.py`:
load/decompress/relocate RPX -> resolve packs -> assemble and validate every write ->
patch the guest Image -> analyze PPC -> generate C++. The source RPX is never modified.
Caves start at Cemu's 0x01800000 address, bounded to 1 MiB per pack and
8 MiB total below `.text`,
with overlap and branch-range checks. Cave constants are excluded from function
discovery. New import calls retain HLE relocation metadata; stale relocations covering
replaced instructions are removed before analysis/codegen.

Direct `bl`/`bla` targets use ordinary function discovery. Constant
`li/mtctr/bctrl` and `lis/(addi|ori)/mtctr/bctrl` sequences in patched text or
caves also seed function entries, without treating cave data as code.
`packs_report.json` includes a `cave_calls` audit of every cave call and patched
call into a cave, including source locations, resolved targets, imports and
displaced-code continuations. Missing known code targets fail compilation.
Conditional branch displacements are parenthesized before LLVM assembly:
otherwise LLVM can parse `blt -16` as the prediction hint `blt- 16` and reverse
the jump. Every assembled direct branch is decoded and its destination checked
against the resolved patch expression.

The normal generated sources are accompanied by `packs_generated.inc` (compiled runtime
snapshot) and `packs_report.json` (selected presets, variables, CRC, hashes). These are
private generated outputs and must never be committed or distributed. Inside this repo,
only `generated/` is accepted as output. Recompile and rebuild for every change to packs,
presets, or game executable. Use this entry point with an empty selection when disabling
packs; the upstream entry point does not remove an old pack snapshot. The fingerprint
covers the executable, configuration, rules, patches and shader sources; use it in shader
cache keys and launcher build metadata.

## Supported surface and explicit limits

- `rules.txt` versions 3–7: repeated sections, quoted metadata, comments, `[Default]`,
  typed variables, categorized `[Preset]`, defaults and conditions. Categories resolve
  in declaration order; dependencies must refer to defaults or preceding categories.
  Hidden explicit selections, ambiguous defaults, missing variables, unsafe expressions,
  unsupported sections and unknown texture/control fields fail the build.
- Expressions: finite bounded arithmetic, comparisons, booleans, bit operations,
  `min/max`, `abs`, `floor/ceil/round`, `int/float`, and `reloc` (identity at link addresses).
  No Python evaluation, shell interpretation, file imports or assembly directives that
  load external files. Variable type annotations are accepted; numeric values retain
  their precision until used as an instruction operand or texture dimension.
- Native `patch_*.asm`: CRC-selected groups, symbols shared across active files in **one
  pack**, forward constants/labels, `.origin`, `codecave`, explicit per-line addresses
  (which do not advance the cursor), `@h/@ha/@l`, `.byte/.short/.int/.uint/.ptr`,
  `.float/.double/.string`, PPC32 instructions and direct `import.library.symbol` calls.
  `.int/.uint` embedded within an instruction stream are treated as raw instructions.
  LLVM encodes normal instructions; unresolved expressions, out-of-range branches,
  unsupported assembler/recompiler instructions, and overlapping writes are errors.
  Absolute `ba`/`bla` targets below 0x02000000 keep their AA bit. Other branch
  targets use range-checked relative encoding. Mnemonics/registers are case
  insensitive; floating comparisons accept implicit cr0 and condition-bit
  expressions accept cr0..cr7 and lt/gt/eq/so. `.align` inserts byte alignment;
  `@lo`/`@hi` are aliases for `@l`/`@h`.
- `[TextureRedefine]`: dimensions, formats/exclusions, tilemodes/exclusions, `inMEM1`,
  resolution/depth/format overrides, LOD biases and anisotropy. Numeric expressions are
  resolved into a build snapshot. The renderer casts dimensions toward zero as Cemu does.
- Shader replacements named `baseHash_auxHash_vs|gs|ps.txt` with both 64-bit hashes:
  preset substitution and exact hash/stage lookup. GLSL is passed to W2's shader pipeline;
  this layer does not compile GLSL itself.
- `[Control] vsyncFrequency` is passed to renderer/host pacing. Conflicting enabled-pack
  frequencies fail. This value is a pacing requirement, not proof that the game runs at
  that frame rate.

Legacy `patches.txt`, RPL module patching, file replacement mods, paired-single assembler
extensions LLVM does not support, `[OutputShader]` and emulator-only services are not
implemented. Packs requiring those features need an adapter rather than silent skipping.
No community pack is bundled or claimed game-tested. Current FPS++ and Graphics pack
files were inspected as format references, but a complete player-supplied revision still
needs compilation and gameplay validation with the matching executable.

## FPS++ and timing ownership

A pack named `FPS++` automatically owns timing; `timing: true` makes that explicit (or
marks another timing pack). Only one timing pack may be enabled. Its code patches are
compiled along with physics/timing fixes from **all matching patch files**, rather than
replacing it with a simple frame-rate unlock. The timing/scaling hooks from `runtime/src/game/fps.cpp` are omitted to prevent
double-scaling physics. The frame bookkeeping hook at 0x031fa97c remains
active after FPS++'s replacement call: it handles UWP pause and CPU telemetry,
and its host delta/scaling path is gated off for a compiled timing pack. Other
hooks on instructions replaced by packs are omitted. The compiled pack owns guest timing and physics. Host `g_target_fps` controls
presentation; it does not restore omitted physics hooks. There is no runtime
PPC patch toggle. Code-patch selection changes require recompilation on the Mac/build host; Xbox
only executes the resulting static code. No JIT or on-console compilation is involved.

## Runtime handoff to loader/host and W2

The runtime contract is [`packs.h`](../../runtime/src/packs/packs.h). `packs.cpp` is
portable C++17. Existing recursive runtime source globs include it; generated/ must be
on the include path for `packs_generated.inc`. Without that file, `compiled_plan()` is
empty, preserving builds using the existing recompile command. No new dependency is
required.

**Loader/host integration remains with its owners:** after successful `load_main` and
all relocations, before any guest thread, obtain `packs::compiled_plan()`, compute the
SHA-256 of the RPX actually loaded, map/reserve the snapshot's codecave ranges, then call:

```cpp
std::string error;
const auto& plan = packs::compiled_plan();
if (!packs::install(plan, loaded_rpx_sha256, guest_memory_hooks, renderer_hooks, error)) {
    // Return to launcher with the error; never start the guest after failure.
}
```

`GuestMemory` validates all ranges before any write. Validation must reject read-only,
unmapped or runtime-owned regions and account for the separate codecave area. Writes initialize
both patched original bytes and cave code/data after the ordinary loader would otherwise
overwrite them. SHA mismatch is fatal. The generated PPC code is already patched; this
initialization makes its data constants, pointers and guest code reads agree with it.

**W2 integration:** implement `RendererHooks::prepare/commit`. `prepare` validates every
texture/control field and compiles replacement shaders without publishing GPU state;
unsupported capabilities return an actionable error. `commit` publishes the accepted
snapshot. The plan must outlive renderer pointers (the generated plan is static).

- Before texture allocation, call `texture_overrides(plan, original_guest_description)`.
  Match dimensions, format, tile mode and MEM1 location against the original description.
  Apply returned fields in order; later rules/packs win per field. Resize render targets,
  viewport/scissor scaling and texture views together. Nonidentity pack dimensions
  take precedence over built-in `g_render_scale` for that texture only; identity-only
  and unmatched textures retain built-in scaling. LOD values are in float LOD units, anisotropy is 1/2/4/8/16.
- Before compiling a guest shader, call `find_shader` with original Cemu hashes and stage.
  Use the returned GLSL through the W2 compiler path. Include `plan.fingerprint` in caches;
  reject incompatible shader dialects in `prepare`.
- Route `Pack.control`'s `vsyncFrequency` to virtual vblank/frame pacing. The old 30/60 host
  settings must not independently change guest timing when FPS++ owns it.

The hosts install this snapshot through `packs/host_install.h` before guest
startup. The Metal adapter still rejects texture/shader replacements. The launcher should show the compiled fingerprint/selection and require a new build
for code-patch changes.

## Validation

```sh
python3 -m pytest tools/packs/tests -q
python3 -m unittest discover -s tools/packs/tests -v
```

Tests use only authored synthetic ELF/PPC bytes. They cover ordered parsing, conditional
presets and resolution expressions, invalid input, code/data layout, cross-file labels,
branch encoding, CRC selection, conflicts and transactional failure, relocations/HLE,
Image -> Analyzer -> Generator, compilation of the emitted snapshot, and the portable
C++ runtime contract under AddressSanitizer and UndefinedBehaviorSanitizer. LLVM and
`clang++` must be available. The upstream `cafe/rpx.py` may emit ResourceWarnings for its unclosed input file.

The Remaster compiler has been run against the player's RPX and official packs.
Console gameplay, D3D12 rendering, timing and performance still need device
validation. See [Remaster validation](../../docs/remaster-validation.md).

Format references: [Cemu patch format](https://wiki.cemu.info/wiki/Cemu_patches),
[Cemu module CRC implementation](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/OS/RPL/rpl.cpp),
[community packs](https://github.com/cemu-project/cemu_graphic_packs),
[Cemu texture rules](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/GraphicPack/GraphicPack2.cpp).
