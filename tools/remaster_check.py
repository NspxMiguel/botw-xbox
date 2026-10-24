#!/usr/bin/env python3
"""Prepare Remaster ablations and run a human-authorized fixed-scene console check.

The default run is a dry run: no lock, keychain, Device Portal or console access.
Use `uv run --no-project --with pillow python tools/remaster_check.py --help`.
"""

import argparse
import copy
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# tools/dis.py is a CLI, not the standard library's dis module. Direct script
# invocation must not shadow it when dataclasses imports inspect.
sys.path = [item for item in sys.path if Path(item).resolve() != ROOT / "tools"]
sys.path.insert(0, str(ROOT))
from tools.packs.preset import write_manifest
from tools.packs.rules import PackError, parse_rules, resolve_presets

DEFAULT_TOOLS = Path.home() / ".local/share/nxbox/artifacts/compat-2026-10-18/_tools"
# Restore each category to the vanilla/default value; keep FPS++ and resolution
# constant. "Off" means remove the Remaster enhancement, not remove all shadows.
ABLATIONS = {
    "shadows": ("Graphics", "Shadows", "Medium (100%, Default)"),
    "shadow-distance": ("Graphics", "Shadow Draw Distance", "High (Default)"),
    "anti-aliasing": ("Graphics", "Anti-Aliasing", "None"),
    "clarity": ("Enhancements", "Clarity", "Disabled (Default)"),
    "reflections": ("Enhancements", "Reflection Range", "Low (4 samples, Default)"),
    "entities": ("Mods/DrawDistance", "NPC, Enemies And Other Entities", "Medium (1x)"),
    "terrain": (
        "Mods/DrawDistance",
        "Terrain, Buildings, Bushes And Other Objects",
        "Medium (1x)",
    ),
    "trees": ("Mods/DrawDistance", "Trees (2D Billboards)", "Medium (Default)"),
    "grass": ("Mods/DrawDistance", "Grass Blades Density", "Medium (Default)"),
}


def prepare(packs, output, helper=None):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    baseline = write_manifest("remaster", "series_x", packs, output / "baseline.json")
    variants = []
    for name in ("baseline", *ABLATIONS):
        manifest = copy.deepcopy(baseline)
        if name in ABLATIONS:
            folder, category, value = ABLATIONS[name]
            entry = next(
                (
                    p
                    for p in manifest["packs"]
                    if (output / p["path"]).resolve() == (packs / folder).resolve()
                ),
                None,
            )
            if entry is None:
                raise PackError(f"Missing required ablation pack: {folder}")
            entry["presets"][category] = value
        for entry in manifest["packs"]:
            sections = parse_rules(
                (output / entry["path"] / "rules.txt").read_text(encoding="utf-8-sig")
            )
            resolve_presets(sections, entry["presets"])
        selection = output / f"{name}.json"
        selection.write_text(json.dumps(manifest, indent=2) + "\n")
        variants.append(
            {
                "name": name,
                "manifest": str(selection),
                "appx": str(output / "packages" / f"{name}.appx"),
            }
        )
    # Exactly the same appx/config/route is repeated to expose scene noise.
    variants.append(dict(variants[0], name="baseline-repeat"))
    # These two reuse the baseline package; no PPC rebuild for render scale.
    variants.extend(
        dict(variants[0], name=f"internal-{height}", scale=scale)
        for height, scale in [(1080, 150), (2160, 300)]
    )
    config = json.loads(helper.read_text()) if helper else {}
    config.update(
        autoLaunch=True,
        debugHud=True,
        fps=60,
        renderScalePercent=200,
        upscaler=1,
        frameGeneration=1,
    )
    (output / "helper.json").write_text(json.dumps(config, indent=2) + "\n")
    plan = {"version": 1, "helper": str(output / "helper.json"), "cases": variants}
    (output / "plan.json").write_text(json.dumps(plan, indent=2) + "\n")
    return plan


class Runner:
    def __init__(self, execute=False):
        self.execute = execute

    def command(self, command, env=None, expect=None):
        command = [str(part) for part in command]
        print("+ " + shlex.join(command), flush=True)
        if not self.execute:
            return
        result = subprocess.run(
            command,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=True,
        )
        # Some legacy wrappers exit zero on HTTP failure. Validate their success
        # markers in addition to the process status.
        if expect and not re.search(expect, result.stdout):
            raise RuntimeError(
                f"Missing success marker {expect!r}: {result.stdout[-2000:]}"
            )
        print(result.stdout[-2000:], end="", flush=True)

    def sleep(self, seconds):
        print(f"+ wait {seconds}s", flush=True)
        if self.execute:
            time.sleep(seconds)


