# Root CBVs and stable D3D12 tables

This work preserves shader registers, bytecode, draw ordering and quality. It
moves b0..b3 from each graphics-stage table into four root CBVs, and restores a
bounded secondary table cache now that low CBV addresses no longer change its
key. No commit or console deployment is part of this task.

## Root layout and internal passes

VS, PS and GS each use k=4. The cost is **6 descriptor tables + 16 runtime
constants + 16 push constants + 3 stages × 4 CBVs × 2 DWORDs = 62 DWORDs**, below
64. Root tables retain indices 0..5; root CBVs occupy 6..17; runtime and push
constants move to 18 and 19. Named helpers address every shared root argument.
The table CBV range starts at b4 in the same stage space; the SPIR-V remapper
and shader-cache ABI remain unchanged.

This checkout actually defines 16 guest uniform buffers and 18 texture units,
so its old tables had **17 CBVs + 18 SRVs + 1 UAV = 36** descriptors, rather
than the task's measured 32-entry configuration. Shared tables now contain
**13 + 18 + 1 = 32** descriptors. No binding limits were reduced.

Surface copy/output and ImGui use the shared signature and compact pixel table.
Output uniforms use the same root-CBV binding helper as game draws. Binding the
shared signature initializes all twelve root CBVs to a permanent zero buffer,
also covering shader stages absent from the draw. Surface-copy paths replace
the pixel bindings before drawing. FFX color/depth conversion and FSR have
private signatures and retain the full-table layout via Stage::Count. Their
literal private parameter indices remain local to those private signatures.
Frame interpolation and simulated-Series-S compute passes have independent
signatures/heaps and need no root-index changes.

## Lifetimes and invalidation

- Root CBVs compare GPU addresses, not dirty flags. Command-list reset and every
  shared root-signature/heap bind clear the tracked addresses and bind zeroes.
  Internal passes invalidate draw state, forcing that bind before game rendering
  resumes. Missing bindings always use the persistent 64-KiB zero resource,
  never a retired transient address.
- The exact last-table cache and 512-entry open-addressed cache compare complete
  value keys with b0..b3 zeroed. Hashes never establish equality. Linear probing
  is bounded to 16 entries; a saturated probe chain replaces its home slot.
  Entries are stored in fixed arrays; lookup and insertion allocate nothing.
- Both caches check GPU-heap generation and staging-source generation. The GPU
  heap advances generation on OnSubmit, wrap to zero and empty-ring reset.
  OnSubmit prevents reuse in a later submission without extending its marker.
  Retire only frees submitted regions, never recording regions. Hits reuse
  immutable copies protected by their recording submission's marker. Therefore
  no table can remain a lookup hit when its slots become eligible for overwrite.
  Allocation may itself advance generation; insertion uses the generation after
  allocation. Retirement waits and the ring's strict free-space checks remain.
- Staging Free advances generation before a handle can be reused. Texture view
  SRVs are created in newly allocated handles for every view/swizzle, including
  slice-copy views, and never rewritten. FFX and FSR do rewrite persistent SRV
  handles; they now explicitly advance staging generation before every rewrite.
  This invalidates cache lookups, while prior shader-visible copies stay intact.
  Persistent CBV/UAV staging slots are private to table construction and their
  address/size or resource/size is already part of the full key.
- Buffer-cache recreation clears both table caches, root-address tracking and
  persistent UAV source identity, including possible COM-address reuse.
  Upload content reuse still checks bytes and recording-submission identity.

The cache intentionally does not survive a submission. It can span draws and
frame boundaries only while the same recording generation remains active;
persistent reuse across submissions would require a different ownership scheme.

## Root-buffer bounds

Root descriptors use the same aligned addresses as the former CBV views. Uniform
uploads are allocated at 256-byte alignment with their complete declared uniform
range rounded to 256 bytes. Guest-cache direct bindings already require aligned
starts and enough rounded logical bytes; unaligned/end bindings use the existing
aligned scratch path. Every root bind asserts alignment in debug builds.

A root CBV has no descriptor size. Upload-ring resources, overflow resources,
the guest cache and every allocation path of its shared scratch resource now
include an additional physical 64-KiB tail beyond their unchanged logical
capacity. Any root start inside that capacity therefore has enough resource
remaining for the maximum legal cbuffer (4096 × 16 bytes), including at the last
allocation. The permanent missing-binding buffer is also 64 KiB and explicitly
zeroed. Ring allocation/retirement, scratch offsets, copy ranges and guest-cache
logical sizes stay unchanged; padding is not used for extra allocations.

## Expected saving and profiler interpretation

