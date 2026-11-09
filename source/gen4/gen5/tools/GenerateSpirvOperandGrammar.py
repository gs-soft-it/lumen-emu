#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate compact operand tables from the Khronos MIT SPIR-V grammar."""

import json
from pathlib import Path
import sys


def main():
    grammar_path, glsl_path, output = map(Path, sys.argv[1:])
    grammar = json.loads(grammar_path.read_text())
    glsl = json.loads(glsl_path.read_text())
    kinds = grammar["operand_kinds"]
    operands = []
    enumerants = []

    def sequence(items):
        start = len(operands)
        for item in items:
            repeat = {None: "One", "?": "Optional", "*": "Remaining"}[item.get("quantifier")]
            operands.append(f'{{Kind::{item["kind"]}, Repeat::{repeat}}}')
        return span(start, len(items))

    def span(start, count):
        if start >= 65536 or count >= 65536:
            raise ValueError("SPIR-V operand grammar exceeds table limits")
        return f'{{{start}, {count}}}'

    def instructions(data):
        rules = {}
        for instruction in data["instructions"]:
            opcode = instruction["opcode"]
            rule = [(operand["kind"], operand.get("quantifier"))
                    for operand in instruction.get("operands", [])]
            if opcode in rules and rules[opcode] != rule:
                raise ValueError(f"operand grammar conflict for opcode {opcode}")
            rules[opcode] = rule
        return [f'{{{opcode}, {sequence([dict(kind=kind, quantifier=q) for kind, q in rule])}}}'
                for opcode, rule in sorted(rules.items())]

    kind_rows = []
    for kind in kinds:
        category = kind["category"]
        if category == "Composite":
            data = sequence([{"kind": base} for base in kind["bases"]])
        elif category in ("BitEnum", "ValueEnum"):
            start = len(enumerants)
            values = {}
            for enumerant in kind["enumerants"]:
                literal = enumerant["value"]
                value = int(literal, 0) if isinstance(literal, str) else literal
                rule = [(item["kind"], item.get("quantifier"))
                        for item in enumerant.get("parameters", [])]
                if value in values and values[value] != rule:
                    raise ValueError(f'operand grammar conflict for {kind["kind"]}={value}')
                values[value] = rule
            for value, rule in sorted(values.items()):
                if category == "BitEnum" and value and value & (value - 1):
                    raise ValueError("bit enumerant has more than one bit")
                params = sequence([dict(kind=name, quantifier=q) for name, q in rule])
                enumerants.append(f'{{{value}u, {params}}}')
            data = span(start, len(values))
        else:
            data = "{0, 0}"
        kind_rows.append(f'{{Category::{category}, {data}}}')

    core_rows = instructions(grammar)
    glsl_rows = instructions(glsl)
    lines = ["// Generated from Khronos SPIRV-Headers. See its LICENSE (MIT).",
             "// Copyright 2014–2024 The Khronos Group Inc.",
             "enum class Kind : uint8_t {", *(f'    {kind["kind"]},' for kind in kinds), "};",
             "enum class Category { Id, Literal, Composite, BitEnum, ValueEnum };",
             "enum class Repeat { One, Optional, Remaining };",
             "struct Sequence { uint16_t offset; uint16_t count; };",
             "struct Operand { Kind kind; Repeat repeat; };",
             "struct KindRule { Category category; Sequence sequence; };",
             "struct Enumerant { uint32_t value; Sequence parameters; };",
             "struct Instruction { uint32_t opcode; Sequence operands; };", ""]
    for datatype, name, rows in [("Operand", "Operands", operands), ("KindRule", "KindRules", kind_rows),
                                 ("Enumerant", "Enumerants", enumerants), ("Instruction", "Core", core_rows),
                                 ("Instruction", "Glsl", glsl_rows)]:
        lines.extend([f'constexpr {datatype} {name}[] = {{', *(f'    {row},' for row in rows), "};", ""])
    output.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
