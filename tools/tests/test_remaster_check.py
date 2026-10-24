"""Synthetic Remaster check fixtures; never contact a console or read credentials."""

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.remaster_check import (
    ABLATIONS,
    allocation_peak,
    image_diff,
    memory_projection,
    metrics,
    package_fingerprint,
    prepare,
    report,
    run,
)
from tools.packs.preset import REMASTER


class RemasterCheckTests(unittest.TestCase):
    def test_one_category_changes_and_dry_run_has_no_external_effects(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            packs = root / "packs"
            categories = {}
            for folder, category, _, name in REMASTER:
                if name:
                    categories.setdefault(folder, {}).setdefault(category, []).append(
                        name
                    )
            for folder, category, name in ABLATIONS.values():
                categories[folder][category].append(name)
            for folder, choices in categories.items():
                path = packs / folder
                path.mkdir(parents=True)
                lines = ["[Definition]", "version = 6", "titleIds = 00050000101c9400"]
                for category, names in choices.items():
                    for index, name in enumerate(dict.fromkeys(names)):
                        lines += [
                            "[Preset]",
                            f'category = "{category}"',
                            f'name = "{name}"',
                            f"default = {int(index == 0)}",
                        ]
                (path / "rules.txt").write_text("\n".join(lines))
            plan = prepare(packs, root / "prepared")
            baseline = json.loads((root / "prepared/baseline.json").read_text())
            package = root / "baseline.appx"
            sidecar = package.with_suffix(".packs.json")
            sidecar.write_text(
                json.dumps({"fingerprint": "a" * 64, "packs": baseline["packs"]})
            )
            case = dict(plan["cases"][0], appx=str(package))
            self.assertEqual(package_fingerprint(case), "a" * 64)
            with self.assertRaises(ValueError):
                package_fingerprint(dict(case, manifest=plan["cases"][1]["manifest"]))
            self.assertEqual(len(plan["cases"]), 13)
            for case in plan["cases"]:
                selection = json.loads(Path(case["manifest"]).read_text())
                changed = [
                    (key, value)
                    for original, new in zip(baseline["packs"], selection["packs"])
                    for key, value in new["presets"].items()
                    if original["presets"][key] != value
                ]
                self.assertEqual(len(changed), int(case["name"] in ABLATIONS))
                self.assertEqual(
                    selection["packs"][0]["presets"]["FPS Limit"],
                    "60FPS Limit (Default)",
                )
            # Dry run does not need packages/tools to exist and cannot call any
            # subprocess, sleep, keychain, lockrun or Device Portal function.
            capture = io.StringIO()
            with (
                patch("subprocess.run", side_effect=AssertionError("external command")),
                patch("time.sleep", side_effect=AssertionError("sleep")),
                contextlib.redirect_stdout(capture),
            ):
                run(
                    root / "prepared/plan.json", root / "results", root / "absent-tools"
                )
            self.assertFalse((root / "results").exists())
            self.assertEqual(capture.getvalue().count("CASE "), 13)
            self.assertEqual(capture.getvalue().count("botw-probe.ts"), 13)
            self.assertEqual(capture.getvalue().count("press a"), 13 * 6)

    def test_missing_gpu_data_cannot_approve_budget(self):
        self.assertIsNone(metrics("")["gpu_mean_ms"])
        data = metrics(
            "D3D12: 43.1 presents/s\nGPU timing: mean 5.000 ms, max 8.000 ms, samples 40, capped 0, skipped 0\nGPU timing: mean 7.000 ms, max 11.000 ms, samples 20, capped 1, skipped 2\nmemory: 4800 of 5120 MB"
        )
        self.assertAlmostEqual(data["gpu_mean_ms"], 340 / 60)
        self.assertEqual(data["gpu_max_ms"], 11)
        self.assertEqual(data["gpu_incomplete"], 3)
        self.assertEqual(data["peak_memory_mib"], 4800)

    def test_allocation_lifetimes_are_not_cumulative_allocations(self):
        trace = "\n".join(
            [
                "RT allocation: id=0x1 1080=1048576 1440=2097152 2160=4194304 scaled=true",
                "RT allocation: id=0x2 1080=1048576 1440=1048576 2160=1048576 scaled=false",
                "RT release: id=0x1",
                "RT release: id=0xff",
                "RT allocation: id=0x1 1080=1048576 1440=2097152 2160=4194304 scaled=true",
            ]
        )
        self.assertEqual(allocation_peak(trace), [2, 3, 5])
        with self.assertRaises(ValueError):
            allocation_peak(trace + "\nRT allocation: id=0x1 1080=1 1440=1 2160=1")

    def test_memory_envelope_requires_1440p_default(self):
        rows = memory_projection()
        self.assertAlmostEqual(rows[0]["process_mib"], 4626.19, places=2)
        self.assertLess(rows[1]["process_mib"], 5120 - 256)
        self.assertGreater(rows[2]["process_mib"], 5120)
        self.assertLess(memory_projection(128)[1]["process_mib"], 5120 - 256)

    def test_diff_and_report_with_authored_images(self):
        from PIL import Image

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ["baseline", "grass"]:
                path = root / name
                path.mkdir()
                Image.new(
                    "RGB", (8, 8), (0, 0, 0) if name == "baseline" else (16, 16, 16)
                ).save(path / "end.png")
                (path / "runtime.log").write_text(
                    "D3D12: 43.0 presents/s\nmemory: 4700 of 5120 MB"
                )
            diff = image_diff(
                root / "baseline/end.png", root / "grass/end.png", (0, 2, 8, 6)
            )
            self.assertEqual(diff["rgb_mae"], 16)
            self.assertEqual(diff["channels_over_8_percent"], 100)
            rows = report(root, None)
            self.assertTrue(all(not row["within_budget"] for row in rows))
            self.assertTrue((root / "grass/diff.png").exists())
            Image.new("RGB", (9, 8)).save(root / "grass/end.png")
            with self.assertRaises(ValueError):
                image_diff(root / "baseline/end.png", root / "grass/end.png")

    def test_scale_candidate_needs_complete_timing_memory_and_control_fps(self):
        from PIL import Image

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, scale in [
                ("baseline", 2),
                ("internal-1080", 1.5),
                ("internal-2160", 3),
                ("grass", 2),
            ]:
                path = root / name
                path.mkdir()
                Image.new("RGB", (8, 8)).save(path / "end.png")
                log = (
                    f"output 3840x2160, render scale {scale:.2f}\n"
                    "D3D12: actual swap chain 3840x2160, 3 buffers, format 28\n"
                    "D3D12: 43.0 presents/s\nmemory: 4800 of 5120 MB\n"
                    + "GPU timing: mean 8.000 ms, max 10.000 ms, samples 40, capped 0, skipped 0\n"
                    * 30
                )
                (path / "runtime.log").write_text(log)
            report(root, None)
            self.assertEqual(
                json.loads((root / "scale-candidate.json").read_text())["scale"], 3
            )
            log_path = root / "internal-2160/runtime.log"
            log = log_path.read_text()
            log_path.write_text(log.replace("4800 of", "4900 of"))
            report(root, None)
            self.assertEqual(
                json.loads((root / "scale-candidate.json").read_text())["scale"], 2
            )
            log_path.write_text(log.replace("43.0 presents", "40.0 presents"))
            report(root, None)
            self.assertEqual(
                json.loads((root / "scale-candidate.json").read_text())["scale"], 2
            )
            log_path.write_text(log.replace("max 10.000", "max 18.000"))
            report(root, None)
            self.assertEqual(
                json.loads((root / "scale-candidate.json").read_text())["scale"], 2
            )
            log_path.write_text(log.replace("skipped 0", "skipped 1"))
            report(root, None)
            self.assertEqual(
                json.loads((root / "scale-candidate.json").read_text())["scale"], 2
            )


if __name__ == "__main__":
    unittest.main()
