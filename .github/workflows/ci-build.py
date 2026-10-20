"""Enforce the game-free CI build contract before configuring or packaging."""

import argparse
import re
import sys
from pathlib import Path

GAME_SUFFIXES = {
    ".rpx",
    ".rpl",
    ".wud",
    ".wux",
    ".tik",
    ".tmd",
    ".h3",
    ".sarc",
    ".pack",
    ".sbfres",
    ".bfres",
    ".key",
}


def audit(root: Path, *, checkout: bool = False) -> None:
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if checkout and (
            relative.parts[0] in {".git", "build", ".venv"}
            or relative.parts[0].startswith("build-")
        ):
            continue
        if path.is_symlink():
            raise ValueError(f"CI input must not contain symlinks: {relative}")
        if path.is_file() and (
            path.suffix.lower() in GAME_SUFFIXES
            or "generated" in {part.lower() for part in relative.parts}
            or (
                "game" in {part.lower() for part in relative.parts}
                and not (checkout and relative.parts[:3] == ("runtime", "src", "game"))
            )
            or re.fullmatch(r"(?:fn|big|huge)_[0-9a-fA-F]+\.cpp", path.name)
        ):
            raise ValueError(
                f"Game data or generated guest code is forbidden in CI: {relative}"
            )


def check_contract(root: Path) -> None:
    audit(root, checkout=True)
    sources = [root / "CMakeLists.txt", *root.glob("cmake/**/*.cmake")]
    text = "\n".join(
        re.sub(r"#[^\n]*", "", path.read_text()) for path in sources if path.is_file()
    )
    required = {
        "BOTW_BUILD_GAME": r"(?:option\s*\(\s*BOTW_BUILD_GAME\b|set\s*\(\s*BOTW_BUILD_GAME\b[^)]*\bCACHE\b)",
        "BOTW_PLATFORM": r"set\s*\(\s*BOTW_PLATFORM\b[^)]*\bCACHE\b",
        "BOTW_RENDERER": r"set\s*\(\s*BOTW_RENDERER\b[^)]*\bCACHE\b",
    }
    missing = [
        name
        for name, pattern in required.items()
        if not re.search(pattern, text, re.IGNORECASE)
    ]
    if missing:
        raise ValueError(
            "Pending CMake integration: declare "
            + ", ".join(missing)
            + ". CI requires BOTW_BUILD_GAME=OFF to build the real runtime and "
            "BotWRecomp launcher without generated/ or a guest target. "
            "See docs/CI.md. No stub game code may be generated."
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["check-contract", "audit-package"])
    parser.add_argument("root", type=Path, nargs="?", default=Path.cwd())
    args = parser.parse_args()
    try:
        if not args.root.is_dir():
            raise ValueError(f"Required input directory does not exist: {args.root}")
        if args.command == "check-contract":
            check_contract(args.root)
        else:
            audit(args.root)
    except ValueError as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
