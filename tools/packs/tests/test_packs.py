"""Synthetic-only parser, assembler and recompiler integration checks."""

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from cafe.analyze import Analyzer
from cafe.codegen import Generator
from cafe.image import Image
from cafe.rpx import Reloc, Symbol
from tools.packs.build import apply_to_image, emit_snapshot, prepare
from tools.packs.checksum import module_checksum
from tools.packs.patches import Assembler, Write, apply_writes, compile_patch, layout
from tools.packs.recompile import configure_hook
from tools.packs.rules import (
    PackError,
    evaluate,
    parse_rules,
    resolve_presets,
    resolve_runtime,
)

RULES = """[Definition]
name = Synthetic timing
version = 6
titleIds = 0000000000000001
[Default]
$width = 1280
$height = 720
$advanced:int = 0
$fps = 30
[Preset]
name = Normal
category = Mode
default = 1
$advanced:int = 0
[Preset]
name = Advanced
category = Mode
$advanced:int = 1
[Preset]
name = 60 FPS
category = Limit
condition = $advanced == 1
default = 1
$fps = 60
[Preset]
name = HD
category = Resolution
[Preset]
name = Full HD
category = Resolution
$width = 1920
$height = 1080
[TextureRedefine]
width = 1280
height = 720
formats = 0x1a, 0x816
formatsExcluded = 0x41a
overwriteWidth = ($width / 1280) * 640
overwriteHeight = $height
[TextureRedefine]
width = 640
overwriteWidth = $width / 2
[Control]
vsyncFrequency = $fps
"""


def synthetic_elf(compressed=False):
    """A tiny executable containing only authored nop/blr instructions."""
    import zlib

    code = struct.pack(">4I", 0x60000000, 0x4E800020, 0, 0)
    stored = struct.pack(">I", len(code)) + zlib.compress(code) if compressed else code
    names = b"\0.text\0.shstrtab\0"
    raw = bytearray(0x100 + len(stored) + len(names))
    raw[:16] = b"\x7fELF\x01\x02\x01" + bytes(9)
    struct.pack_into(
        ">HHIIIIIHHHHHH", raw, 16, 2, 20, 1, 0x02000000, 0, 52, 0, 52, 0, 0, 40, 3, 2
    )
    struct.pack_into(
        ">10I",
        raw,
        92,
        1,
        1,
        6 | (0x08000000 if compressed else 0),
        0x02000000,
        0x100,
        len(stored),
        0,
        0,
        4,
        0,
    )
    struct.pack_into(
        ">10I", raw, 132, 7, 3, 0, 0, 0x100 + len(stored), len(names), 0, 0, 1, 0
    )
    raw[0x100 : 0x100 + len(stored)] = stored
    raw[0x100 + len(stored) :] = names
    return bytes(raw)


class RulesTests(unittest.TestCase):
    def test_repeated_sections_comments_bom(self):
        sections = parse_rules(
            "\ufeff"
            + RULES.replace("Synthetic timing", '"Synthetic # timing; name" # comment')
        )
        self.assertEqual(len([s for s in sections if s.name == "TextureRedefine"]), 2)
        self.assertIn("# timing;", sections[0].values["name"])

    def test_preset_categories_conditions_and_resolution(self):
        sections = parse_rules(RULES)
        variables, chosen = resolve_presets(
            sections, {"Mode": "Advanced", "Resolution": "Full HD"}
        )
        self.assertEqual(chosen["Limit"], "60 FPS")
        textures, control = resolve_runtime(sections, variables)
        self.assertEqual(textures[0]["overwriteWidth"], [960])
        self.assertEqual(textures[0]["overwriteHeight"], [1080])
        self.assertEqual(control["vsyncFrequency"], 60)

    def test_nested_expression_lists(self):
        sections = parse_rules(
            RULES.replace("($width / 1280) * 640", "max(1, $width / 2)")
        )
        variables, _ = resolve_presets(sections)
        textures, _ = resolve_runtime(sections, variables)
        self.assertEqual(textures[0]["overwriteWidth"], [640])

    def test_hidden_and_unknown_presets(self):
        sections = parse_rules(RULES)
        self.assertEqual(resolve_presets(sections)[0]["$fps"], 30)
        for selected in ({"Limit": "60 FPS"}, {"Missing": "HD"}, {"Mode": "Missing"}):
            with self.assertRaises(PackError):
                resolve_presets(sections, selected)

    def test_expressions(self):
        self.assertEqual(
            evaluate("(($x == 1) + ($y != 2)) * 30", {"$x": 1, "$y": 3}), 60
        )
        self.assertEqual(evaluate("0xFF & 3", {}), 3)
        self.assertEqual(evaluate("const_0.5 / 2", {"const_0.5": 30}), 15)
        self.assertEqual(evaluate("max(4, floor(5.7))", {}), 5)
        self.assertEqual(evaluate("1 && !0", {}), 1)

    def test_reject_malformed_and_unsafe(self):
        for text in (
            "x = 1",
            "[Definition]\nversion=6\nversion=7",
            "[Definition]\nversion=99",
            RULES + "\n[Unknown]\nx=1",
        ):
            with self.assertRaises(PackError):
                parse_rules(text)
        for expression in (
            '__import__("os")',
            "2**10000000",
            "1/0",
            "$missing",
            "1 << 9999",
            "[1][0]",
            "(1).__class__",
        ):
            with self.assertRaises(PackError, msg=expression):
                evaluate(expression, {})


