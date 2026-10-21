# Series S simulation

This opt-in diagnostic approximates Series S throughput on a Series X. It does
not change the hardware, memory budget, guest clock or audio pacing. Restart the
application after every configuration change.

On UWP, create `LocalState/sim_series_s.txt` (contents do not matter), or put this
in `LocalState/settings` or `LocalState/botw_env.txt`:

```ini
BOTW_SIM_SERIES_S=1
BOTW_SIM_GPU_FACTOR=3.0
BOTW_SIM_CPU_FACTOR=1.05
```

String values with these names in UWP `ApplicationData.LocalSettings` also work.
Text files override LocalSettings; `botw_env.txt` overrides `settings`. The flag
file enables simulation only if no explicit enable value is present. Only the
exact enable value `1` activates it. On desktop, use environment variables with
the same names. Factors alone do not enable simulation. Finite GPU factors in
`[1, 10]` and CPU factors in `[1, 2]` are accepted; invalid values use the defaults
above. Factor `1` adds no load for that component.

The existing output-size scale selection is capped at 1.5 (1080p internal), as
on a Series S. The swap-chain output resolution is unchanged.

The direct queue records timestamp pairs around command-list segments. At the
main present boundary, a DXC `cs_6_0` shader reads the current frame's timestamps
on the GPU, excluding presentation, gaps between submissions and prior ballast.
It adds a fixed 128-group, 64-thread integer ALU dispatch targeting
`(gpu_factor - 1) * active_gpu_time`. Completed dispatch timestamps calibrate
loops per GPU tick with a 25% exponential update. Initial calibration uses 4096
loops; targets are capped at 100 ms extra and 4,194,304 loops per thread. These
bounds are not a GPU watchdog guarantee.

Eight query/resource slots are polled using the existing submission fence. No
simulation code waits for a fence. If all slots are in flight, the next frame
skips simulation and increments `skipped`; it retries at the following frame.
Frames exceeding 1024 command-list segments are underweighted and logged.

The existing engine-frame hook measures the main guest thread's host interval,
subtracts waits recorded by `WaitMark`, then sleeps/spins for
`(cpu_factor - 1) * work_time`, capped at 100 ms. Its own added delay is excluded
from the next sample. The first frame primes measurement. Audio mixing, alarms
and guest time-base accounting are unchanged.

The runtime logs the enabled factors once and, every second of main presentation,
reports FPS, mean main-thread work ms, and GPU ms before/after ballast. GPU values
come from completed samples, so they may lag presentation; `samples`, `capped`
and `skipped` expose incomplete coverage and clamp pressure. CPU ms excludes the
added stretch. Disabled mode creates no GPU resources, compiles no ballast
shader, and adds no clocks, sleeps or GPU commands in the frame hooks.

This is ALU throughput stress, not an accurate model of Series S memory bandwidth,
cache, rasterization, occupancy or power behavior. Compare the same scene and
settings after calibration settles. On-device GPU captures and timings are still
required to establish convergence and useful performance estimates.

Portable validation:

```sh
cmake -S runtime/src/platform/tests -B build/platform-tests -G Ninja
cmake --build build/platform-tests
ctest --test-dir build/platform-tests --output-on-failure
```
