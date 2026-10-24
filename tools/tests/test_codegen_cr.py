"""Execute CR code emitted by the recompiler against the bit-by-bit model."""

import os
import platform
import shutil
import subprocess
from pathlib import Path

import pytest

from cafe.analyze import Function, Switch
from cafe.codegen import Generator
from cafe.ppc import Insn

ROOT = Path(__file__).resolve().parents[2]


def extract_function(shift=3, begin=31, end=31, record=False, register=3):
    address = 0x02000000
    return Function(
        address,
        insns={
            address: Insn(address, 0, "mfcr", rD=register),
            address + 4: Insn(
                address + 4,
                0,
                "rlwinm",
                rD=register,
                rA=register,
                imm=shift,
                mb=begin,
                me=end,
                Rc=record,
            ),
            address + 8: Insn(address + 8, 0, "bclr", BO=20),
        },
    )


@pytest.mark.parametrize(
    "barrier",
    [
        "hook",
        "probe",
        "label",
        "entry",
        "branch",
        "switch",
        "source",
        "destination",
        "gap",
        "wide-mask",
        "nine-bit-mask",
    ],
)
def test_extract_observation_barriers(barrier):
    function = extract_function()
    generator = Generator(None, set())
    first = function.entry
    second = first + 4
    assert set(generator.cr_extracts(function)) == {first, second}
    if barrier == "hook":
        generator.hooks.add(first)
    elif barrier == "probe":
        generator.probes.add(second)
    elif barrier == "label":
        function.labels.add(second)
    elif barrier == "entry":
        generator.entries.add(second)
    elif barrier == "branch":
        function.insns[first + 16] = Insn(first + 16, 0, "bc", imm=second)
    elif barrier == "switch":
        function.switches[first + 16] = Switch(0, 1, [second])
    elif barrier == "source":
        function.insns[second].rD = 4
    elif barrier == "destination":
        function.insns[second].rA = 4
    elif barrier == "gap":
        function.insns[second + 8] = function.insns.pop(second)
    elif barrier == "wide-mask":
        function.insns[second].mb = 0
    elif barrier == "nine-bit-mask":
        function.insns[second].mb = 0
        function.insns[second].me = 8
    assert not generator.cr_extracts(function)


def test_extract_permits_observation_before_and_after_pair():
    function = extract_function()
    generator = Generator(None, set())
    generator.probes.add(function.entry)
    generator.hooks.add(function.entry + 4)
    assert set(generator.cr_extracts(function)) == {function.entry, function.entry + 4}


def cr_kernels():
    generator = Generator(None, set())
    functions = []
    for register in (3, 31):
        code = generator.simple(Insn(0, 0, "mfcr", rD=register))
        functions.append(
            f"__attribute__((noinline)) void generated_mfcr_{register}(PPCContext* c) {{"
            + "\n".join(code)
            + "}\n"
        )
    for mask in range(256):
        code = generator.simple(Insn(0, 0, "mtcrf", rD=3, imm=mask))
        functions.append(
            f"__attribute__((noinline)) void generated_mtcrf_{mask}(PPCContext* c) {{"
            + "\n".join(code)
            + "}\n"
        )
    return "\n".join(functions)


def extract_kernels():
    generator = Generator(None, set())
    baseline = Generator(None, set())
    baseline.cr_extracts = lambda _: {}
    output, pairs = [], []
    for shift in range(32):
        for begin, end in ((31, 31), (0, 3), (29, 2), (0, 7), (28, 3)):
            for record in (False, True):
                index = len(pairs)
                function = extract_function(
                    shift, begin, end, record, 3 if record else 31
                )
                assert generator.cr_extracts(function)
                names = []
                for prefix, emitter in (("folded", generator), ("unfolded", baseline)):
                    name = f"{prefix}_{index}"
                    code, _ = emitter.gen_function(function)
                    output.append(code.replace("f_02000000", name))
                    names.append(name)
                pairs.append("{" + ",".join(names) + "}")
    output.append("GuestFn extract_pairs[][2] = {" + ",".join(pairs) + "};")
    return "\n".join(output)


