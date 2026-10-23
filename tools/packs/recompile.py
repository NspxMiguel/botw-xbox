"""Recompile with explicit graphic packs, without modifying the shared recompiler.

Usage: python -I tools/packs/recompile.py game.rpx generated --packs /external/packs.json
"""

import argparse
import importlib.util
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from cafe import ppc
from tools.packs.build import (
    apply_to_image,
    emit_snapshot,
    load_manifest,
    prepare,
)
from tools.packs.patches import Assembler
from tools.packs.preset import CONSOLES, write_manifest
from tools.packs.rules import PackError


def configure_hook(driver, config, assembler, rpx_bytes):
    """Install a per-invocation Image/Analyzer/Generator hook, before driver.main()."""
    original_image, original_analyzer, original_generator = (
        driver.Image,
        driver.Analyzer,
        driver.Generator,
    )
    state = {}

    class PackImage(original_image):
        def __init__(self, path):
            super().__init__(path)
            plan = prepare(self, rpx_bytes, config, assembler)
            overwritten = apply_to_image(self, plan)
            state.update(plan=plan, overwritten=overwritten)

    class PackAnalyzer(original_analyzer):
        def insn(self, address):
            # Caves interleave constants and instructions. Data is never gap-compiled.
            if not self.img.is_text(address):
                return ppc.decode(0, address)
            if address in self.img.ctr_continuations:
                return ppc.Insn(address, self.img.u32(address), "b", imm=self.img.ctr_continuations[address])
            return super().insn(address)

        def walk(self, entry):
            # Continuations execute in the current guest stack frame, even when
            # another caller also needs a standalone entry at that address.
            removed = self.entries.intersection(self.img.ctr_continuations.values())
            self.entries.difference_update(removed)
            try:
                return super().walk(entry)
            finally:
                self.entries.update(removed)

        def run(self):
            super().run()
            state["redirect_callees"] = {
                entry: {self.img.lr_redirects[a] for a in function.insns
                        if a in self.img.lr_redirects}
                for entry, function in self.functions.items()
            }

    class PackGenerator(original_generator):
        def insn(self, function, instruction, callees, labels):
            address = instruction.addr
            if address in self.img.lr_redirects:
                # This pack return deliberately replaces the caller's PC.
                return ["return;"], False
            code, falls = super().insn(function, instruction, callees, labels)
            if address in self.img.ctr_continuations:
                code.insert(0, f"c->lr = 0x{address + 4:08x}u;")
            if instruction.op in ("b", "bc") and instruction.LK:
                redirects = state.get("redirect_callees", {}).get(instruction.target, set())
                if redirects and instruction.op != "b":
                    raise PackError(f"conditional call with LR redirect at {address:#x}")
                for target in sorted(redirects):
                    if target not in function.insns:
                        raise PackError(f"LR redirect {target:#x} leaves caller {function.entry:#x}")
                    labels.add(target)
                    code.append(f"if (c->lr == 0x{target:08x}u) goto L_{target:08x};")
                if redirects:
                    code.append(f'if (c->lr != 0x{address + 4:08x}u) rt_trap(c, 0x{address:08x}u, "unexpected pack LR redirect");')
            return code, falls

        def gen_function(self, function):
            if not getattr(self, "_packs_hooks_filtered", False):
                # FPS++ replaces this instruction with a call that executes the
                # original store. Retain only its host pause/telemetry work;
                # fps.cpp gates host delta/scaling off for compiled timing packs.
                housekeeping = {0x031FA97C} if state["plan"].external_timing else set()
                self.hooks.difference_update(state["overwritten"] - housekeeping)
                if state["plan"].external_timing:
                    source = (ROOT / "runtime/src/game/fps.cpp").read_text()
                    self.hooks.difference_update(
                        {int(a, 16) for a in re.findall(r"BOTW_HOOK\((0x[\da-fA-F]+)\)", source)}
                        - housekeeping
                    )
                self._packs_hooks_filtered = True
            before = dict(self.unsupported)
            result = super().gen_function(function)
            if self.unsupported != before and any(
                a in state["overwritten"] for a in function.insns
            ):
                raise PackError(
                    f"patch function {function.entry:#x} uses unsupported recompiler instructions"
                )
            return result

    driver.Image, driver.Analyzer, driver.Generator = (
        PackImage,
        PackAnalyzer,
        PackGenerator,
    )
    return state


def selection_config(args):
    if args.preset:
        with tempfile.TemporaryDirectory(prefix="botw-packs-") as directory:
            manifest = Path(directory) / "selection.json"
            write_manifest(args.preset, args.console, args.graphic_packs, manifest)
            return load_manifest(manifest)
    return load_manifest(args.packs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rpx", type=Path)
    parser.add_argument("output", type=Path)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--packs", type=Path)
    selection.add_argument("--preset", choices=("remaster",))
    parser.add_argument("--console", choices=CONSOLES)
    parser.add_argument("--graphic-packs", type=Path)
    parser.add_argument("--llvm-mc")
    args = parser.parse_args()
    if args.preset and (not args.console or args.graphic_packs is None):
        parser.error("--preset requires --console and --graphic-packs")
    if not args.preset and (args.console or args.graphic_packs is not None):
        parser.error("--console and --graphic-packs require --preset")
    if (
        args.output.resolve() == ROOT
        or ROOT in args.output.resolve().parents
        and args.output.resolve() != ROOT / "generated"
    ):
        raise PackError("inside the repository, pack build output must be generated/")
    spec = importlib.util.spec_from_file_location(
        "pack_recompile_driver", ROOT / "tools/recompile.py"
    )
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    config = selection_config(args)
    state = configure_hook(
        driver, config, Assembler(args.llvm_mc), args.rpx.read_bytes()
    )
    original_argv = sys.argv
    try:
        sys.argv = [str(ROOT / "tools/recompile.py"), str(args.rpx), str(args.output)]
        driver.main()
    finally:
        sys.argv = original_argv
    plan = state["plan"]
    driver.write_if_changed(
        str(args.output / "packs_generated.inc"), emit_snapshot(plan)
    )
    report = {
        "version": 1,
        "fingerprint": plan.fingerprint,
        "rpx_sha256": plan.sha256,
        "module_checksum": f"{plan.checksum:08X}",
        "external_timing": plan.external_timing,
        "writes": len(plan.writes),
        "packs": plan.packs,
    }
    driver.write_if_changed(
        str(args.output / "packs_report.json"), json.dumps(report, indent=2) + "\n"
    )
    print(
        f"Packs: {len(plan.packs)}, {len(plan.writes)} writes before PPC analysis; {plan.fingerprint}"
    )


if __name__ == "__main__":
    try:
        main()
    except (PackError, OSError, KeyError, json.JSONDecodeError) as exc:
        sys.exit(f"graphic packs: {exc}")
