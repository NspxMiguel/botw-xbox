# GamePad display and controller-only input

UWP uses one CoreWindow swapchain for TV and DRC. The quick menu has a **GamePad
screen: Off / Corner / Swap** row. Off is the initial state on every launch; a
screen-mode change applies after Resume and does not restart the guest.

| Xbox input                           | Action                                                           |
| ------------------------------------ | ---------------------------------------------------------------- |
| Tap View                             | Wii U Minus (map), replayed long enough for a 30 Hz guest sample |
| Hold View for 0.5 seconds            | Toggle full-screen DRC / TV                                      |
| Hold View + Menu for 0.5 seconds     | Quick menu; takes priority over screen shortcuts                 |
| View + Y                             | Full-screen DRC / TV shortcut, retained for compatibility        |
| View + X                             | Touch the center of the GamePad screen                           |
| View + right-stick click (R3)        | Enter / leave motion-puzzle controls                             |
| Right stick in motion mode           | Pitch and roll the virtual GamePad                               |
| LT / RT in motion mode               | Turn the virtual GamePad around the vertical axis                |
| Left-stick click (L3) in motion mode | Recenter                                                         |

A short View press still opens the map. Host chords consume their buttons until
release, including a partial release of R3, so they do not also open the scope,
attack or open the map. Touch lasts at least 100 ms. Motion controls consume the
camera stick, triggers and recenter click while preserving movement, face buttons
and the game's cancel action. A two-line EN/PT reminder stays visible during
motion mode. Leaving motion mode restores normal right-stick aiming. Pause,
focus loss and controller removal neutralize input and clear motion mode.

## Display path

`GX2SetDRCBuffer` in `runtime/src/gx2/gx2_core.cpp` supplies DRC's sRGB flag through
`latte::set_scanout_srgb(false, ...)`; like the existing TV setup, its guest buffer
address is not the host render target. Actual display content comes from
`GX2CopyColorBufferToScanBuffer`, target 1 for TV and target 4 for DRC, and reaches
`LatteRenderTarget_itHLECopyColorBufferToScanBuffer`.

Previously the shim's controller/window screen selectors always returned false,
and Xbox's `gamepad_screen_requested()` flag had no renderer consumer. DRC copies
were discarded because Xbox has no second pad window. The UWP D3D12 path now
submits both sources to `DrawBackbufferQuad` with their original screen identity.
Metal and the standalone Cemu window selection remain unchanged.

The backend captures each source into its own RGBA8 texture at the effective
source resolution, including that screen's gamma, swizzle and output effects.
These are independent images: a later guest render-target write or retirement
cannot alter the last DRC image. The compositor scales those snapshots with the
plain copy shader, without applying gamma a second time. Descriptor replacement
advances the staging heap generation; old native textures retire behind their
last GPU submission. Both snapshots are released during renderer teardown.

Composition happens in `SwapBuffers_record`, after all TV/DRC copy commands, so
copy order cannot erase Corner. Swap selects DRC full-screen; a missing DRC frame
falls back to TV, and a DRC-only producer still presents on the main swapchain.
Corner is an aspect-preserving, top-right inset bounded by one third of the
output width and height, with a 2.5% short-edge margin. Resolution changes refit
both images. Frame generation is suppressed while Corner or Swap is active;
regular TV upscaling remains available. The snapshots add approximately 5.1 MiB
at 1280×720 plus 854×480, or 15.6 MiB at 2560×1440 plus 854×480, excluding driver
alignment and temporarily deferred resources during a source-resolution change.

## Touch and motion

BotW's shipping gameplay uses the TV's button-driven map, inventory and rune
menus. The GamePad touch interaction to account for is its screen-routing /
off-TV-play prompt; View+X supplies a center contact for it. No touch is injected
by ordinary confirm/cancel buttons. VPADRead now fills all three raw/filtered
VPADTPData records with valid 12-bit center coordinates and a contact bit.
VPADGetTPCalibratedPoint converts that sample to the 1280×720 VPAD logical space,
including in-place calls. This is a center tap, not a general touchscreen cursor.
The title/off-TV prompt and every gameplay menu still require console acceptance.

For background, the director's public account describes the removal of planned
touch gameplay features ([Nintendo Everything, March 2017](https://nintendoeverything.com/zelda-breath-of-the-wild-director-on-cutting-wii-u-touch-features/)).
That account supports the button-first design, not proof of every game flow.

`VPADRead` already exported `input::Motion` into accelerometer, gyro rate,
orientation and attitude fields. Real host sensor samples pass through the
existing Mahony filter in `runtime/src/input/motion.cpp`. The old virtual tilt
implementation had no caller enabling `set_tilt_mode`, a permanent real-source
latch, and only a spring-return 40-degree two-axis range. It could not substitute
for apparatus controls on Xbox.

View+R3 now explicitly enables apparatus controls without depending on an
unverified game-memory address or version-specific shrine-state hook. It feeds a
normalized quaternion, gravity, angular velocity in revolutions/second and
integrated angular displacement in revolutions into VPAD. Both stick axes tilt
gravity; triggers turn around gravity. All axes support full rotations, including
flipping a maze over. Release holds the pose; deadzones prevent drift. Time steps
are capped after a pause. A recently active real sensor takes priority, but after
500 ms without samples the virtual source becomes available again. Reset clears
sensor ownership, pose and sample timestamps.

Normal camera and bow/rune aiming continue to use the right stick with virtual
motion inactive. Automatic apparatus detection is not claimed: enter motion mode
after activating an apparatus with the normal action button, and leave it with
View+R3 when finished. Existing real-sensor calibration HLE stubs are unchanged.

## Validation and console acceptance

Host tests exercise screen selection and fallbacks, mode cycling, snapshot
metadata lifetime, corner geometry, View tap/hold/chord priority, partial releases,
controller motion-input consumption, EN/PT labels, three-axis full rotations,
holding/recentering, attitude orthonormality, tilt versus yaw gravity behavior,
sensor handoff and the actual VPAD guest-memory ABI for touch and motion. They use
synthetic memory and inputs; no game assets or console access are needed.

A human must check fresh launch and Resume in EN/PT; Off/Corner/Swap at
720p/1080p/1440p/4K; live DRC content and gamma; repeated source-resolution changes;
frame-generation opt-in followed by Corner/Swap; View taps opening the map once;
the off-TV prompt using View+X; map, inventory, rune selection and bow aiming;
Myahm Agana's maze (including upside down) and other apparatus
shrines with all three axes; mode reminder and recenter behavior; cancel/exit;
controller removal and suspend/resume while holding each shortcut; and memory /
frame-time costs with both snapshots allocated. No Xbox validation is automated.
