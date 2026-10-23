# Latte bindless feasibility on D3D12 UWP

## Decision

Technically feasible with a new binding ABI on a device exposing Shader Model
6.6 **and** Resource Binding Tier 3. It is not a compiler-target switch. This
round does not implement bindless or change generated shaders.

Do not assume the Series X UWP adapter `SraKmd_arden` supports it. Microsoft's
DirectX engineer documents SM 5.1–6.4 for Series X|S UWP, which would rule out
6.6 direct heap indexing. That guidance is historical, not a measurement of the
owner's current console. The supplied log/profile contains neither the returned
shader model nor binding tier; support on that console remains unverified.
Use the actual UWP device queries below, rather than retail/GDK hardware caps.
[Series X|S UWP guidance](https://walbourn.github.io/directx-and-uwp-on-xbox-series-x-s/).

## Existing compiler and binding path

`D3D12ShaderTranslateHLSL.cpp::SPIRVToHLSL` uses SPIRV-Cross to emit fixed
register bindings after the SPIR-V remapper assigns a register/space per stage
and resource class. `D3D12ShaderCompiler.cpp::_CompileDXC` currently chooses
`vs_6_0`, `ps_6_0`, `gs_6_0` (and `cs_6_0` for compute). FXC remains the
legacy path; the optional SPIR-V-to-DXIL backend is a distinct translation path.
None currently emits heap-indexed resources.

A future bindless variant must use DXC with `*_6_6` targets and typed resource
locals from `ResourceDescriptorHeap` / `SamplerDescriptorHeap`. DXC supports
those constructs; existing fixed register declarations must be translated to
those accesses. Modify the structured SPIRV-Cross backend or a controlled
intermediate representation, not global text replacement: array textures,
integer textures, comparison samplers, cbuffer layouts, UAV operations and
helper-function resource parameters all require correct typing.

For example, a uniform per-draw texture index can initialize a
`Texture2D<float4>` local, and its sampler index a `SamplerState` local.
Root constants are uniform across a draw. Any future lane-varying index needs
`NonUniformResourceIndex`. The root signature needs both
`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` and `SAMPLER_HEAP_DIRECTLY_INDEXED` flags.
Bind the heaps before that root signature, and repeat the required bindings
after heap changes. These are requirements of the
[SM 6.6 dynamic-resource specification](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html).

## Runtime gate and fallback

`D3D12Renderer.cpp::CreateDevice` already queries shader models descending from
0x67 to 0x60 into `m_highestShaderModel`, and queries
`D3D12_FEATURE_D3D12_OPTIONS` into `m_bindingTier`. `HighestShaderModel` is an
in/out field of `D3D12_FEATURE_DATA_SHADER_MODEL`, not an independent capability.
Log both returned values, query HRESULTs, OS/runtime and adapter/driver identity
on the console. The debug overlay also exposes the cached shader-model value.

A standalone equivalent is:

```cpp
D3D_SHADER_MODEL highest = D3D_SHADER_MODEL_5_1;
for (auto requested : {D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
                      D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
                      D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
                      D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0})
{
    D3D12_FEATURE_DATA_SHADER_MODEL model{requested};
    const HRESULT hr = device->CheckFeatureSupport(
        D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
    if (SUCCEEDED(hr))
    {
        highest = model.HighestShaderModel;
        break;
    }
    if (hr != E_INVALIDARG)
        break; // Report unexpected/device failure; never enable bindless.
}
D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
const HRESULT optionsResult = device->CheckFeatureSupport(
    D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
const bool hardwareEligible = highest >= D3D_SHADER_MODEL_6_6 &&
    SUCCEEDED(optionsResult) &&
    options.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_3;
```

Older runtimes can reject an unrecognized requested model with `E_INVALIDARG`;
the descending query matters. A successful call returns the lower of device
support and the requested ceiling.
[Query contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_shader_model).

The full enable gate must also successfully compile/load signed DXIL 6.6 with
the packaged DXC/DXIL libraries, serialize/create the new root signature, and
create a representative PSO on the UWP device. Never bypass a failed runtime
query based on adapter name. Keep the existing table ABI and compiler backend
as the fallback when the gate fails; select one ABI before loading shader/PSO
caches. Keep internal copy/clear/postprocess shaders on the old compatible root
signature initially, rebinding heaps/root arguments when switching paths.

## Descriptor and root-argument design

Create shader-visible SRV descriptors once per actual view/swizzle/format,
samplers once per description, and streamout UAVs once per resource/range.
Handles become heap slot indices. CPU descriptors alone are not sufficient:
the shader indexes the currently bound shader-visible heaps. Allocate null
slots for each supported resource type; keep feedback/slice-copy refreshes and
resource transitions exactly as today.

A changed descriptor gets a new slot; do not overwrite an in-flight descriptor.
Free slots only after the last referencing submission fence. Resource deletion,
view replacement, sampler changes, heap growth/reset and device loss need
versioned handles and fence-safe retirement. Sampler-heap capacity is especially
restrictive: reserve capacity or rebuild/remap safely after draining. Existing
sampler reset semantics cannot be reused with persistent indices blindly.

The current ABI permits up to 17 CBVs, 18 SRVs and 18 samplers per stage, plus a
streamout UAV. Three complete index arrays plus existing runtime constants exceed
the [64-DWORD root-signature budget](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html). Pass compact indices only for used bindings
when they fit; the scalable approach is a small root descriptor pointing at an
immutable per-draw upload record of indices, plus existing runtime constants.
Transient CBVs still need descriptors when their GPU address changes, or an
alternative uniform-buffer ABI using offsets into persistent backing buffers.
This is why “draws never copy descriptors” also requires CBV/uniform design,
not just replacing texture tables. An index upload record is data, not a
shader-visible descriptor-table copy.

## Cache impact

`RendererShaderD3D12.cpp` currently stores bytecode-cache version 2 and a binding
remap in `CacheHeader`, with backend-specific filenames. Add explicit binding
ABI, target SM, translator version and layout/remap metadata to cache identity;
bump the version or use separate files. Keep table/FXC and bindless/DXC variants
separate. Guest shader hashes and source caches can remain, but native bytecode
must be regenerated. Never load cached fixed-table bytecode with the new root
signature. Graphic-pack overrides need the same variant/typing treatment.

The PSO cache and serialized pipeline descriptions must also include the binding
ABI/root-signature layout and bytecode identity. Invalidate incompatible native
PSOs and preload/scene records, or extend their schema/version; clearing only
in-memory `D3D12PipelineCache` is insufficient. Preserve the existing SraKmd
prohibition on driver pipeline libraries and cached PSO blobs. Bindless does not
make those driver paths safe.

## Effort and expected saving

Planning estimate: 2–4 engineer weeks for a guarded texture/sampler prototype,
4–8 weeks for a complete CBV/UAV/uniform ABI, cache migration and device
validation, assuming runtime support. Roughly 10–16 renderer/compiler/cache
files plus focused tests. Validation needs representative shader types, all
texture dimensions/comparison modes, streamout, internal passes, descriptor
pressure, device recreation, and fixed-scene image/query/readback parity.

Against the supplied **4.004 ms/frame descriptors** slot, a plausible full-ABI
saving is **1–3 ms/frame**, unmeasured. The absolute descriptor-slot ceiling is
4.004 ms; index-record preparation and root updates still cost time. A texture
only prototype saves less because transient uniform CBVs keep table work alive.
These savings overlap the round-3 table optimizations and must not be added to
them. Unsupported UWP devices save **0 ms** from SM 6.6 bindless. This proposal
alone does not justify a <=13 ms/frame claim.
