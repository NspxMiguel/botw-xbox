"""Load an explicit enabled-pack manifest and patch an Image before analysis."""

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path

from .checksum import module_checksum
from .patches import Assembler, apply_writes, compile_patch, layout
from .rules import PackError, parse_rules, resolve_presets, resolve_runtime, unquote


@dataclass
class Prepared:
    writes: list
    packs: list
    checksum: int
    sha256: str
    fingerprint: str
    cave_base: int
    cave_end: int
    external_timing: bool


def load_manifest(path):
    path = Path(path).resolve()
    config = json.loads(path.read_text(encoding="utf-8"))
    if config.get("version") != 1 or not isinstance(config.get("packs"), list):
        raise PackError("pack manifest requires version: 1 and packs: []")
    if not re.fullmatch(r"[\da-fA-F]{16}", config.get("title_id", "")):
        raise PackError("manifest title_id must contain 16 hex digits")
    for entry in config["packs"]:
        entry["path"] = str((path.parent / entry["path"]).resolve())
    return config


def drop_identity_dimensions(packs):
    """Prune statically known identities without losing ordered size resets.

    Wildcards and potentially overlapping earlier writes are resolved against
    the original texture by the runtime instead.
    """
    previous = []
    for pack in packs:
        retained = []
        for rule in pack["textures"]:
            for dimension in ("width", "height", "depth"):
                name = "overwrite" + dimension.capitalize()
                original = rule.get(dimension, [-1])
                if (
                    name not in rule
                    or len(original) != 1
                    or original[0] < 1
                    or int(rule[name][0]) != original[0]
                ):
                    continue
                overlaps = any(
                    name in earlier
                    and all(
                        earlier.get(axis, [-1])[0] == -1
                        or rule.get(axis, [-1])[0] == -1
                        or earlier[axis] == rule[axis]
                        for axis in ("width", "height", "depth")
                    )
                    for earlier in previous
                )
                if not overlaps:
                    del rule[name]
            if any(name.startswith("overwrite") for name in rule):
                retained.append(rule)
                previous.append(rule)
        pack["textures"] = retained


def prepare(image, rpx_bytes, config, assembler=None):
    checksum = module_checksum(rpx_bytes)
    sha256 = hashlib.sha256(rpx_bytes).hexdigest()
    if config.get("rpx_sha256", sha256).lower() != sha256:
        raise PackError("manifest RPX SHA-256 does not match the input")
    cave_base = cave_end = (image.text_hi + 15) & ~15
    writes, packs, seen = [], [], set()
    external_timing = False
    assembler = assembler or Assembler()
    content_hash = hashlib.sha256()
    for entry in config["packs"]:
        if entry.get("enabled", True) is False:
            continue
        path = Path(entry["path"]).resolve()
        if path in seen:
            raise PackError(f"pack enabled twice: {path}")
        seen.add(path)
        raw_rules = (path / "rules.txt").read_text(encoding="utf-8-sig")
        content_hash.update(raw_rules.encode())
        sections = parse_rules(raw_rules)
        definition = next(s.values for s in sections if s.name == "Definition")
        titles = [x.strip().lower() for x in definition.get("titleIds", "").split(",")]
        if config["title_id"].lower() not in titles:
            raise PackError(f"{path}: pack does not support title {config['title_id']}")
        variables, presets = resolve_presets(sections, entry.get("presets", {}))
        textures, control = resolve_runtime(sections, variables)
        name = unquote(definition.get("name", path.name))
        files = []
        for file in sorted(path.glob("patch_*.asm")):
            text = file.read_text(encoding="utf-8-sig")
            files.append((str(file), text))
            content_hash.update(file.name.encode() + b"\0" + text.encode())
        if not files and (path / "patches.txt").exists():
            raise PackError(
                f"{path}: legacy patches.txt is unsupported; use native patch_*.asm"
            )
        statements, symbols, cave_end, active = layout(
            files, checksum, variables, cave_end
        )
        if files and not active:
            raise PackError(f"{path}: no patch groups match module CRC {checksum:08X}")
        pack_writes = compile_patch(statements, symbols, assembler)
        writes.extend(pack_writes)
        timing = entry.get("timing", name.casefold() == "fps++")
        if timing:
            if external_timing:
                raise PackError("only one timing pack can be enabled")
            if not pack_writes:
                raise PackError("timing pack has no matching code patches")
            external_timing = True
        shaders = []
        for file in sorted(path.glob("*.txt")):
            match = re.fullmatch(
                r"([\da-fA-F]{16})_([\da-fA-F]{16})_(vs|gs|ps)\.txt", file.name
            )
            if not match:
                continue
            source = file.read_text(encoding="utf-8-sig")
            content_hash.update(file.name.encode() + b"\0" + source.encode())

            def substitute(match, variables=variables, file=file):
                variable = match[0]
                if variable not in variables:
                    raise PackError(f"{file}: undefined shader variable {variable}")
                return str(variables[variable])

            source = re.sub(r"\$[A-Za-z_]\w*", substitute, source)
            shaders.append(
                {
                    "base_hash": int(match[1], 16),
                    "auxiliary_hash": int(match[2], 16),
                    "stage": {"vs": "vertex", "gs": "geometry", "ps": "pixel"}[
                        match[3]
                    ],
                    "source": source,
                }
            )
        for other in packs:
            if (
                "vsyncFrequency" in control
                and "vsyncFrequency" in other["control"]
                and control["vsyncFrequency"] != other["control"]["vsyncFrequency"]
            ):
                raise PackError(
                    "enabled packs request conflicting vsyncFrequency values"
                )
        packs.append(
            {
                "name": name,
                "textures": textures,
                "control": control,
                "shaders": shaders,
                "presets": presets,
                "variables": variables,
            }
        )
    drop_identity_dimensions(packs)
    if cave_end >= 0x0DF00000:
        raise PackError("codecaves overlap runtime-owned memory")
    for base, buffer in image.secs.items():
        if cave_base < base + len(buffer) and base < cave_end:
            raise PackError("codecave overlaps another RPX section")
    content_hash.update(json.dumps(config, sort_keys=True).encode())
    content_hash.update(sha256.encode())
    # Validate against a copy; no partial application if the last pack conflicts.
    sections = {base: bytearray(data) for base, data in image.secs.items()}
    if cave_end > cave_base:
        sections[cave_base] = bytearray(cave_end - cave_base)
    apply_writes(sections, writes)
    return Prepared(
        writes,
        packs,
        checksum,
        sha256,
        content_hash.hexdigest(),
        cave_base,
        cave_end,
        external_timing,
    )


