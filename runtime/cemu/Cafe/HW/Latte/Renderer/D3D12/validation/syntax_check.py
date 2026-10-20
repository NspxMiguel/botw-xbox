#!/usr/bin/env python3
"""Check the port without game files, generated code, linking, or a Windows host.

Prefer --xwin-root for actual Microsoft SDK/STL headers. Otherwise use real
MinGW D3D12/CRT/STL headers with a small, explicitly modified PCH, retaining the
MSVC target triple. No Renderer, Latte, COM, D3D12, DXGI, or shader APIs are mocked.
"""

import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default="/opt/homebrew/opt/llvm/bin/clang++")
    parser.add_argument("--xwin-root", type=Path)
    parser.add_argument(
        "--mingw-root",
        type=Path,
        default=Path("/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32"),
    )
    parser.add_argument("--dxc-include", type=Path, required=True)
    parser.add_argument("--spirv-cross-include", type=Path, required=True)
    parser.add_argument(
        "--dependency-include", type=Path, default=Path("/opt/homebrew/include")
    )
    parser.add_argument("--log", type=Path, default=Path("/tmp/botw-w2-syntax.log"))
    parser.add_argument("--uwp", action="store_true")
    args = parser.parse_args()
    backend = Path(__file__).resolve().parents[1]
    repo = next(p for p in backend.parents if (p / "runtime/cemu").is_dir())
    cemu = repo / "runtime/cemu"
    latte = cemu / "Cafe/HW/Latte"
    flags = [
        args.clang,
        "-target",
        "x86_64-pc-windows-msvc",
        "-std=c++20",
        "-fsyntax-only",
        "-Wno-everything",
        "-ferror-limit=0",
        "-DBOTWRECOMP",
        "-DENABLE_D3D12",
        "-DBOOST_NOWIDE_NO_LIB",
        "-DEMULATOR_VERSION_MAJOR=2",
        "-DEMULATOR_VERSION_MINOR=6",
        "-DEMULATOR_VERSION_PATCH=0",
        "-DEMULATOR_HASH=a53cf0e",
    ]
    if args.uwp:
        flags += ["-DCEMU_UWP", "-DWINAPI_FAMILY=WINAPI_FAMILY_APP"]
    sources = sorted(backend.glob("*.cpp"))
    sources += sorted((backend / "compat").rglob("*.cpp"))
    sources += [
        latte / name
        for name in [
            "Core/LatteShader.cpp",
            "Core/LatteShaderCache.cpp",
            "Core/LatteIndices.cpp",
            "LegacyShaderDecompiler/LatteDecompiler.cpp",
            "LegacyShaderDecompiler/LatteDecompilerEmitGLSL.cpp",
            "Renderer/Renderer.cpp",
            "Renderer/RendererOuputShader.cpp",
        ]
    ]
    with tempfile.TemporaryDirectory(prefix="botw-w2-") as work:
        overlay = Path(work)
        cross = args.spirv_cross_include.resolve()
        if (cross / "spirv_cross/spirv_hlsl.hpp").exists():
            flags += ["-I" + str(cross)]
        elif (cross / "spirv_hlsl.hpp").exists():
            (overlay / "spirv_cross").symlink_to(cross, target_is_directory=True)
        else:
            parser.error("SPIRV-Cross headers not found in --spirv-cross-include")
        if args.xwin_root:
            for name in [
                "crt/include",
                "sdk/include/ucrt",
                "sdk/include/um",
                "sdk/include/shared",
                "sdk/include/winrt",
            ]:
                flags += ["-isystem", str(args.xwin_root / name)]
            pch = cemu / "Common/precompiled.h"
            mode = "Microsoft SDK/STL (no PCH substitutions)"
        else:
            mingw = args.mingw_root.resolve()
            versions = sorted((mingw / "include/c++").iterdir())
            stl = versions[-1]
            flags += [
                "--sysroot=" + str(mingw),
                "-isystem",
                str(stl),
                "-isystem",
                str(stl / "x86_64-w64-mingw32"),
                "-isystem",
                str(mingw / "include"),
                "-U_MSC_VER",
                "-D__MINGW32__",
                "-D__MINGW64__",
                "-fms-extensions",
                "-fno-ms-compatibility",
                "-fgnuc-version=4.2.1",
                "-fdeclspec",
            ]
            # MSVC-specific PCH assumptions are outside W2 ownership. This overlay
            # selects Windows types with _WIN32, uses the public performance-counter
            # API, and replaces MSVC's private future helper. The original is untouched.
            text = (cemu / "Common/precompiled.h").read_text()
            for name in ["version.h", "platform.h", "enumFlags.h", "betype.h"]:
                text = text.replace(f'#include "{name}"', f'#include "Common/{name}"')
            text = text.replace("#if _MSC_VER", "#if defined(_WIN32)")
            text = text.replace("#if defined(_MSC_VER)", "#if defined(_WIN32)")
            text = (
                text.replace(
                    "static const long long _Freq = _Query_perf_frequency();",
                    "LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);\n"
                    "static const long long _Freq = frequency.QuadPart;",
                )
                .replace(
                    "const long long _Ctr = _Query_perf_counter();",
                    "LARGE_INTEGER counter; QueryPerformanceCounter(&counter);\n"
                    "const long long _Ctr = counter.QuadPart;",
                )
                .replace(
                    "return f._Is_ready();",
                    "return f.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;",
                )
            )
            pch = overlay / "Common/precompiled.h"
            pch.parent.mkdir()
            pch.write_text(text)
            mode = "MinGW real Windows headers/STL + validation PCH substitutions"
            if args.uwp and not (mingw / "include/fileapifromapp.h").exists():
                # MinGW omits this UWP SDK header. Only its one used declaration is
                # supplied; runtime behavior still requires Microsoft's Windows SDK.
                (overlay / "fileapifromapp.h").write_text(
                    "#pragma once\n#include <windows.h>\n"
                    'extern "C" __declspec(dllimport) HANDLE WINAPI CreateFileFromAppW('
                    "LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);\n"
                )
                mode += "; declaration-only CreateFileFromAppW shim"

        # GLM is included by the common PCH but unused by these translation units.
        if not (args.dependency_include / "glm/glm.hpp").exists():
            (overlay / "glm/gtc").mkdir(parents=True)
            (overlay / "glm/glm.hpp").touch()
            (overlay / "glm/gtc/quaternion.hpp").touch()
            mode += "; empty unused GLM includes"
        includes = [
            overlay,
            backend / "compat",
            repo / "runtime/cemu_shims",
            cemu,
            cemu / "Cafe",
            cemu / "dependencies/imgui",
            args.dxc_include,
            args.dependency_include,
        ]
        for directory in includes:
            flags += ["-I" + str(directory)]
        flags += ["-include", str(pch)]
        failures = 0
        with args.log.open("w") as log:
            log.write(mode + "\n" + " ".join(flags) + "\n")
            for source in sources:
                result = subprocess.run(
                    flags + [str(source)],
                    cwd=repo,
                    check=False,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                status = "PASS" if result.returncode == 0 else "FAIL"
                line = f"{source.relative_to(repo)}: {status}"
                print(line, flush=True)
                log.write(line + "\n" + result.stdout)
                log.flush()
                failures += result.returncode != 0
            summary = f"{len(sources) - failures}/{len(sources)} passed; {failures} failed. {mode}"
            print(summary)
            log.write(summary + "\n")
        return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
