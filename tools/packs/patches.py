"""Cemu patch layout, PPC32 assembly and transactional image writes.

LLVM supplies instruction encoding; all labels/expressions are resolved here. No
assembler input can open files, invoke a shell, or emit arbitrary directives.
"""

import ast
import re
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .rules import PackError, evaluate, split_expressions, uncomment


@dataclass(frozen=True)
class Write:
    address: int
    data: bytes
    source: str
    executable: bool = False
    import_name: str = ""


@dataclass
class Statement:
    address: int
    text: str
    source: str


_SIZES = {
    ".byte": 1,
    ".short": 2,
    ".int": 4,
    ".uint": 4,
    ".ptr": 4,
    ".float": 4,
    ".double": 8,
}
_LABEL = re.compile(r"[A-Za-z_][\w.]*\Z")


def string_bytes(text):
    try:
        value = ast.literal_eval(text)
    except (ValueError, SyntaxError) as exc:
        raise PackError("invalid .string") from exc
    if not isinstance(value, str):
        raise PackError(".string requires a quoted string")
    return value.encode("utf-8") + b"\0"


def align_padding(args, address):
    """Bytes `.align N` inserts at address (Cemu packs: N is a byte alignment)."""
    try:
        alignment = int(args.strip(), 0)
    except ValueError as exc:
        raise PackError("invalid .align") from exc
    if alignment <= 0 or alignment & (alignment - 1):
        raise PackError(".align requires a power of two")
    return -address % alignment


def size_of(text, address):
    pieces = text.split(None, 1)
    op, args = pieces[0].lower(), pieces[1] if len(pieces) > 1 else ""
    if op == ".align":
        return align_padding(args, address)
    if op == ".string":
        return len(string_bytes(args))
    if op in _SIZES:
        return _SIZES[op] * len(split_expressions(args))
    if op.startswith("."):
        raise PackError(f"unsupported directive {op}")
    return 4


def active_groups(text, checksum, source):
    groups, current = [], None
    for number, raw in enumerate(text.lstrip("\ufeff").splitlines(), 1):
        line = uncomment(raw)
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = []
            groups.append((line, current))
        elif current is None:
            raise PackError(f"{source}:{number}: expected patch group")
        else:
            current.append((number, line))
    result = []
    for name, lines in groups:
        matches = [
            (n, line.split("=", 1)[1])
            for n, line in lines
            if line.startswith("moduleMatches") and "=" in line
        ]
        if len(matches) != 1:
            raise PackError(f"{source}:{name}: require one moduleMatches")
        if checksum in [int(x.strip(), 0) for x in matches[0][1].split(",")]:
            result.extend(
                (n, line) for n, line in lines if not line.startswith("moduleMatches")
            )
    return result


def layout(files, checksum, variables, cave_base):
    """Labels are shared between active groups/files of one pack only."""
    symbols = dict(variables)
    statements, constants = [], {}
    cursor = None
    cave_end = cave_base
    in_cave = False
    active = 0
    for source, text in files:
        lines = active_groups(text, checksum, source)
        active += bool(lines)
        cursor = None
        in_cave = False
        for number, line in lines:
            location = f"{source}:{number}"
            try:
                if line.startswith(".origin"):
                    _, value = map(str.strip, line.split("=", 1))
                    in_cave = value == "codecave"
                    cursor = (
                        (cave_end + 15) & ~15
                        if in_cave
                        else int(evaluate(value, symbols))
                    )
                    continue
                address = cursor
                explicit = False
                if "=" in line and not any(
                    c in line.split("=", 1)[0].strip() for c in " ()!,"
                ):
                    lhs, rhs = map(str.strip, line.split("=", 1))
                    if _LABEL.fullmatch(lhs):
                        if lhs in symbols or lhs in constants:
                            raise PackError(f"duplicate symbol {lhs}")
                        constants[lhs] = rhs
                        # Make already-resolvable constants available to subsequent origins.
                        try:
                            symbols[lhs] = evaluate(rhs, symbols)
                        except PackError:
                            pass
                        continue
                    address = int(evaluate(lhs, symbols))
                    line = rhs
                    explicit = True
                if address is None:
                    raise PackError("output or label before .origin")
                if not 0 <= address < 2**32:
                    raise PackError("address outside PPC32")
                if line.endswith(":"):
                    name = line[:-1]
                    if (
                        not _LABEL.fullmatch(name)
                        or name in symbols
                        or name in constants
                    ):
                        raise PackError(f"invalid or duplicate label {name}")
                    symbols[name] = address
                    continue
                if explicit and line.startswith(".align"):
                    raise PackError(".align needs a sequential address")
                size = size_of(line, address)
                statements.append(Statement(address, line, location))
                if not explicit:
                    cursor += size
                    if in_cave:
                        cave_end = max(cave_end, cursor)
                if cave_end - cave_base > 1024 * 1024:
                    raise PackError("codecaves exceed 1 MiB")
            except (ValueError, TypeError) as exc:
                raise PackError(f"{location}: {exc}") from exc
    pending = dict(constants)
    while pending:
        resolved = []
        for name, expression in pending.items():
            try:
                symbols[name] = evaluate(expression, symbols)
                resolved.append(name)
            except PackError:
                pass
        if not resolved:
            raise PackError(f"undefined or cyclic patch constants: {sorted(pending)}")
        for name in resolved:
            del pending[name]
    return statements, symbols, (cave_end + 15) & ~15, active