@pytest.mark.parametrize("variant", ["native-o1-sanitized", "native-o2", "x64-o2"])
def test_cr_semantics(tmp_path, variant):
    compiler = shutil.which(os.environ.get("CXX", "clang++"))
    if not compiler:
        pytest.skip("clang++ is required to execute emitted code")
    if variant == "x64-o2" and platform.system() != "Darwin":
        pytest.skip("the x64 variant uses Apple's cross-architecture driver")
    source = tmp_path / "cr.cpp"
    table = ",".join(f"generated_mtcrf_{mask}" for mask in range(256))
    source.write_text(
        '#include "ppc.h"\n#include <cassert>\n#include <cstdio>\n'
        "#define RETURN() return\nuint8_t* g_mem = nullptr;\n"
        + cr_kernels()
        + extract_kernels()
        + "\nGuestFn writers[] = {"
        + table
        + "};\n"
        + r"""
uint32_t random_word() {
    static uint32_t seed = 0x13579bdf;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
uint32_t pack(const PPCContext& c) {
    uint32_t value = 0;
    for (int bit = 0; bit < 32; ++bit)
        value |= uint32_t(c.cr[bit]) << (31 - bit);
    return value;
}
void check_read(uint32_t value) {
    PPCContext initial{};
    for (int bit = 0; bit < 32; ++bit)
        initial.cr[bit] = (value >> (31 - bit)) & 1;
    for (auto reader : {generated_mfcr_3, generated_mfcr_31}) {
        PPCContext actual = initial, expected = initial;
        expected.r[reader == generated_mfcr_3 ? 3 : 31] = value;
        reader(&actual);
        assert(memcmp(&actual, &expected, sizeof(actual)) == 0);
    }
}
int main() {
    check_read(0); check_read(0xffffffff);
    for (int bit = 0; bit < 32; ++bit) check_read(1u << bit);
    // Exhaust each 16-bit half, then cover independent combinations.
    for (uint32_t word = 0; word < 65536; ++word) {
        check_read(word); check_read(word << 16);
    }
    for (int iteration = 0; iteration < 2000; ++iteration) {
        uint32_t value = random_word();
        check_read(value);
        for (int mask = 0; mask < 256; ++mask) {
            PPCContext actual{};
            for (auto& byte : actual.cr) byte = random_word() & 1;
            for (auto& reg : actual.r) reg = random_word();
            actual.r[3] = value;
            actual.ca = 1; actual.so = 1; actual.ov = 1;
            actual.lr = 0x12345678; actual.ctr = 0xfedcba98;
            PPCContext expected = actual;
            for (int field = 0; field < 8; ++field)
                if (mask & (0x80 >> field))
                    for (int bit = 0; bit < 4; ++bit) {
                        int index = field * 4 + bit;
                        expected.cr[index] = (value >> (31 - index)) & 1;
                    }
            writers[mask](&actual);
            assert(memcmp(&actual, &expected, sizeof(actual)) == 0);
            generated_mfcr_3(&actual);
            assert(actual.r[3] == pack(expected));
        }
    }
    for (auto& pair : extract_pairs) {
        for (int iteration = 0; iteration < 300; ++iteration) {
            PPCContext initial{};
            for (auto& byte : initial.cr) byte = random_word() & 1;
            for (auto& reg : initial.r) reg = random_word();
            initial.so = random_word() & 1;
            initial.ca = random_word() & 1;
            initial.ov = random_word() & 1;
            PPCContext folded = initial, unfolded = initial;
            pair[0](&folded); pair[1](&unfolded);
            assert(memcmp(&folded, &unfolded, sizeof(folded)) == 0);
        }
    }
    puts("CR gather/scatter: exhaustive halves, all masks, full-context preservation passed");
}
"""
    )
    flags = ["-O2"]
    if variant == "native-o1-sanitized":
        flags = ["-O1", "-fsanitize=address,undefined"]
    elif variant == "x64-o2":
        flags += ["-arch", "x86_64", "-mavx2"]
    executable = tmp_path / "cr"
    subprocess.run(
        [
            compiler,
            "-std=c++20",
            "-include",
            "initializer_list",
            *flags,
            "-I",
            str(ROOT / "runtime/include"),
            str(source),
            "-o",
            str(executable),
        ],
        check=True,
    )
    subprocess.run([str(executable)], check=True, timeout=60)
