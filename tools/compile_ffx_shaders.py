#!/usr/bin/env python3
"""Compile SDK 1.1.4 FSR3 DXIL permutations on the build host, never on Xbox."""

import argparse
import os
import re
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine", default="")
    parser.add_argument("--bottle-path", type=Path)
    args = parser.parse_args()
    sdk = args.source.resolve() / "sdk"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = sdk / "tools/binary_store/FidelityFX_SC.exe"
    env = os.environ.copy()
    prefix = []
    if args.wine:
        # CrossOver's bottle is isolated inside the build tree.
        env["CX_BOTTLE_PATH"] = str(args.bottle_path.resolve())
        env["WINEDEBUG"] = "-all"
        prefix = [
            args.wine,
            "--bottle",
            "ffx",
            "--workdir",
            str(sdk / "include/FidelityFX/gpu"),
            "--no-convert",
        ]

    def path(value):
        value = str(value)
        return "Z:" + value if args.wine else value

    gpu = sdk / "include/FidelityFX/gpu"

    def expand(source, stack=()):
        # The shipped compiler's include handler fails to resolve SDK paths on
        # Wine's Z: drive. Inline SDK includes, retaining all preprocessor guards
        # and permutation macros; DXC still does the actual preprocessing.
        if source in stack:
            raise RuntimeError(f"Recursive shader include: {source}")

        def include(match):
            name = match[1]
            for root in (source.parent, gpu, gpu / "fsr3upscaler"):
                target = root / name
                if target.is_file():
                    return expand(target, stack + (source,))
            return match[0]

        return re.sub(
            r'^\s*#include\s+["<]([^">]+)[">].*$',
            include,
            source.read_text(),
            flags=re.MULTILINE,
        )

    base = [
        "-reflection",
        "-deps=gcc",
        "-num-threads=2",
        "-DFFX_GPU=1",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
        "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
        "-E",
        "CS",
        "-Wno-for-redefinition",
        "-Wno-ambig-lit-shift",
        "-DFFX_HLSL=1",
    ]
    for option in [
        "REPROJECT_USE_LANCZOS_TYPE",
        "HDR_COLOR_INPUT",
        "LOW_RESOLUTION_MOTION_VECTORS",
        "JITTERED_MOTION_VECTORS",
        "INVERTED_DEPTH",
        "APPLY_SHARPENING",
    ]:
        base.append(f"-DFFX_FSR3UPSCALER_OPTION_{option}={{0,1}}")
    base += [
        "-I",
        path(gpu),
        "-I",
        path(gpu / "fsr3upscaler"),
        "-output=" + path(output),
    ]
    for shader in sorted(
        (sdk / "src/backends/dx12/shaders/fsr3upscaler").glob("*.hlsl")
    ):
        expanded = output / "sources" / shader.name
        expanded.parent.mkdir(parents=True, exist_ok=True)
        expanded.write_text(expand(shader))
        for wave64, half in [
            (False, False),
            (True, False),
            (False, True),
            (True, True),
        ]:
            name = (
                shader.stem + ("_wave64" if wave64 else "") + ("_16bit" if half else "")
            )
            options = [
                f"-name={name}",
                f"-DFFX_HALF={int(half)}",
                "-T",
                "cs_6_6" if wave64 else "cs_6_2",
                f"-DFFX_HLSL_SM={66 if wave64 else 62}",
            ]
            if wave64:
                options.append("-DFFX_PREFER_WAVE64=[WaveSize(64)]")
            if half:
                options.append("-enable-16bit-types")
            print(f"Compiling {name}", flush=True)
            subprocess.run(
                prefix + [str(compiler)] + base + options + [path(expanded)],
                env=env,
                cwd=gpu,
                check=True,
            )
    (output / "compiled.stamp").touch()


if __name__ == "__main__":
    main()
