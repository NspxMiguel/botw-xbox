# AMD FidelityFX spatial fallback

`fsr1/ffx_a.h` and `fsr1/ffx_fsr1.h` are the official AMD FSR 1
v1.20210629 shader headers, copied unchanged from the offline dependency cache.
Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/master/ffx-fsr
MIT license: [LICENSE](LICENSE). No game data or proprietary Xbox SDK files.

Content pins (SHA-256):

- `ffx_a.h`: `f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb`
- `ffx_fsr1.h`: `93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5`

The D3D12 configure step verifies these hashes and embeds both headers in a
build-directory header. No Windows-only offline shader compiler is required;
the D3D12 output pipelines use the same linked D3DCompiler runtime as the backend.
These are **FSR 1**, not the FSR 3.1 SDK. The SDK integration requested for temporal
upscaling/frame interpolation is pending; see [docs/fsr.md](../../../docs/fsr.md).
The browser download of the SDK was denied in this session, and the shell has no
network access. No alternate download or unverified replacement was attempted.
