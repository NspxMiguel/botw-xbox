"""Strict, ordered Cemu rules parser and bounded numeric expression evaluator."""

import ast
import math
import operator
import re
from dataclasses import dataclass


class PackError(ValueError):
    pass


def uncomment(line: str) -> str:
    quote = None
    escaped = False
    for i, c in enumerate(line):
        if escaped:
            escaped = False
        elif c == "\\":
            escaped = True
        elif c == quote:
            quote = None
        elif c in "\"'" and quote is None:
            quote = c
        elif c in "#;" and quote is None:
            return line[:i].strip()
    return line.strip()


def unquote(value: str) -> str:
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


@dataclass
class Section:
    name: str
    values: dict[str, str]
    line: int


def parse_rules(text: str) -> list[Section]:
    sections = []
    current = None
    for line_number, raw in enumerate(text.lstrip("\ufeff").splitlines(), 1):
        line = uncomment(raw)
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = Section(line[1:-1], {}, line_number)
            sections.append(current)
        elif current is None or "=" not in line:
            raise PackError(f"rules:{line_number}: expected section or key = value")
        else:
            key, value = map(str.strip, line.split("=", 1))
            if not key or not value or key in current.values:
                raise PackError(f"rules:{line_number}: empty or duplicate key {key!r}")
            current.values[key] = value
    definitions = [s for s in sections if s.name == "Definition"]
    if len(definitions) != 1:
        raise PackError("rules require exactly one [Definition]")
    version = definitions[0].values.get("version", "0")
    if version not in ("3", "4", "5", "6", "7"):
        raise PackError(f"unsupported rules version {version}")
    known = {"Definition", "Default", "Preset", "TextureRedefine", "Control"}
    for section in sections:
        if section.name not in known:
            raise PackError(
                f"rules:{section.line}: unsupported section [{section.name}]"
            )
    return sections


_TOKEN = re.compile(r"(?<![\w.])(?:\$[A-Za-z_]\w*|[A-Za-z_][\w.]*)")
_BIN = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod,
    ast.BitAnd: operator.and_,
    ast.BitOr: operator.or_,
    ast.BitXor: operator.xor,
    ast.LShift: operator.lshift,
    ast.RShift: operator.rshift,
}
_CMP = {
    ast.Eq: operator.eq,
    ast.NotEq: operator.ne,
    ast.Lt: operator.lt,
    ast.LtE: operator.le,
    ast.Gt: operator.gt,
    ast.GtE: operator.ge,
}
_FUN = {
    "min": min,
    "max": max,
    "floor": math.floor,
    "ceil": math.ceil,
    "round": round,
    "int": int,
    "float": float,
    "abs": abs,
    "reloc": lambda x: x,
}


def evaluate(expression: str, variables: dict[str, int | float]) -> int | float:
    """Never eval pack text. Reject object access, unbounded powers and huge integers."""
    if len(expression) > 4096:
        raise PackError("expression too long")
    expression = expression.replace("&&", " and ").replace("||", " or ")
    expression = re.sub(r"!(?!=)", " not ", expression)
    values = {}

    def replace(match):
        name = match[0]
        if name in _FUN or name in ("and", "or", "not"):
            return name
        if name in ("true", "false"):
            return "1" if name == "true" else "0"
        if name not in variables:
            raise PackError(f"undefined symbol {name}")
        alias = f"v{len(values)}"
        values[alias] = variables[name]
        return alias

    # Hex digits must remain part of their numeric literal.
    expression = _TOKEN.sub(replace, expression).strip()

    def visit(node):
        if isinstance(node, ast.Constant) and type(node.value) in (int, float):
            result = node.value
        elif isinstance(node, ast.Name) and node.id in values:
            result = values[node.id]
        elif isinstance(node, ast.BinOp) and type(node.op) in _BIN:
            left, right = visit(node.left), visit(node.right)
            if isinstance(node.op, (ast.LShift, ast.RShift)) and not 0 <= right <= 63:
                raise PackError("shift outside 0..63")
            result = _BIN[type(node.op)](left, right)
        elif isinstance(node, ast.UnaryOp):
            op = {
                ast.USub: operator.neg,
                ast.UAdd: operator.pos,
                ast.Invert: operator.invert,
                ast.Not: operator.not_,
            }.get(type(node.op))
            if op is None:
                raise PackError("unsupported unary operator")
            result = op(visit(node.operand))
        elif (
            isinstance(node, ast.Compare)
            and len(node.ops) == 1
            and type(node.ops[0]) in _CMP
        ):
            result = int(
                _CMP[type(node.ops[0])](visit(node.left), visit(node.comparators[0]))
            )
        elif isinstance(node, ast.BoolOp):
            result = int(
                (all if isinstance(node.op, ast.And) else any)(
                    visit(n) for n in node.values
                )
            )
        elif (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id in _FUN
            and not node.keywords
        ):
            result = _FUN[node.func.id](*(visit(n) for n in node.args))
        else:
            raise PackError("unsupported expression syntax")
        if not math.isfinite(result) or abs(result) > 2**64:
            raise PackError("expression outside finite 64-bit range")
        return int(result) if isinstance(result, bool) else result

    try:
        tree = ast.parse(expression, mode="eval")
        if sum(1 for _ in ast.walk(tree)) > 512:
            raise PackError("expression too complex")
        return visit(tree.body)
    except (
        SyntaxError,
        TypeError,
        OverflowError,
        ZeroDivisionError,
        RecursionError,
    ) as exc:
        raise PackError(f"invalid expression {expression!r}: {exc}") from exc


