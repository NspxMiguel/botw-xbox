"""Preset selection checks using authored rules only; no community pack assets."""

import contextlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from tools.packs.build import drop_identity_dimensions, load_manifest
from tools.packs.preset import write_manifest
from tools.packs.recompile import selection_config
from tools.packs.rules import PackError, parse_rules, resolve_presets, resolve_runtime

ROOT = Path(__file__).resolve().parents[3]


def graphic_packs(tmp_path):
    # Independent fixture names ensure a typo in the implementation fails tests.
    categories = {
        "Mods/FPS++": {
            "Mode": ["Normal Settings", "Advanced Settings"],
            "FPS Limit": ["60FPS Limit (Default)", "30FPS Limit"],
        },
        "Graphics": {
            "Resolution": ["1280x720 (HD, Default)", "1920x1080"],
            "Shadows": ["Ultra (300%)", "Extreme (400%, Unstable)"],
            "Shadow Draw Distance": ["High (Default)", "Very High (Recommended)"],
            "Anti-Aliasing": ["Normal FXAA (Default)"],
        },
        "Enhancements": {
            "Clarity": ["Disabled", "t3nka Neutral+"],
            "Reflection Range": ["Low (4 samples, Default)", "Medium (8 samples)"],
            "Anisotropic Filtering": ["Synthetic default filtering"],
        },
        "Mods/DrawDistance": {
            "NPC, Enemies And Other Entities": ["Medium (1x)", "High (1.25x)"],
            "Terrain, Buildings, Bushes And Other Objects": [
                "Medium (1x)",
                "High (1.25x)",
            ],
            "Trees (2D Billboards)": ["Medium (Default)", "High"],
            "Grass Blades Density": ["Medium (Default)", "High"],
            "Texture Distance Detail (LOD)": ["Synthetic default LOD"],
        },
    }
    base = tmp_path / "player packs"
    for folder, presets in categories.items():
        path = base / folder
        path.mkdir(parents=True)
        lines = ["[Definition]", "version = 6", "titleIds = 00050000101c9400"]
        for category, names in presets.items():
            for index, name in enumerate(names):
                lines.extend(
                    [
                        "[Preset]",
                        f'category = "{category}"',
                        f'name = "{name}"',
                        f"default = {int(index == 0)}",
                    ]
                )
        (path / "rules.txt").write_text("\n".join(lines), encoding="utf-8")
    return base


class PresetTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.tmp_path = Path(work.name)
        self.graphic_packs = graphic_packs(self.tmp_path)

    def test_manifest(self):
        tmp_path = self.tmp_path
        graphic_packs = self.graphic_packs
        for console in ["series_s", "series_x"]:
            with self.subTest(console=console):
                output = tmp_path / "selection.json"
                result = subprocess.run(
                    [
                        sys.executable,
                        "-I",
                        str(ROOT / "tools/packs/preset.py"),
                        "remaster",
                        "--console",
                        console,
                        str(graphic_packs),
                        "-o",
                        str(output),
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                assert result.returncode == 0, result.stderr
                manifest = json.loads(output.read_text())
                assert manifest["version"] == 1
                assert manifest["title_id"] == "00050000101c9400"
                entries = {Path(p["path"]).name: p for p in manifest["packs"]}
                assert len(entries) == 4
                for entry in manifest["packs"]:
                    assert not Path(entry["path"]).is_absolute()
                    assert (output.parent / entry["path"] / "rules.txt").is_file()
                assert entries["FPS++"]["timing"] is True
                assert entries["FPS++"]["presets"] == {
                    "Mode": "Normal Settings",
                    "FPS Limit": "60FPS Limit (Default)",
                }
                graphics = entries["Graphics"]["presets"]
                assert graphics["Resolution"] == "1280x720 (HD, Default)"
                assert graphics["Anti-Aliasing"] == "Normal FXAA (Default)"
                assert graphics["Shadows"] == (
                    "Ultra (300%)"
                    if console == "series_s"
                    else "Extreme (400%, Unstable)"
                )
                assert graphics["Shadow Draw Distance"] == (
                    "High (Default)"
                    if console == "series_s"
                    else "Very High (Recommended)"
                )
                enhancements = entries["Enhancements"]["presets"]
                assert enhancements["Clarity"] == "t3nka Neutral+"
                assert "Anisotropic Filtering" not in enhancements
                if console == "series_x":
                    assert enhancements["Reflection Range"] == "Medium (8 samples)"
                else:
                    assert "Reflection Range" not in enhancements
                draw = entries["DrawDistance"]["presets"]
                assert "Texture Distance Detail (LOD)" not in draw
                assert draw["Trees (2D Billboards)"] == "High"
                assert draw["Grass Blades Density"] == (
                    "Medium (Default)" if console == "series_s" else "High"
                )
                for category in (
                    "NPC, Enemies And Other Entities",
                    "Terrain, Buildings, Bushes And Other Objects",
                ):
                    assert draw[category] == (
                        "Medium (1x)" if console == "series_s" else "High (1.25x)"
                    )
                assert all(
                    Path(p["path"]).is_absolute()
                    for p in load_manifest(output)["packs"]
                )

    def test_missing_name_fails(self):
        tmp_path = self.tmp_path
        graphic_packs = self.graphic_packs
        rules = graphic_packs / "Enhancements/rules.txt"
        original_rules = rules.read_text()
        for field, value in [("name", "t3nka Neutral+"), ("category", "Clarity")]:
            with self.subTest(field=field):
                rules.write_text(
                    original_rules.replace(
                        f'{field} = "{value}"', f'{field} = "Renamed"'
                    )
                )
                output = tmp_path / "selection.json"
                with self.assertRaisesRegex(
                    PackError, value.replace("+", r"\+")
                ) as error:
                    write_manifest("remaster", "series_x", graphic_packs, output)
                assert "Enhancements" in str(error.exception)
                assert not output.exists()

    def test_missing_optional_pack(self):
        tmp_path = self.tmp_path
        graphic_packs = self.graphic_packs
        shutil.rmtree(graphic_packs / "Enhancements")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            manifest = write_manifest(
                "remaster", "series_s", graphic_packs, tmp_path / "selection.json"
            )
        assert len(manifest["packs"]) == 3
        warning = stderr.getvalue()
        assert (
            "warning" in warning and "Enhancements" in warning and "skipping" in warning
        )

    def test_fps_required(self):
        tmp_path = self.tmp_path
        graphic_packs = self.graphic_packs
        shutil.rmtree(graphic_packs / "Mods/FPS++")
        with self.assertRaisesRegex(PackError, r"required FPS\+\+"):
            write_manifest(
                "remaster", "series_x", graphic_packs, tmp_path / "selection.json"
            )

    def test_existing_pack_missing_rules_fails(self):
        tmp_path = self.tmp_path
        graphic_packs = self.graphic_packs
        (graphic_packs / "Graphics/rules.txt").unlink()
        with self.assertRaisesRegex(PackError, "Graphics.*rules.txt"):
            write_manifest(
                "remaster", "series_x", graphic_packs, tmp_path / "selection.json"
            )

    def test_build_selection_uses_same_manifest(self):
        tmp_path = self.tmp_path
        graphic_packs = self.graphic_packs
        args = SimpleNamespace(
            preset="remaster", console="series_x", graphic_packs=graphic_packs
        )
        config = selection_config(args)
        output = tmp_path / "manual.json"
        write_manifest("remaster", "series_x", graphic_packs, output)
        assert config == load_manifest(output)
        assert selection_config(SimpleNamespace(preset=None, packs=output)) == config

    def test_invalid_build_options(self):
        for options in [
            ["--preset", "remaster"],
            ["--packs", "selection.json", "--console", "series_s"],
            ["--packs", "selection.json", "--preset", "remaster"],
        ]:
            with self.subTest(options=options):
                result = subprocess.run(
                    [
                        sys.executable,
                        "-I",
                        str(ROOT / "tools/packs/recompile.py"),
                        "unused.rpx",
                        "generated",
                        *options,
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                assert result.returncode == 2
                assert "error:" in result.stderr

    def test_remaster_evaluates_native_resolution(self):
        tmp_path = self.tmp_path
        graphic_packs = self.graphic_packs
        rules = graphic_packs / "Graphics/rules.txt"
        rules.write_text(
            rules.read_text()
            .replace(
                'name = "1280x720 (HD, Default)"\ndefault = 1',
                'name = "1280x720 (HD, Default)"\ndefault = 0\n$width = 1280\n$height = 720',
            )
            .replace(
                'name = "1920x1080"\ndefault = 0',
                'name = "1920x1080"\ndefault = 1\n$width = 1920\n$height = 1080',
            )
            + """
    [TextureRedefine]
    width = 1280
    height = 720
    overwriteWidth = ($width / 1280) * 1280
    overwriteHeight = ($height / 720) * 720
    """
        )
        manifest = write_manifest(
            "remaster", "series_s", graphic_packs, tmp_path / "selection.json"
        )
        selected = next(
            p["presets"]
            for p in manifest["packs"]
            if Path(p["path"]).name == "Graphics"
        )
        sections = parse_rules(rules.read_text())
        variables, _ = resolve_presets(sections, selected)
        textures, _ = resolve_runtime(sections, variables)
        assert variables["$width"] == 1280 and variables["$height"] == 720
        assert textures[0]["overwriteWidth"] == [1280]
        assert textures[0]["overwriteHeight"] == [720]

    def test_build_drops_identity_dimensions(self):
        shadow = {"width": [1024], "height": [1024], "overwriteWidth": [3072]}
        native = {
            "width": [1280],
            "height": [720],
            "overwriteWidth": [1280.49],
            "overwriteHeight": [720],
        }
        packs = [{"textures": [shadow, native]}]
        drop_identity_dimensions(packs)
        assert packs[0]["textures"] == [shadow]
        # A later identity must still cancel an overlapping earlier size override.
        reset = {"width": [1024], "overwriteWidth": [1024]}
        packs.append({"textures": [reset]})
        drop_identity_dimensions(packs)
        assert packs[1]["textures"] == [reset]
        # Identity dimensions cannot hide unsupported non-dimension overrides.
        fields = {"width": [1280], "overwriteWidth": [1280], "overwriteFormat": [1]}
        packs = [{"textures": [fields]}]
        drop_identity_dimensions(packs)
        assert packs[0]["textures"] == [{"width": [1280], "overwriteFormat": [1]}]

    def test_build_keeps_multi_value_dimension_filters(self):
        for dimension in ("width", "height", "depth"):
            with self.subTest(dimension=dimension):
                name = "overwrite" + dimension.capitalize()
                rule = {dimension: [1280, 640], name: [1280]}
                packs = [{"textures": [rule]}]
                drop_identity_dimensions(packs)
                self.assertEqual(
                    packs[0]["textures"], [{dimension: [1280, 640], name: [1280]}]
                )


if __name__ == "__main__":
    unittest.main()
