"""Audit calls in player-supplied codecaves without decoding their data."""

from cafe import ppc

from .rules import PackError


def constant_ctr_target(image, address):
    """Resolve adjacent li/mtctr or lis/(addi|ori)/mtctr call sequences.

    Do not infer values across other instructions or control-flow boundaries.
    Relocated low halves use the same code-pointer metadata as .text discovery.
    """

    def instruction(at):
        if not image.is_text(at):
            return None
        return ppc.decode(image.u32(at), at)

    move, low = instruction(address - 4), instruction(address - 8)
    if not move or move.op != "mtspr" or move.imm != 9 or not low:
        return None
    register = move.rD
    if low.op == "addi" and low.rD == register:
        relocated = image.reloc_target(address - 6)
        if relocated is not None:
            return relocated
        if low.rA == 0:
            return low.imm & 0xFFFFFFFF
    high = instruction(address - 12)
    if not high or high.op != "addis" or high.rA != 0:
        return None
    value = (high.imm << 16) & 0xFFFFFFFF
    if low.op == "addi" and low.rD == register and low.rA == high.rD:
        return (value + low.imm) & 0xFFFFFFFF
    # Logical immediates encode the destination in rA, source in rD.
    if low.op == "ori" and low.rA == register and low.rD == high.rD:
        return value | low.imm
    return None


def audit_cave_calls(image, prepared, functions):
    """List all cave calls and patched calls into caves; reject missing entries.

    Import calls, dynamic CTR/LR calls and displaced-code continuations remain
    explicit in the report, rather than being silently counted as known calls.
    """
    result = []
    for write in prepared.writes:
        if not write.executable:
            continue
        for address in range(write.address, write.address + len(write.data), 4):
            instruction = ppc.decode(image.u32(address), address)
            if not instruction.LK or instruction.op not in ("b", "bc", "bcctr", "bclr"):
                continue
            target = None
            if instruction.op in ("b", "bc") and not write.import_name:
                target = instruction.target
            elif instruction.op == "bcctr":
                target = constant_ctr_target(image, address)
            in_cave = address in image.cave_code
            target_in_cave = (
                target is not None and prepared.cave_base <= target < prepared.cave_end
            )
            if not in_cave and not target_in_cave:
                continue
            kind = "call"
            if address in image.ctr_continuations:
                kind = "continuation"
            elif write.import_name:
                kind = "import"
            elif target == address + 4 and instruction.op in ("b", "bc"):
                kind = "read_pc"
            elif target is None:
                kind = "dynamic"
            elif (image.is_text(target) or target_in_cave) and target not in functions:
                raise PackError(
                    f"{write.source}: cave call at {address:#x} has no function at {target:#x}"
                )
            result.append(
                {
                    "address": f"{address:08X}",
                    "instruction": ppc.fmt(instruction),
                    "target": f"{target:08X}" if target is not None else None,
                    "kind": kind,
                    "source": write.source,
                    "target_is_function": target in functions,
                }
            )
    return result