def package_fingerprint(case):
    """Validate the generated selection report paired with a human-built appx."""
    snapshot = json.loads(Path(case["appx"]).with_suffix(".packs.json").read_text())
    selection = json.loads(Path(case["manifest"]).read_text())
    if len(snapshot["packs"]) != len(selection["packs"]):
        raise ValueError("Package pack count does not match the case")
    for entry in selection["packs"]:
        matching = [
            pack
            for pack in snapshot["packs"]
            if all(
                pack["presets"].get(key) == value
                for key, value in entry["presets"].items()
            )
        ]
        if len(matching) != 1:
            raise ValueError(f"Package presets do not match {case['name']}")
    if not re.fullmatch(r"[a-f0-9]{64}", snapshot["fingerprint"]):
        raise ValueError("Invalid compiled pack fingerprint")
    return snapshot["fingerprint"]


def run(plan_path, output, tools, execute=False, only=None):
    if execute and not os.environ.get("TESTER_LOCK_HELD"):
        # Hold the existing console-wide lock for the entire experiment. The dry
        # run never executes this wrapper (or dp.py, which reads the keychain).
        subprocess.run(
            [
                "bash",
                str(tools / "lockrun.sh"),
                sys.executable,
                str(Path(__file__).resolve()),
                "run",
                str(plan_path.resolve()),
                "--out",
                str(output.resolve()),
                "--tools",
                str(tools.resolve()),
                "--execute",
                *(["--only", only] if only else []),
            ],
            check=True,
        )
        return
    plan = json.loads(plan_path.read_text())
    cases = [case for case in plan["cases"] if only is None or case["name"] == only]
    if not cases:
        raise ValueError(f"Unknown case: {only}")
    if execute:
        for case in cases:
            if not Path(case["appx"]).is_file():
                raise FileNotFoundError(case["appx"])
            case["fingerprint"] = package_fingerprint(case)
        output.mkdir(parents=True, exist_ok=False)
    else:
        print("DRY RUN: would hold lockrun.sh for all cases; PKG=BotWRecompiled")
    runner = Runner(execute)
    env = dict(os.environ, PKG="BotWRecompiled")
    try:
        installed = None
        for case in cases:
            name = case["name"]
            destination = output.resolve() / name
            print(f"CASE {name}: {case['manifest']}", flush=True)
            if execute:
                destination.mkdir()
            if installed != case["appx"]:
                runner.command(
                    [
                        "bun",
                        Path.home() / ".local/share/nxbox/tools/install-appx.ts",
                        case["appx"],
                    ],
                    env,
                )
                installed = case["appx"]
            config = json.loads(Path(plan["helper"]).read_text())
            config["renderScalePercent"] = case.get("scale", 200)
            helper = destination / "helper.json"
            if execute:
                helper.write_text(json.dumps(config, indent=2) + "\n")
            dp = [sys.executable, tools / "dp.py"]
            runner.command(
                [*dp, "botw-ctl.ts", "rm", "ftp-autostart", "LocalState"], env
            )
            runner.command(
                [*dp, "botw-ctl.ts", "push", helper, "LocalState"],
                env,
                r"pushed helper.json",
            )
            # Profiling creates GPU timestamps and the allocation audit, no ballast.
            profile = destination / "latte-profile.txt"
            if execute:
                profile.write_text("1\n")
            runner.command(
                [*dp, "botw-ctl.ts", "push", profile, "LocalState"],
                env,
                r"pushed latte-profile.txt",
            )
            runner.command([*dp, "botw-probe.ts"], env, r"launch 20[0-9]")
            runner.sleep(40)
            for _ in range(6):
                runner.command([*dp, "botw-ctl.ts", "press", "a"], env, r"pressed a")
                runner.sleep(8)
            runner.sleep(100)
            runner.command(
                [*dp, "botw-ctl.ts", "shot", destination / "end.png"],
                env,
                r"shot [0-9]+",
            )
            runner.command(
                [
                    *dp,
                    "botw-fs.ts",
                    "get",
                    "\\LocalState\\runtime.log",
                    destination / "runtime.log",
                ],
                env,
                r"get 200",
            )
            if execute:
                log = (destination / "runtime.log").read_text(errors="replace")
                if f"Graphic pack plan: {case['fingerprint']}" not in log:
                    raise ValueError(f"Wrong package/pack selection running for {name}")
    finally:
        if execute:
            runner.command(
                [
                    sys.executable,
                    tools / "dp.py",
                    "botw-ctl.ts",
                    "rm",
                    "latte-profile.txt",
                    "LocalState",
                ],
                env,
            )
    if execute and (output / "baseline/end.png").exists():
        report(output, None)