class PatchTests(unittest.TestCase):
    def compile(self, text, base=0x02000010):
        statements, symbols, end, active = layout(
            [("synthetic.asm", text)], 0x1234, {"$fps": 60}, base
        )
        return compile_patch(statements, symbols, Assembler()), symbols, end, active

    def test_labels_cave_data_and_branches(self):
        writes, symbols, end, _ = self.compile("""[Test]
moduleMatches = 0xFFFF,0x1234
.origin = codecave
value:
.float $fps
entry:
lis r11, value@ha
lfs f1, value@l(r11)
blr
0x02000000 = bla entry
0x02000004 = nop
""")
        by_address = {w.address: w for w in writes}
        self.assertEqual(by_address[0x02000010].data, struct.pack(">f", 60))
        self.assertEqual(by_address[0x02000000].data, struct.pack(">I", 0x48000015))
        self.assertEqual(by_address[0x02000014].data, bytes.fromhex("3d600200"))
        self.assertEqual(by_address[0x02000018].data, bytes.fromhex("c02b0010"))
        self.assertEqual(symbols["entry"], 0x02000014)
        self.assertEqual(end, 0x02000020)

    def test_cross_file_labels_and_forward_constants(self):
        files = [
            (
                "a.asm",
                "[A]\nmoduleMatches=0x1234\n.origin=codecave\nentry:\nli r3, n\nb done\nn = later + 1\nlater = 59",
            ),
            ("b.asm", "[B]\nmoduleMatches=0x1234\n.origin=codecave\ndone:\nblr"),
        ]
        statements, symbols, _, _ = layout(files, 0x1234, {}, 0x02000010)
        writes = compile_patch(statements, symbols, Assembler())
        self.assertEqual(writes[0].data, bytes.fromhex("3860003c"))
        self.assertEqual(writes[1].data, bytes.fromhex("4800000c"))

    def test_explicit_address_does_not_advance_cursor(self):
        writes, _, _, _ = self.compile(
            "[A]\nmoduleMatches=0x1234\n.origin=codecave\nnop\n0x02000000=nop\nblr"
        )
        self.assertEqual(
            [w.address for w in writes], [0x02000000, 0x02000010, 0x02000014]
        )

    def test_conditional_branch_and_nested_directive(self):
        writes, _, _, _ = self.compile(
            "[A]\nmoduleMatches=0x1234\n.origin=codecave\nstart:\n"
            "bc 12,2,start+8\nbeqla start+8\nblr\n.float max(1, $fps), min(2, $fps)"
        )
        self.assertEqual(writes[0].data.hex(), "41820008")
        self.assertEqual(writes[1].data.hex(), "41820005")
        self.assertEqual(writes[-1].data, struct.pack(">2f", 60, 2))

    def test_module_filter(self):
        writes, _, _, active = self.compile(
            "[A]\nmoduleMatches=0x8888\n.origin=codecave\nunsupported"
        )
        self.assertEqual((writes, active), ([], 0))

    def test_atomic_conflicts_and_bounds(self):
        sections = {0x1000: bytearray(16)}
        for writes in (
            [Write(0x1000, b"ab", "a"), Write(0x1001, b"b", "b")],
            [Write(0x1000, b"ab", "a"), Write(0x2000, b"b", "b")],
        ):
            with self.assertRaises(PackError):
                apply_writes(sections, writes)
            self.assertEqual(sections[0x1000], bytearray(16))
        apply_writes(sections, [Write(0x1004, bytes.fromhex("60000000"), "a")])
        self.assertEqual(sections[0x1000][4:8], bytes.fromhex("60000000"))

    def test_reject_invalid_assembly(self):
        for body in (
            "0x02000001=nop",
            "0x02000000=b 0x08000000",
            '.origin=codecave\n.include "x"',
            ".origin=codecave\n.int 0x100000000",
            ".origin=codecave\nli r3, unknown",
        ):
            with self.assertRaises(PackError, msg=body):
                self.compile("[A]\nmoduleMatches=0x1234\n" + body)

    def test_signed_low_and_ha(self):
        writes, _, _, _ = self.compile(
            "[A]\nmoduleMatches=0x1234\nx=0x10008004\n.origin=codecave\nlis r3,x@ha\nlwz r3,x@l+4(r3)"
        )
        self.assertEqual([w.data.hex() for w in writes], ["3c601001", "80638008"])