def patch_expression(text, symbols, address):
    text = re.sub(r"(?<![\w.])\.(?![\w.])", str(address), text)

    def modifier(match):
        value = int(evaluate(match[1], symbols))
        kind = match[2]
        if kind == "ha":
            return str(((value + 0x8000) >> 16) & 0xFFFF)
        if kind in ("h", "hi"):
            return str((value >> 16) & 0xFFFF)
        low = value & 0xFFFF
        return str(low - 0x10000 if low & 0x8000 else low)

    text = re.sub(r"([A-Za-z_][\w.]*|0x[\da-fA-F]+)@(ha|hi|h|lo|l)\b", modifier, text)
    return evaluate(text, symbols)


class Assembler:
    def __init__(self, executable=None):
        self.executable = executable or shutil.which("llvm-mc")
        if self.executable is None:
            candidate = Path("/opt/homebrew/opt/llvm/bin/llvm-mc")
            if candidate.is_file():
                self.executable = str(candidate)
        self.cache = {}

    def encode_many(self, instructions):
        missing = list(dict.fromkeys(s for s in instructions if s not in self.cache))
        if missing:
            if not self.executable:
                raise PackError(
                    "PPC patches require llvm-mc (brew install llvm), or --llvm-mc"
                )
            result = subprocess.run(
                [
                    self.executable,
                    "--triple=powerpc-unknown-linux-gnu",
                    "--show-encoding",
                ],
                input="\n".join(missing) + "\n",
                text=True,
                capture_output=True,
                check=False,
            )
            if result.returncode:
                raise PackError(f"PPC assembly failed:\n{result.stderr}")
            encoded = re.findall(r"encoding: \[([^\]]+)\]", result.stdout)
            if len(encoded) != len(missing):
                raise PackError(
                    "assembler emitted fixups or an unexpected instruction count"
                )
            for instruction, value in zip(missing, encoded):
                try:
                    data = bytes(int(v.strip(), 16) for v in value.split(","))
                except ValueError as exc:
                    raise PackError(
                        f"unresolved assembler fixup: {instruction}"
                    ) from exc
                if len(data) != 4:
                    raise PackError("expected one PPC32 instruction")
                self.cache[instruction] = data
        return [self.cache[s] for s in instructions]