def assignments(values, variables):
    for key, value in values.items():
        if key.startswith("$"):
            name, _, kind = key.partition(":")
            if kind not in ("", "int", "float"):
                raise PackError(f"unsupported variable type {kind}")
            variables[name] = evaluate(value, variables)


def resolve_presets(sections: list[Section], selected: dict[str, str] | None = None):
    """Resolve categories in declaration order; conditions see earlier categories.

    Hidden categories retain [Default] values. Explicit hidden selections are errors.
    Defaults are preferred; otherwise the first visible preset is selected, as in Cemu.
    """
    selected = selected or {}
    variables = {}
    for section in sections:
        if section.name == "Default":
            assignments(section.values, variables)
    categories = {}
    for section in sections:
        if section.name == "Preset":
            category = unquote(section.values.get("category", ""))
            categories.setdefault(category, []).append(section)
    if selected.keys() - categories.keys():
        raise PackError(
            f"unknown preset categories: {sorted(selected.keys() - categories.keys())}"
        )
    chosen = {}
    for category, presets in categories.items():
        visible = [
            p for p in presets if evaluate(p.values.get("condition", "1"), variables)
        ]
        if category in selected:
            candidates = [
                p
                for p in visible
                if unquote(p.values.get("name", "")) == selected[category]
            ]
            if len(candidates) != 1:
                raise PackError(
                    f"unknown, ambiguous or hidden preset {category}: {selected[category]}"
                )
            preset = candidates[0]
        elif visible:
            defaults = [
                p for p in visible if evaluate(p.values.get("default", "0"), variables)
            ]
            if len(defaults) > 1:
                raise PackError(f"multiple defaults for {category}")
            preset = (defaults or visible)[0]
        else:
            continue
        assignments(preset.values, variables)
        chosen[category] = unquote(preset.values.get("name", ""))
    return variables, chosen


def split_expressions(text):
    """Split a numeric list without splitting function arguments."""
    depth, start, result = 0, 0, []
    for index, char in enumerate(text):
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(text[start:index].strip())
            start = index + 1
    result.append(text[start:].strip())
    return result


def resolve_runtime(sections, variables):
    textures, control = [], {}
    for section in sections:
        if section.name == "TextureRedefine":
            fields = {}
            allowed = {
                "width",
                "height",
                "depth",
                "formats",
                "formatsExcluded",
                "tilemodes",
                "tilemodesExcluded",
                "inMEM1",
                "overwriteWidth",
                "overwriteHeight",
                "overwriteDepth",
                "overwriteFormat",
                "overwriteLodBias",
                "overwriteRelativeLodBias",
                "overwriteAnisotropy",
            }
            for key, value in section.values.items():
                key = {
                    "tileModes": "tilemodes",
                    "tileModesExcluded": "tilemodesExcluded",
                }.get(key, key)
                if key not in allowed:
                    raise PackError(f"unsupported texture field {key}")
                fields[key] = [
                    evaluate(v.strip(), variables) for v in split_expressions(value)
                ]
            list_fields = {
                "formats",
                "formatsExcluded",
                "tilemodes",
                "tilemodesExcluded",
            }
            for key, values in fields.items():
                if key not in list_fields and len(values) != 1:
                    raise PackError(f"{key} requires one value")
                if (
                    key in {"overwriteWidth", "overwriteHeight", "overwriteDepth"}
                    and values[0] < 1
                ):
                    raise PackError(f"{key} must be positive")
                if key == "overwriteAnisotropy" and values[0] not in (1, 2, 4, 8, 16):
                    raise PackError("invalid anisotropy")
            textures.append(fields)
        elif section.name == "Control":
            for key, value in section.values.items():
                if key != "vsyncFrequency":
                    raise PackError(f"unsupported control field {key}")
                control[key] = evaluate(value, variables)
                if not 1 <= control[key] <= 1000:
                    raise PackError("vsyncFrequency outside 1..1000")
    return textures, control