class IntegrationTests(unittest.TestCase):
    def test_synthetic_image_hook_analyze_codegen_snapshot(self):
        raw = synthetic_elf()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rpx = root / "authored.elf"
            rpx.write_bytes(raw)
            crc = module_checksum(raw)
            (root / "rules.txt").write_text(RULES)
            (root / "patch_synthetic.asm").write_text(f"""[Test]
moduleMatches = 0x{crc:08x}
.origin = codecave
constant:
.float $fps
entry:
li r3, $fps
blr
0x02000000 = bl entry
""")
            config = {
                "version": 1,
                "title_id": "0000000000000001",
                "packs": [{"path": str(root), "presets": {"Mode": "Advanced"}}],
            }
            driver = SimpleNamespace(
                Image=Image, Analyzer=Analyzer, Generator=Generator
            )
            state = configure_hook(driver, config, Assembler(), raw)
            image = driver.Image(str(rpx))
            analyzer = driver.Analyzer(image)
            analyzer.run()
            generator = driver.Generator(image, analyzer.entries)
            emitted = "\n".join(
                generator.gen_function(f)[0] for f in analyzer.functions.values()
            )
            self.assertIn("f_02000014", emitted)
            self.assertIn("(uint32_t)60", emitted)
            self.assertNotIn(0x02000010, analyzer.functions)
            self.assertFalse(generator.unsupported)
            self.assertEqual(image.u32(0x02000000), 0x48000015)
            include = emit_snapshot(state["plan"])
            self.assertIn(hashlib.sha256(raw).hexdigest(), include)
            self.assertIn("pack.textures.push_back", include)
            # Compile the exact generated snapshot with the portable runtime.
            (root / "packs_generated.inc").write_text(include)
            import subprocess

            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(root),
                    "-I",
                    "runtime/src/packs",
                    "-c",
                    "runtime/src/packs/packs.cpp",
                    "-o",
                    str(root / "packs.o"),
                ],
                check=True,
                capture_output=True,
            )

    def test_relocation_invalidation_and_import_calls(self):
        raw = synthetic_elf()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "authored.elf"
            path.write_bytes(raw)
            image = Image(str(path))
            image.rpx.symbols.append(Symbol("call", 0xC0000000, 0, 0, 0))
            image.rpx.imports.append(
                SimpleNamespace(
                    lib="gx2", name="Call", addr=0xC0000000, is_data=False, sym_index=0
                )
            )
            image.import_at[0xC0000000] = image.rpx.imports[0]
            relocation = Reloc(0x02000002, 4, 0, 0, 1)
            image.reloc_at[relocation.offset] = relocation
            image.rpx.relocs.append(relocation)
            writes = [
                Write(
                    0x02000000,
                    bytes.fromhex("48000001"),
                    "test",
                    True,
                    "import.gx2.Call",
                )
            ]
            plan = SimpleNamespace(
                writes=writes, cave_base=0x02000010, cave_end=0x02000010
            )
            apply_to_image(image, plan)
            self.assertNotIn(0x02000002, image.reloc_at)
            self.assertEqual(image.reloc_import(0x02000000).name, "Call")
            analyzer = Analyzer(image)
            analyzer.run()
            generator = Generator(image, analyzer.entries)
            code = generator.gen_function(analyzer.functions[0x02000000])[0]
            self.assertIn("HLE_CALL", code)

    def test_checksum_compressed_and_corrupt(self):
        self.assertEqual(module_checksum(synthetic_elf()), 0x5C3D40D7)
        self.assertEqual(module_checksum(synthetic_elf(True)), 0xFA9F894C)
        self.assertNotEqual(
            module_checksum(synthetic_elf()), module_checksum(synthetic_elf(True))
        )
        with self.assertRaises(PackError):
            module_checksum(b"bad")

    def test_manifest_mismatch(self):
        raw = synthetic_elf()
        image = SimpleNamespace(text_hi=0x02000010)
        with self.assertRaises(PackError):
            prepare(image, raw, {"rpx_sha256": "0" * 64, "packs": []})