def apply_to_image(image, prepared):
    from cafe.rpx import Reloc

    imports = {
        f"import.{i.lib.removesuffix('.rpl')}.{i.name}": i
        for i in image.rpx.imports
        if not i.is_data
    }
    for write in prepared.writes:
        if write.import_name and write.import_name not in imports:
            raise PackError(f"{write.source}: unresolved {write.import_name}")
    if prepared.cave_end > prepared.cave_base:
        image.secs[prepared.cave_base] = bytearray(
            prepared.cave_end - prepared.cave_base
        )
    apply_writes(image.secs, prepared.writes)
    image.starts = sorted(image.secs)
    overwritten = set()
    for write in prepared.writes:
        overwritten.update(range(write.address, write.address + len(write.data)))
    # Stale relocations would make codegen ignore a new instruction or branch.
    image.reloc_at = {
        a: r
        for a, r in image.reloc_at.items()
        if not any(
            p in overwritten
            for p in range(a, a + (2 if r.type in (4, 5, 6, 251, 252, 253) else 4))
        )
    }
    image.rpx.relocs = [r for r in image.rpx.relocs if r.offset in image.reloc_at]
    for write in prepared.writes:
        if write.import_name:
            imp = imports[write.import_name]
            image.reloc_at[write.address] = Reloc(
                write.address, 10, imp.sym_index, 0, image.text.index
            )
    original_hi = image.text_hi
    cave_code = {
        w.address + offset
        for w in prepared.writes
        if w.executable
        for offset in range(0, len(w.data), 4)
        if w.address >= prepared.cave_base
    }
    image.text_hi = max(original_hi, prepared.cave_end)
    image.is_text = lambda a: image.text_lo <= a < original_hi or a in cave_code
    return overwritten


def cpp_string(value):
    # Use UTF-8 bytes and fixed-width octal escapes, avoiding C++ escape ambiguities.
    return (
        '"'
        + "".join(
            chr(b) if 32 <= b < 127 and chr(b) not in '\\"' else f"\\{b:03o}"
            for b in value.encode("utf-8")
        )
        + '"'
    )


def emit_snapshot(prepared):
    """Generated include is private build output, never a distributable source asset."""

    def fields(values):
        return (
            "{"
            + ",".join(
                "{" + cpp_string(k) + ",{" + ",".join(str(v) for v in vs) + "}}"
                for k, vs in values.items()
            )
            + "}"
        )

    lines = [
        "// Generated from player-supplied files. Do not commit.",
        "namespace packs {",
        "const BuildPlan& compiled_plan() {",
        "static const BuildPlan plan = [] {",
        "BuildPlan p;",
        f"p.rpx_sha256 = {cpp_string(prepared.sha256)};",
        f"p.fingerprint = {cpp_string(prepared.fingerprint)};",
        f"p.module_checksum = {prepared.checksum}u;",
    ]
    for write in prepared.writes:
        data = ",".join(str(b) for b in write.data)
        lines.append(f"p.initial_memory.push_back({{{write.address}u, {{{data}}}}});")
    for pack in prepared.packs:
        lines.extend(["{ Pack pack;", f"pack.name = {cpp_string(pack['name'])};"])
        for rule in pack["textures"]:
            lines.append(f"pack.textures.push_back({{{fields(rule)}}});")
        lines.append(
            f"pack.control = {fields({k: [v] for k, v in pack['control'].items()})};"
        )
        for shader in pack["shaders"]:
            lines.append(
                "pack.shaders.push_back({"
                + f"{shader['base_hash']}ull,{shader['auxiliary_hash']}ull,"
                + f"ShaderStage::{shader['stage']},{cpp_string(shader['source'])}"
                + "});"
            )
        lines.append("p.packs.push_back(std::move(pack)); }")
    lines.extend(["return p;", "}();", "return plan;", "}", "} // namespace packs"])
    return "\n".join(lines) + "\n"
