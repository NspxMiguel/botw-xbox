"""Preset selection checks using authored rules only; no community pack assets."""

import json
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

from tools.packs.build import load_manifest
from tools.packs.preset import write_manifest
from tools.packs.recompile import selection_config
from tools.packs.rules import PackError

ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture
def graphic_packs(tmp_path):
    # Independent fixture names ensure a typo in the implementation fails tests.
    categories = {
        "Mods/FPS++": {
            "Mode": ["Normal Settings", "Advanced Settings"],
            "FPS Limit": ["60FPS Limit (Default)", "30FPS Limit"],
        },
        "Graphics": {
            "Resolution": ["Synthetic native resolution"],
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


@pytest.mark.parametrize("console", ["series_s", "series_x"])
def test_manifest(console, graphic_packs, tmp_path):
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
    assert "Resolution" not in graphics
    assert graphics["Anti-Aliasing"] == "Normal FXAA (Default)"
    assert graphics["Shadows"] == (
        "Ultra (300%)" if console == "series_s" else "Extreme (400%, Unstable)"
    )
    assert graphics["Shadow Draw Distance"] == (
        "High (Default)" if console == "series_s" else "Very High (Recommended)"
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
    assert all(Path(p["path"]).is_absolute() for p in load_manifest(output)["packs"])


@pytest.mark.parametrize(
    "field, value", [("name", "t3nka Neutral+"), ("category", "Clarity")]
)
def test_missing_name_fails(field, value, graphic_packs, tmp_path):
    rules = graphic_packs / "Enhancements/rules.txt"
    rules.write_text(
        rules.read_text().replace(f'{field} = "{value}"', f'{field} = "Renamed"')
    )
    output = tmp_path / "selection.json"
    with pytest.raises(PackError, match=value.replace("+", r"\+")) as error:
        write_manifest("remaster", "series_x", graphic_packs, output)
    assert "Enhancements" in str(error.value)
    assert not output.exists()


def test_missing_optional_pack(graphic_packs, tmp_path, capsys):
    shutil.rmtree(graphic_packs / "Enhancements")
    manifest = write_manifest(
        "remaster", "series_s", graphic_packs, tmp_path / "selection.json"
    )
    assert len(manifest["packs"]) == 3
    warning = capsys.readouterr().err
    assert "warning" in warning and "Enhancements" in warning and "skipping" in warning


def test_fps_required(graphic_packs, tmp_path):
    shutil.rmtree(graphic_packs / "Mods/FPS++")
    with pytest.raises(PackError, match=r"required FPS\+\+"):
        write_manifest(
            "remaster", "series_x", graphic_packs, tmp_path / "selection.json"
        )


def test_existing_pack_missing_rules_fails(graphic_packs, tmp_path):
    (graphic_packs / "Graphics/rules.txt").unlink()
    with pytest.raises(PackError, match="Graphics.*rules.txt"):
        write_manifest(
            "remaster", "series_x", graphic_packs, tmp_path / "selection.json"
        )


def test_build_selection_uses_same_manifest(graphic_packs, tmp_path):
    args = SimpleNamespace(
        preset="remaster", console="series_x", graphic_packs=graphic_packs
    )
    config = selection_config(args)
    output = tmp_path / "manual.json"
    write_manifest("remaster", "series_x", graphic_packs, output)
    assert config == load_manifest(output)
    assert selection_config(SimpleNamespace(preset=None, packs=output)) == config


@pytest.mark.parametrize(
    "options",
    [
        ["--preset", "remaster"],
        ["--packs", "selection.json", "--console", "series_s"],
        ["--packs", "selection.json", "--preset", "remaster"],
    ],
)
def test_invalid_build_options(options):
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