class CliTests(unittest.TestCase):
    def test_cli_snapshot_shader_and_timing_hook(self):
        raw = synthetic_elf()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rpx = root / "authored.elf"
            rpx.write_bytes(raw)
            (root / "rules.txt").write_text(RULES.replace("Synthetic timing", "FPS++"))
            crc = module_checksum(raw)
            (root / "patch_test.asm").write_text(
                f"[Authored]\nmoduleMatches=0x{crc:08x}\n0x02000000=li r3, $fps\n"
            )
            (root / "0000000000000001_0000000000000002_ps.txt").write_text(
                "// Authored test shader\nvoid main() { float value = $fps; }\n"
            )
            config = {
                "version": 1,
                "title_id": "0000000000000001",
                "packs": [{"path": str(root), "presets": {"Mode": "Advanced"}}],
            }
            manifest = root / "selection.json"
            manifest.write_text(json.dumps(config))
            output = root / "generated"
            result = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    "tools/packs/recompile.py",
                    str(rpx),
                    str(output),
                    "--packs",
                    str(manifest),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("1 writes before PPC analysis", result.stdout)
            self.assertTrue((output / "table.cpp").exists())
            report = json.loads((output / "packs_report.json").read_text())
            self.assertTrue(report["external_timing"])
            self.assertIn("value = 60", report["packs"][0]["shaders"][0]["source"])
            driver = SimpleNamespace(
                Image=Image, Analyzer=Analyzer, Generator=Generator
            )
            configure_hook(driver, config, Assembler(), raw)
            image = driver.Image(str(rpx))
            analyzer = driver.Analyzer(image)
            analyzer.run()
            generator = driver.Generator(image, analyzer.entries)
            generator.hooks.update({0x02000000, 0x031FAAE8, 0x02000004})
            generator.gen_function(analyzer.functions[0x02000000])
            self.assertEqual(generator.hooks, {0x02000004})
            # An empty selection overwrites the old snapshot instead of retaining patches.
            config["packs"] = []
            manifest.write_text(json.dumps(config))
            subprocess.run(
                [
                    sys.executable,
                    "-I",
                    "tools/packs/recompile.py",
                    str(rpx),
                    str(output),
                    "--packs",
                    str(manifest),
                ],
                check=True,
                capture_output=True,
            )
            self.assertNotIn(
                "initial_memory.push_back", (output / "packs_generated.inc").read_text()
            )


class RuntimeTests(unittest.TestCase):
    def test_portable_runtime_contract(self):
        import subprocess

        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "runtime_test")
            subprocess.run(
                [
                    "clang++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    "runtime/src/packs",
                    "runtime/src/packs/packs.cpp",
                    "tools/packs/tests/runtime_test.cpp",
                    "-o",
                    binary,
                ],
                check=True,
                capture_output=True,
            )
            subprocess.run([binary], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