def compile_patch(statements, symbols, assembler):
    writes, jobs = [], []
    branch_targets = {}
    previous_code = False
    previous_end, previous_file = None, None
    for statement in statements:
        address, text, source = statement.address, statement.text, statement.source
        source_file = source.rsplit(":", 1)[0]
        if address != previous_end or source_file != previous_file:
            previous_code = False
        previous_end, previous_file = address + size_of(text, address), source_file
        pieces = text.split(None, 1)
        op, args = pieces[0].lower(), pieces[1] if len(pieces) > 1 else ""
        try:
            if op == ".align":
                padding = align_padding(args, address)
                if padding:
                    # Pad code with nops so falling through the gap stays harmless.
                    nop = padding % 4 == 0 and address % 4 == 0
                    data = (
                        b"\x60\x00\x00\x00" * (padding // 4) if nop else bytes(padding)
                    )
                    writes.append(Write(address, data, source, previous_code and nop))
                continue
            if op in _SIZES or op == ".string":
                if op == ".string":
                    data = string_bytes(args)
                else:
                    values = [
                        patch_expression(v.strip(), symbols, address)
                        for v in split_expressions(args)
                    ]
                    if op in (".float", ".double"):
                        data = b"".join(
                            struct.pack(">f" if op == ".float" else ">d", v)
                            for v in values
                        )
                    else:
                        size = _SIZES[op]
                        if any(
                            int(v) != v
                            or not -(1 << (size * 8 - 1)) <= v < 1 << (size * 8)
                            for v in values
                        ):
                            raise PackError("integer directive overflow")
                        data = b"".join(
                            (int(v) & ((1 << (size * 8)) - 1)).to_bytes(size, "big")
                            for v in values
                        )
                # Cemu packs use .int/.uint to embed instructions not accepted by their assembler.
                raw_code = previous_code and op in (".int", ".uint")
                writes.append(Write(address, data, source, raw_code))
                previous_code = raw_code
                continue
            if address % 4:
                raise PackError("unaligned PPC instruction")
            previous_code = op not in ("blr", "bctr", "b", "ba", "trap")
            if op in ("b", "bl", "ba", "bla") and args.startswith("import."):
                if op not in ("b", "bl", "ba", "bla") or not re.fullmatch(
                    r"import\.\w+\.\w+", args
                ):
                    raise PackError("invalid import branch")
                writes.append(
                    Write(
                        address,
                        struct.pack(">I", 0x48000000 | int(op in ("bl", "bla"))),
                        source,
                        True,
                        args,
                    )
                )
                continue
            branch = op in ("b", "bl", "ba", "bla", "bc", "bcl", "bca", "bcla") or bool(
                re.fullmatch(r"b(eq|ne|lt|le|gt|ge|so|ns|dnz|dz)l?a?[+-]?", op)
            )
            operands = split_expressions(args) if args else []
            if op in ("fcmpu", "fcmpo") and len(operands) == 2:
                operands.insert(0, "cr0")
            result = []
            absolute = False
            for i, operand in enumerate(operands):
                if re.fullmatch(r"(?:r|f|cr)\d+", operand, re.IGNORECASE):
                    if branch and i == len(operands) - 1:
                        raise PackError("branch target must be an address")
                    result.append(re.sub(r"^(r|f|cr)", "", operand.lower()))
                elif re.search(r"\((r\d+)\)$", operand, re.IGNORECASE):
                    offset, register = operand.rsplit("(", 1)
                    result.append(
                        f"{int(patch_expression(offset or '0', symbols, address))}({register[1:-1]})"
                    )
                else:
                    operand_symbols = symbols
                    if op.startswith("cr") or (
                        op in ("bc", "bcl", "bca", "bcla") and i == 1
                    ):
                        operand_symbols = {
                            **symbols,
                            **{f"cr{k}": k for k in range(8)},
                            "lt": 0,
                            "gt": 1,
                            "eq": 2,
                            "so": 3,
                        }
                    value = patch_expression(operand, operand_symbols, address)
                    if int(value) != value:
                        raise PackError("non-integer instruction operand")
                    value = int(value)
                    if branch and i == len(operands) - 1:
                        branch_targets[address] = value
                        absolute = op in ("ba", "bla") and 0 <= value < 0x02000000
                        if not absolute:
                            value -= address
                        bits = 26 if op in ("b", "bl", "ba", "bla") else 16
                        if value % 4 or not -(1 << (bits - 1)) <= value < 1 << (
                            bits - 1
                        ):
                            raise PackError(
                                f"branch target out of range or unaligned: {op} at {address:#x} offset {value:#x}"
                            )
                    # LLVM parses `blt -16` as the prediction-hint mnemonic
                    # `blt- 16`. Parentheses keep the displacement signed.
                    result.append(
                        f"({value})"
                        if branch and i == len(operands) - 1
                        else str(value)
                    )
            if op in ("ba", "bla") and not absolute:
                op = "bl" if op == "bla" else "b"
            elif branch and not absolute:
                op = re.sub(r"a(?=[+-]?$)", "", op)
            if op in ("b", "bl", "ba", "bla") and len(result) == 1:
                # LLVM rejects negative numeric operands for b/bl. Encode the
                # already range-checked I-form directly, retaining AA and LK.
                word = 0x48000000 | (int(result[0].strip("()")) & 0x03FFFFFC)
                word |= 2 * absolute | int(op in ("bl", "bla"))
                writes.append(Write(address, struct.pack(">I", word), source, True))
                continue
            instruction = op + (" " + ",".join(result) if result else "")
            jobs.append((statement, instruction))
        except (ValueError, OverflowError, struct.error) as exc:
            raise PackError(f"{source}: {exc}") from exc
    for (statement, _), data in zip(jobs, assembler.encode_many([i for _, i in jobs])):
        writes.append(Write(statement.address, data, statement.source, True))
    from cafe import ppc

    for write in writes:
        if write.address in branch_targets:
            decoded = ppc.decode(int.from_bytes(write.data, "big"), write.address)
            if decoded.target != branch_targets[write.address]:
                raise PackError(f"{write.source}: assembled branch target mismatch")
    return sorted(writes, key=lambda w: w.address)


def apply_writes(sections: dict[int, bytearray], writes: list[Write]):
    """Validate the whole transaction, then mutate. Reject overlaps, even equal writes."""
    occupied, targets = set(), []
    for write in writes:
        end = write.address + len(write.data)
        if not write.data or end > 2**32:
            raise PackError(f"{write.source}: invalid write range")
        addresses = set(range(write.address, end))
        if occupied & addresses:
            raise PackError(f"{write.source}: overlapping patch at {write.address:#x}")
        occupied.update(addresses)
        target = next(
            (
                (buf, write.address - base)
                for base, buf in sections.items()
                if base <= write.address and end <= base + len(buf)
            ),
            None,
        )
        if target is None:
            raise PackError(f"{write.source}: unmapped write at {write.address:#x}")
        targets.append((target, write))
    for (buffer, offset), write in targets:
        buffer[offset : offset + len(write.data)] = write.data
