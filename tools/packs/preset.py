"""Generate validated selections referencing player-supplied graphic packs."""

import argparse
import json
import os
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.packs.rules import PackError, parse_rules, resolve_presets

CONSOLES = ("series_s", "series_x")
TITLE_ID = "00050000101c9400"

# Pack, category, Series S, Series X. None leaves that category at pack defaults.
# Native Resolution leaves render-target scaling to FSR. Texture LOD and
# anisotropy retain the pack defaults; D3D12 applies their unconditional rules.
REMASTER = (
    ("Mods/FPS++", "Mode", "Normal Settings", "Normal Settings"),
    ("Mods/FPS++", "FPS Limit", "60FPS Limit (Default)", "60FPS Limit (Default)"),
    ("Graphics", "Resolution", "1280x720 (HD, Default)", "1280x720 (HD, Default)"),
    ("Graphics", "Shadows", "Ultra (300%)", "Extreme (400%, Unstable)"),
    ("Graphics", "Shadow Draw Distance", "High (Default)", "Very High (Recommended)"),
    ("Graphics", "Anti-Aliasing", "Normal FXAA (Default)", "Normal FXAA (Default)"),
    ("Enhancements", "Clarity", "t3nka Neutral+", "t3nka Neutral+"),
    ("Enhancements", "Reflection Range", None, "Medium (8 samples)"),
    (
        "Mods/DrawDistance",
        "NPC, Enemies And Other Entities",
        "Medium (1x)",
        "High (1.25x)",
    ),
    (
        "Mods/DrawDistance",
        "Terrain, Buildings, Bushes And Other Objects",
        "Medium (1x)",
        "High (1.25x)",
    ),
    ("Mods/DrawDistance", "Trees (2D Billboards)", "High", "High"),
    ("Mods/DrawDistance", "Grass Blades Density", "Medium (Default)", "High"),
)
# No Workarounds are selected: AMDShaderCrash targets AMD OpenGL, GrassWorkaround
# targets Vulkan, CPUOcclusionQuery/KakarikoTorchShadows/LWZXNullCheck target
# OpenGL, NVIDIA fixes target a different vendor, and ReshadeCompatibility targets
# Vulkan with ReShade. None is a D3D12/AMD workaround.


def write_manifest(preset, console, graphic_packs, output):
    """Validate the complete selection before writing a version 1 manifest."""
    if preset != "remaster":
        raise PackError(f"unknown pack preset: {preset}")
    if console not in CONSOLES:
        raise PackError(f"unknown console: {console}")
    column = CONSOLES.index(console) + 2
    selections = {}
    for row in REMASTER:
        selections.setdefault(row[0], {})
        if row[column] is not None:
            selections[row[0]][row[1]] = row[column]
    output = Path(output).resolve()
    entries = []
    for folder, selected in selections.items():
        path = Path(graphic_packs).resolve() / folder
        if not path.is_dir():
            if folder == "Mods/FPS++":
                raise PackError(f"required FPS++ pack folder is missing: {path}")
            print(
                f"warning: optional pack folder is missing, skipping: {path}",
                file=sys.stderr,
            )
            continue
        try:
            sections = parse_rules((path / "rules.txt").read_text(encoding="utf-8-sig"))
            resolve_presets(sections, selected)
        except (PackError, OSError) as exc:
            raise PackError(f"{path}: {exc}") from exc
        entry = {
            "path": os.path.relpath(path, output.parent),
            "presets": selected,
        }
        if folder == "Mods/FPS++":
            entry["timing"] = True
        entries.append(entry)
    manifest = {"version": 1, "title_id": TITLE_ID, "packs": entries}
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("preset", choices=("remaster",))
    parser.add_argument("--console", required=True, choices=CONSOLES)
    parser.add_argument("graphic_packs", type=Path)
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()
    write_manifest(args.preset, args.console, args.graphic_packs, args.output)


if __name__ == "__main__":
    try:
        main()
    except (PackError, OSError) as exc:
        sys.exit(f"graphic packs: {exc}")
