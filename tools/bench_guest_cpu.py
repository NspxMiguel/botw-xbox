"""Build synthetic guest/AX microbenchmarks; never load a game or contact a console.

Run with `uv run python -I tools/bench_guest_cpu.py [--arch x86_64] [--opt 1]`.
x86_64 on Apple Silicon runs under Rosetta, not at Xbox CPU speed.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from cafe.analyze import Function
from cafe.codegen import Generator
from cafe.ppc import Insn


def kernels():
    generator = Generator(None, set())
    result = ['#include "ppc.h"']
    for name, instruction in (
        ("current_mfcr", Insn(0, 0, "mfcr", rD=3)),
        ("current_mtcrf", Insn(0, 0, "mtcrf", rD=3, imm=255)),
        ("current_mtcrf_partial", Insn(0, 0, "mtcrf", rD=3, imm=32)),
    ):
        result.append(
            f'extern "C" __attribute__((noinline)) void {name}(PPCContext* c) {{'
        )
        result.extend(generator.simple(instruction))
        result.append("}")
    result.append(
        'extern "C" __attribute__((noinline)) void baseline_mfcr(PPCContext* c) {'
    )
    result.append(
        "c->r[3] = "
        + " | ".join(f"((uint32_t)c->cr[{bit}] << {31 - bit})" for bit in range(32))
        + "; }"
    )
    for name, mask in (("baseline_mtcrf", 255), ("baseline_mtcrf_partial", 32)):
        result.append(
            f'extern "C" __attribute__((noinline)) void {name}(PPCContext* c) {{'
        )
        result.append("uint32_t v_ = c->r[3];")
        for bit in range(32):
            if mask & (0x80 >> (bit // 4)):
                result.append(f"c->cr[{bit}] = (v_ >> {31 - bit}) & 1;")
        result.append("}")
    # Separate translation unit prevents direct call elimination or constant folding.
    for index in range(64):
        result.append(
            f'extern "C" __attribute__((noinline)) void guest_{index}(PPCContext* c) '
            f"{{ c->r[3] += {index + 1}u; }}"
        )
    result.append(
        '#include "guest_table.h"\nconst GuestFnEntry g_guest_functions[] = {'
    )
    result.extend(
        f"{{0x{0x02000000 + index * 4:08x}, guest_{index}}}," for index in range(64)
    )
    result.append("};\nconst uint32_t g_guest_function_count = 64;")
    # Keep the rejected scatter candidate reproducible, outside production code.
    result.append(
        'extern "C" __attribute__((noinline)) void candidate_mtcrf(PPCContext* c) {'
    )
    result.append("uint32_t value = c->r[3];")
    for field in range(8):
        result.append(
            f"{{ uint32_t bits = (value >> {28 - field * 4}) & 15;"
            "bits = botw_bswap32((bits * 0x00204081u) & 0x01010101u);"
            f"memcpy(c->cr + {field * 4}, &bits, 4); }}"
        )
    result.append("}")
    address = 0x02000000
    function = Function(
        address,
        insns={
            address: Insn(address, 0, "mfcr", rD=3),
            address + 4: Insn(
                address + 4, 0, "rlwinm", rD=3, rA=3, imm=3, mb=31, me=31
            ),
            address + 8: Insn(address + 8, 0, "bclr", BO=20),
        },
    )
    result.append("#define RETURN() return")
    for name in ("current_extract", "unfolded_extract"):
        emitter = Generator(None, set())
        if name == "unfolded_extract":
            emitter.cr_extracts = lambda _: {}
        code, _ = emitter.gen_function(function)
        result.append(code.replace("f_02000000", name))
    return "\n".join(result) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"))
    parser.add_argument("--opt", choices=("1", "2"), default="2")
    parser.add_argument("--out", type=Path, default=ROOT / "build/60-hud-60fps-audit")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    suffix = f"{args.arch or 'native'}-o{args.opt}"
    source = args.out / f"kernels-{suffix}.cpp"
    source.write_text(kernels())
    executable = args.out / f"guest-cpu-{suffix}"
    command = [
        os.environ.get("CXX", "clang++"),
        "-std=c++20",
        f"-O{args.opt}",
        "-fwrapv",
        "-fno-strict-aliasing",
        "-ffp-contract=off",
        "-ffunction-sections",
        "-fdata-sections",
        "-I",
        str(ROOT / "runtime/include"),
        "-I",
        str(ROOT / "runtime/src"),
    ]
    if args.arch:
        command += ["-arch", args.arch]
    if args.arch == "x86_64":
        # Match the UWP guest target's /arch:AVX2 baseline.
        command += ["-mavx2"]
    command += [
        str(source),
        str(ROOT / "tools/tests/guest_cpu_bench.cpp"),
        str(ROOT / "runtime/src/dispatch.cpp"),
        str(ROOT / "runtime/src/snd/ax_mix.cpp"),
        "-Wl,-dead_strip",
        "-o",
        str(executable),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