def metrics(log):
    fps = [float(v) for v in re.findall(r"D3D12: ([\d.]+) presents/s", log)][-3:]
    gpu = re.findall(
        r"GPU timing: mean ([\d.]+) ms, max ([\d.]+) ms, samples (\d+), capped (\d+), skipped (\d+)",
        log,
    )[-30:]
    gpu = [(float(a), float(b), int(n), int(c), int(s)) for a, b, n, c, s in gpu]
    memory = [
        (int(used), int(limit))
        for used, limit in re.findall(r"memory: (\d+) of (\d+) MB", log)
    ]
    samples = sum(row[2] for row in gpu)
    return {
        "fps": statistics.mean(fps) if fps else None,
        "gpu_mean_ms": sum(row[0] * row[2] for row in gpu) / samples
        if samples
        else None,
        "gpu_max_ms": max((row[1] for row in gpu), default=None),
        "gpu_samples": samples,
        "gpu_windows": len(gpu),
        "gpu_incomplete": sum(row[3] + row[4] for row in gpu),
        "peak_memory_mib": max((used for used, _ in memory), default=None),
        "memory_limit_mib": min((limit for _, limit in memory), default=None),
        "pack_fingerprint": re.findall(r"Graphic pack plan: ([a-f0-9]{64})", log)[-1:],
        "output": re.findall(r"output (\d+x\d+), render scale ([\d.]+)", log)[-1:],
        "swapchain_output": re.findall(
            r"D3D12: (?:actual swap chain|resized swap chain) (\d+x\d+)", log
        )[-1:],
    }


def image_diff(reference, candidate, crop=None, diff_path=None):
    from PIL import Image, ImageChops, ImageStat

    with Image.open(reference) as first, Image.open(candidate) as second:
        first, second = first.convert("RGB"), second.convert("RGB")
        if first.size != second.size:
            raise ValueError(
                f"Screenshot dimensions differ: {first.size} != {second.size}"
            )
        if crop:
            x, y, width, height = crop
            if (
                min(x, y) < 0
                or min(width, height) <= 0
                or x + width > first.width
                or y + height > first.height
            ):
                raise ValueError("Crop is outside the screenshot")
            first, second = (
                first.crop((x, y, x + width, y + height)),
                second.crop((x, y, x + width, y + height)),
            )
        diff = ImageChops.difference(first, second)
        if diff_path:
            diff.save(diff_path)
        histogram = diff.histogram()
        pixels = first.width * first.height
        return {
            "rgb_mae": statistics.mean(ImageStat.Stat(diff).mean),
            "channels_over_8_percent": 100
            * sum(sum(histogram[base + 9 : base + 256]) for base in (0, 256, 512))
            / (pixels * 3),
        }


def allocation_peak(log):
    live, total, peak = {}, [0, 0, 0], [0, 0, 0]
    for line in log.splitlines():
        allocation = re.search(
            r"RT allocation: id=(\S+) 1080=(\d+) 1440=(\d+) 2160=(\d+)", line
        )
        release = re.search(r"RT release: id=(\S+)", line)
        if allocation:
            key, *sizes = allocation.groups()
            if key in live:
                raise ValueError("Duplicate live allocation ID")
            sizes = list(map(int, sizes))
            if any(v >= 2**63 for v in sizes):
                raise ValueError("Invalid driver allocation forecast")
            live[key] = sizes
            total = [a + b for a, b in zip(total, sizes)]
            peak = [max(a, b) for a, b in zip(peak, total)]
        elif release and release[1] in live:
            total = [a - b for a, b in zip(total, live.pop(release[1]))]
    return [round(v / 2**20, 2) for v in peak]


