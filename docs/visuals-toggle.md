# Remaster / Original visuals

Settings and the in-game quick menu offer **Visuals: Remaster / Original** in
English and Portuguese. Remaster is the default. A selection is saved in UWP
LocalSettings as `visuals` (`0` Remaster, `1` Original); missing or invalid stored
values select Remaster. `helper.json` accepts the same numeric setting.

Changes apply when the app next launches the guest. Fully close and reopen the
app, or use Return to launcher home, which restarts the process. Resume from the
quick menu and suspend/resume keep the current visuals. There is no rebuild,
shader compilation on the build host, or download needed to switch modes in a
Remaster build. Ordinary runtime shader compilation still happens on demand.

## What switches

`packs::selected_plan()` captures the preference once, after Mods filtering and
before guest startup. `select_visuals()` creates an independent renderer snapshot.
Remaster preserves the selected packs. Original clears **all shader replacements
and texture rules** from that snapshot, including unconditional rules and rules
in packs containing static PPC patches. It does not change the player's Mods
selection file. Switching back restores the build's selected resources.

| Category in the current Remaster preset                                     | Original behavior               |
| --------------------------------------------------------------------------- | ------------------------------- |
| Graphics replacement shaders, FXAA and shadow shader variants               | Original guest shaders          |
| Enhancements clarity (t3nka Neutral+), reflections and other shader effects | Original guest shaders          |
| Pack resolution, shadow-map dimensions and render-target scale overrides    | All pack texture rules disabled |
| GUI RGB10A2 to RGBA16 precision override                                    | Original guest format           |
| Texture LOD, relative LOD and anisotropy overrides                          | Original guest sampler behavior |
| Draw Distance's texture LOD rule                                            | Disabled                        |
| FPS++ PPC timing/physics fixes and 60 Hz control                            | Retained in both modes          |
| Graphics aspect ratio / GUI PPC patches                                     | Retained in both modes          |
| Draw Distance entity, object, billboard and grass PPC patches               | Retained in both modes          |

The inspected Series X snapshot contains 37 Graphics shaders and 12 texture rules,
4 Enhancements shaders and 1 texture rule, and 1 Draw Distance texture rule.
Original suppresses all **41 replacement shaders and 14 texture rules**. Counts
can vary with the player's pack revision and console preset. Series S follows
the same selection policy, with the resources compiled for Series S.

The independent Render scale, FSR/upscaling, frame generation and presentation
FPS settings retain their values. Original removes pack scale rules, but does
not force the renderer to native 720p or turn FSR off. File mods, including texture
replacements and translations, are also independent. Original is therefore an
original-style rendering mode, not a claim of a byte-identical Wii U image.

## Static PPC limits and build alternatives

The pack compiler patches a private RPX image **before** function discovery,
control-flow analysis and C++ generation. The runtime's `initial_memory` restores
patched instructions, cave constants and pointers after the loader; it cannot
undo the machine code compiled from those instructions. Both modes preserve
that entire memory initialization, RPX identity, pack controls and timing
ownership. Removing writes alone would leave native code and guest memory
inconsistent.

FPS++ remains enabled and owns timing at 60 in both modes. The host physics hooks
omitted for FPS++ stay omitted. Changing Visuals neither disables FPS++ nor
changes the separate presentation FPS setting.

Graphics also supplies aspect ratio / GUI PPC patches; these are not shader
replacements. Even with the preset's native 16:9 dimensions, Original retains
the compiled implementation. Draw Distance's entity/object distances, tree
billboards and grass density are compiled PPC behavior and remain enhanced.

A completely pack-free executable still requires the builder's **`--preset
vanilla`**. That preset uses an empty pack manifest: it removes Graphics and Draw
Distance PPC patches **and FPS++**, so it is a separate build, not this feature's
Original mode. To remove the visual PPC patches while keeping FPS++ at 60,
recompile with a custom `--packs` manifest selecting only the player's FPS++
folder, `timing: true`, Mode `Normal Settings`, and FPS Limit
`60FPS Limit (Default)`, then rebuild. The builder's vanilla preset does not
currently offer that FPS++-only combination.

### Why one PPC variant

There is no existing dual-variant or runtime PPC guard mechanism. Duplicating an
entire analysis/codegen pass could share host/renderer libraries, but would need
separate generated symbols, dispatch tables, cave layouts, relocated data and
hook selections. Only FPS++ should be shared; using a pack-free second variant
would also lose its timing ownership. Duplicating all guest functions approaches
two copies of guest native code rather than the small renderer-selection cost.

Guarding only visual patch sites could be smaller and faster than whole-program
duplication. It still requires preserving original instructions, discovering both
control-flow graphs (including displaced continuations), reconciling their
function boundaries, and selecting matching cave constants/data at startup.
Current snapshots merge memory writes without runtime per-pack ownership or
original-byte alternatives. Arbitrary branches/calls cannot safely be handled by
adding a boolean around emitted C++ statements. A selective function-variant
compiler is feasible future work, but its size and speed need measurement after
implementing those analysis and dispatch contracts. Neither approach is claimed
implemented or benchmarked here. This change has no per-frame PPC dispatch flag.

## Cache and lifetime

The final snapshot is immutable for the process and outlives D3D12 pointers.
Its diagnostic fingerprint includes `:visuals:remaster` or `:visuals:original`.
D3D12 currently bypasses persistent shader bytecode and PSO caches whenever a
pack plan is active. Original retains the timing/compiled packs, so it takes the
same safe cache path. The fingerprint suffix does not claim to enable a new
persistent pack cache. Empty vanilla plans retain the existing cache behavior.
If reading the Mods catalog fails, the selected Visuals policy still applies to
the compiled snapshot.

## Binary size

Measured with the same existing Series X Remaster generated sources, Release
UWP toolchain/dependencies and `nice ninja -C build/local-uwp-game -j4`:

| Executable             |                                      Bytes |
| ---------------------- | -----------------------------------------: |
| `xbox/port` baseline   |                                421,637,632 |
| With Visuals selection |                                421,644,288 |
| Delta                  | **+6,656 bytes (+6.5 KiB, about 0.0016%)** |

Both modes use that same executable. No second PPC program, second shader bundle
or generated pack snapshot was added. Size is the linked PE file, excluding PDBs,
packages and runtime shader caches; it is not an estimate of console memory.

## Verification and console checks

`uwp_visuals_tests` exercises default/invalid/persisted selection decoding,
cycling, selection after the real Mods filter with locked PPC packs, original
shader lookup and texture-rule suppression, retained memory writes and timing,
independent snapshots, empty plans and both translations. The UWP executable
links with the player's existing Remaster-generated code. Broader host results
and any environment constraints are recorded in `REPORT-80-visuals-toggle.md`.

A human must check on Series X and S: both menu rows and all subsequent quick-menu
actions; persistence after a full restart; no visual change on Resume; a visible
clarity/shadow/reflection difference after restarting; unchanged saves/file mods;
FPS++ timing, physics and cutscenes at 60; Original's retained draw distances; and
switching back to Remaster without stale shaders. Check native and FSR output,
GUI formats and memory/performance on each console. No console was contacted.
