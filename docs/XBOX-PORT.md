# Xbox Series port — team contract

Goal: run this recompilation on Xbox Series X|S as a UWP app with D3D12, before 2026-10-24 23:00.
Each agent works in its own git worktree of branch `xbox/port`, owns only its paths, never commits.
Legal: no game files, no generated code from the game in the repo (same rule as upstream).

## Ownership
- W1 platform: `runtime/src/mem.*`, `runtime/src/thread.*`, `runtime/src/debug_watch.*`, file-system path code,
  and a new `runtime/src/platform/` (Win32/UWP implementations behind small interfaces; macOS keeps working).
- W2 renderer: new `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/` implementing the same `Renderer`
  interface as `Renderer/Metal/`, plus the shader path GLSL (Cemu's emitter) -> SPIR-V (glslang) ->
  HLSL (SPIRV-Cross) -> DXIL (DXC). Do not edit Metal/.
- W3 host: new `runtime/src/host/uwp/` (CoreWindow, Windows.Gaming.Input or GameInput, XAudio2, game-file
  location in LocalState or an external drive, launcher menu), replacing SDL3/AppKit on UWP only.
- Build: `CMakeLists.txt` gets `BOTW_PLATFORM=apple|win32|uwp` and `BOTW_RENDERER=metal|d3d12`; each agent
  adds only its own source lists inside clearly marked blocks.

## Player experience (owner's spec)
First launch: menu with select the game (only if not found automatically), settings, mods. Setup asks
"next time, open the game automatically?" — if yes, later launches go straight in. Select + Start in
game returns to the menu (same gesture as NXbox).

## Mods (owner's request, 2026-10-20)
- Brazilian Portuguese translation and dubbing mods must work. They replace game files (message
  archives under content/, voice/sound archives). Players supply the mods; we ship none.
- Graphic packs (Cemu format) and file-replacement mods load from the mods folder chosen in the
  launcher; later packs override earlier ones; each applied file is logged once.