def report(output, crop):
    rows = []
    baseline = output / "baseline" / "end.png"
    for path in sorted(output.iterdir()):
        if not path.is_dir() or not (path / "runtime.log").is_file():
            continue
        log = (path / "runtime.log").read_text(errors="replace")
        row = {
            "case": path.name,
            **metrics(log),
            "rt_cohort_peak_mib_1080_1440_2160": allocation_peak(log),
        }
        if path.name != "baseline":
            row.update(image_diff(baseline, path / "end.png", crop, path / "diff.png"))
        # Approval needs both timing and process memory, plus a complete sample.
        row["within_budget"] = (
            row["gpu_mean_ms"] is not None
            and row["gpu_samples"] > 0
            and row["gpu_windows"] == 30
            and row["gpu_max_ms"] <= 1000 / 60
            and row["gpu_incomplete"] == 0
            and row["peak_memory_mib"] is not None
            and row["peak_memory_mib"] <= 5120 - 256
            and row["memory_limit_mib"] == 5120
        )
        rows.append(row)
    # Only report a candidate from baseline/render-scale runs, never infer it
    # from an ablation with cheaper shadows/grass. Human visual review is still
    # required before applying a candidate as a release default.
    control = next(
        (row for row in rows if row["case"] == "internal-1080"),
        next((row for row in rows if row["case"] == "baseline"), {}),
    )
    candidates = []
    for row in rows:
        if (
            row["case"] not in ("baseline", "internal-1080", "internal-2160")
            or not row["within_budget"]
        ):
            continue
        expected = {"baseline": 2.0, "internal-1080": 1.5, "internal-2160": 3.0}[
            row["case"]
        ]
        if (
            row["output"] == [("3840x2160", f"{expected:.2f}")]
            and row["swapchain_output"] == ["3840x2160"]
            and row["fps"]
            and control.get("fps")
            and row["fps"] >= control["fps"] * 0.97
        ):
            candidates.append(expected)
    candidate = max(candidates, default=None)
    (output / "scale-candidate.json").write_text(
        json.dumps({"scale": candidate, "requires_visual_review": True}, indent=2)
        + "\n"
    )
    (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
    lines = [
        "# Remaster category check",
        "",
        "Visual change is evidence, not a correctness verdict. Compare baseline-repeat noise and inspect diff.png.",
        "",
        "| Case | FPS | GPU mean / max ms | Peak MiB | RGB MAE | Within budget |",
        "|---|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        lines.append(
            f"| {row['case']} | {row['fps']} | {row['gpu_mean_ms']} / {row['gpu_max_ms']} | {row['peak_memory_mib']} | {row.get('rgb_mae', '—')} | {row['within_budget']} |"
        )
    lines += [
        "",
        f"Highest measured budget candidate: {candidate}. Human visual review required.",
    ]
    (output / "report.md").write_text("\n".join(lines) + "\n")
    return rows


def memory_projection(bpp=96):
    # Planning envelope, NOT a measured target inventory. Fit the non-scaling
    # component to the supplied 4555 MiB @ 1080p baseline, keep shadows fixed.
    base_pixels = 1920 * 1080
    swap_1080 = base_pixels * 4 * 3 / 2**20
    swap_4k = 3840 * 2160 * 4 * 3 / 2**20
    fixed = 4555 - base_pixels * bpp / 2**20 - swap_1080
    return [
        {
            "internal_height": height,
            "rt_mib": round((height * 16 // 9) * height * bpp / 2**20, 2),
            "swap_4k_mib": round(swap_4k, 2),
            "process_mib": round(
                fixed + (height * 16 // 9) * height * bpp / 2**20 + swap_4k, 2
            ),
        }
        for height in (1080, 1440, 2160)
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    prep.add_argument("--packs", required=True, type=Path)
    prep.add_argument("--out", required=True, type=Path)
    prep.add_argument("--helper", type=Path)
    bench = commands.add_parser("run")
    bench.add_argument("plan", type=Path)
    bench.add_argument("--out", required=True, type=Path)
    bench.add_argument("--tools", type=Path, default=DEFAULT_TOOLS)
    bench.add_argument(
        "--execute",
        action="store_true",
        help="Human-only opt-in: acquire the console lock and access Device Portal",
    )
    bench.add_argument("--only", help="Run a single named case")
    summary = commands.add_parser("report")
    summary.add_argument("out", type=Path)
    summary.add_argument(
        "--crop",
        type=lambda text: tuple(map(int, text.split(","))),
        help="x,y,width,height in screenshot pixels, exclude HUD",
    )
    memory = commands.add_parser("memory")
    memory.add_argument("--bytes-per-pixel", type=int, default=96)
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.packs, args.out, args.helper)
    elif args.command == "run":
        run(args.plan, args.out, args.tools, args.execute, args.only)
    elif args.command == "report":
        report(args.out, args.crop)
    else:
        print(json.dumps(memory_projection(args.bytes_per_pixel), indent=2))


if __name__ == "__main__":
    main()