Each shared miss copies four fewer descriptors (36 → 32, **11.1% fewer slots**),
and each new cache hit avoids the whole table allocation/copy. At the supplied
4,963 misses/frame, eliminating all those misses would avoid about 178,668 old
source descriptor copies/frame; that is an upper bound, not a predicted hit rate.
Root bindings introduce up to four address checks per active stage, with native
writes only on changes. Unchanged sampler/table/runtime bindings still skip.

The aim is to remove most of the one-table-per-draw misses when the low four
registers contain all changing CBVs. Registers b4 and above, texture changes,
cache pressure and submission boundaries still cause misses. Against the supplied
**4.05 ms/frame descriptors** slot, **1–3 ms/frame** is an unmeasured working
estimate if most misses disappear, with a strict gross ceiling of 4.05 ms and
possible regression if lookup/root writes outweigh copies avoided. A console
profile is required to establish any saving; no FPS claim follows from host tests.

`descriptor-hit` includes both exact and hash-cache hits; `descriptor-fast` is
the exact-cache subset; `descriptor-miss` counts actual new table copies.
`root-cbv-set/frame` and `root-cbv-skip/frame` count native root-CBV writes and
equal-address skips, including internal/reset zero bindings. All retain the
same source-game-frame divisor.

## Changed files

Paths below are relative to `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/` unless
otherwise qualified.

| Files | Change |
| --- | --- |
| D3D12BindingModel.h, D3D12RootSignature.h | Budget, named indices and register/table split. |
| D3D12StageTable.h | Stable value keys, hash and bounded fixed table cache. |
| D3D12Renderer.h, D3D12Renderer.cpp, D3D12RendererCore.cpp | Root tracking/binding, zero resource, cache integration/recreation invalidation and backing bounds. |
| D3D12Memory.cpp | Physical upload/overflow tails. |
| D3D12DescriptorHeaps.h, D3D12DescriptorHeaps.cpp | Source rewrite notification and ring-lifetime proof. |
| D3D12SurfaceCopy.cpp, D3D12ImGui.cpp | Shared compact-table bindings and output root CBVs. |
| D3D12Ffx.cpp, D3D12Fsr.cpp, LatteTextureViewD3D12.cpp | Mutable-source invalidation and immutable-view contract. |
| ../../Core/LatteProfiler.h, ../../Core/LattePerformanceMonitor.cpp | Root-CBV counters. |
| README.md, docs/latte-profiler.md, docs/latte-rootcbv.md | Layout, counter contracts and this report. |
| tools/tests/latte_stage_table_test.cpp, latte_root_signature_test.cpp, latte_descriptor_heap_test.cpp | Production key/cache, signature description and source/ring lifetime tests. |
| tools/tests/d3d12_stubs/{d3d12.h,Cafe/HW/Latte/Core/LatteConst.h,Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h}, tools/tests/run_latte_perf3_tests.sh | Host SDK stand-ins, production binding constants and test runner. |

## Validation

Host runner: `bash tools/tests/run_latte_perf3_tests.sh`. It exercises the actual
root-signature builder through a host serializer stand-in, computes the DWORD
budget from emitted parameters, checks stage visibility/register coverage and
private full-table offsets, and tests cache hit/miss, both generation changes,
clear, colliding bucket chains, full-cache replacement and low-CBV exclusion.
Existing descriptor retirement/wait/reset, profiler and index tests also run
with AddressSanitizer and fatal UndefinedBehaviorSanitizer, including x86 AVX2
under Rosetta. Output: `build/rootcbv-host-tests.log`.

The UWP build tree and generated corpus were copied into this worktree, with
textual absolute paths rewritten. Copied binary dependency records were removed
so Ninja reconstructs them against this worktree. Three unused copied compiler-
probe binaries containing old source paths were removed after the full rebuild. The build runs only after
checking `pgrep -x ninja`, using `nice ninja -C build/local-uwp-game -j4`.
The full UWP build passed (exit 0), compiling 1,285 objects/steps and linking
`build/local-uwp-game/BotWRecomp.exe` at step 1,286. Output is in
`build/rootcbv-build.log`. A second invocation passed with **no work to do**
(`build/rootcbv-build-verified.log`). The host suite passed, and
`git diff --check` passed. Existing `/await:strict` and unrelated source warnings
remain; no compile/link errors occurred. A final scan of the build tree found
no remaining `/botw-wiiu/base` references, including its reconstructed binary
Ninja dependency records. All changes remain uncommitted.

Still requires the console: root-signature serialization/driver validation,
D3D12 debug-layer and GPU validation under descriptor pressure, fixed-Plateau
pixel parity, internal output/ImGui/FSR/FFX and compute passes, actual root-set/
skip and table hit/miss rates, and a comparable descriptor timing window.
