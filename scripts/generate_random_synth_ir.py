#!/usr/bin/env python3
"""Generate random Synth dialect IR for AIG/XAG/MIG/XMG fuzzing."""

import argparse
from pathlib import Path
import random
import sys


KINDS = ("aig", "xag", "mig", "xmg")


def value_ref(name: str, rng: random.Random, invert_probability: float) -> str:
    if rng.random() < invert_probability:
        return f"not {name}"
    return name


def choose_operands(values: list[str], count: int, rng: random.Random,
                    invert_probability: float) -> list[str]:
    return [
        value_ref(rng.choice(values), rng, invert_probability)
        for _ in range(count)
    ]


def choose_op(kind: str, rng: random.Random) -> tuple[str, int]:
    if kind == "aig":
        return "synth.aig.and_inv", 2
    if kind == "mig":
        return "synth.majority", 3
    if kind == "xag":
        return rng.choice([("synth.aig.and_inv", 2), ("synth.xor_inv", 2)])
    if kind == "xmg":
        return rng.choice([("synth.majority", 3), ("synth.xor_inv", 2)])
    raise ValueError(f"unknown kind: {kind}")


def emit_inverter(kind: str, result: str, operand: str) -> str:
    if kind in ("mig", "xmg"):
        operands = f"not {operand}, not {operand}, not {operand}"
        return f"{result} = synth.majority {operands} : i1"
    if kind == "xag":
        return f"{result} = synth.xor_inv not {operand} : i1"
    return f"{result} = synth.aig.and_inv not {operand} : i1"


def module_name(base: str, kind: str, multi_module: bool) -> str:
    if multi_module:
        return f"{base}_{kind}"
    return base


def emit_module(kind: str, args: argparse.Namespace,
                rng: random.Random, multi_module: bool) -> list[str]:
    name = module_name(args.module_name, kind, multi_module)
    inputs = [f"%i{i}" for i in range(args.inputs)]
    input_ports = [f"in {name} : i1" for name in inputs]
    output_ports = [f"out out{i} : i1" for i in range(args.outputs)]
    signature = ", ".join(input_ports + output_ports)
    lines = [f"  hw.module @{name}({signature}) {{"]

    values = list(inputs)
    if args.constants:
        lines.append("    %false = hw.constant false")
        lines.append("    %true = hw.constant true")
        values.extend(["%false", "%true"])

    for index in range(args.nodes):
        op_name, arity = choose_op(kind, rng)
        operands = choose_operands(values, arity, rng, args.invert_probability)
        result = f"%n{index}"
        lines.append(f"    {result} = {op_name} {', '.join(operands)} : i1")
        values.append(result)

    output_values = []
    for index in range(args.outputs):
        output_value = rng.choice(values)
        if rng.random() < args.output_invert_probability:
            inverted_value = f"%out_inv{index}"
            lines.append(f"    {emit_inverter(kind, inverted_value, output_value)}")
            output_value = inverted_value
            values.append(output_value)
        output_values.append(output_value)
    output_types = ", ".join(["i1"] * args.outputs)
    lines.append(f"    hw.output {', '.join(output_values)} : {output_types}")
    lines.append("  }")
    return lines


def emit(args: argparse.Namespace) -> str:
    rng = random.Random(args.seed)
    kinds = list(KINDS) if args.kind == "all" else [args.kind]
    multi_module = len(kinds) > 1

    lines = ["module {"]
    for index, kind in enumerate(kinds):
        if index:
            lines.append("")
        lines.extend(emit_module(kind, args, rng, multi_module))
    lines.append("}")
    return "\n".join(lines) + "\n"


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def nonnegative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def probability(value: str) -> float:
    parsed = float(value)
    if parsed < 0.0 or parsed > 1.0:
        raise argparse.ArgumentTypeError("must be in [0, 1]")
    return parsed


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--kind", choices=KINDS + ("all",), default="all")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--inputs", type=positive_int, default=8)
    parser.add_argument("--nodes", type=nonnegative_int, default=64)
    parser.add_argument("--outputs", type=positive_int, default=1)
    parser.add_argument("--module-name", default="random")
    parser.add_argument("--constants", action=argparse.BooleanOptionalAction,
                        default=True)
    parser.add_argument("--invert-probability", type=probability, default=0.25,
                        help="Probability of adding 'not' to each op operand")
    parser.add_argument("--output-invert-probability", type=probability,
                        default=0.0,
                        help="Probability of adding 'not' to each output")
    parser.add_argument("-o", "--output", type=Path,
                        help="Output MLIR path; stdout when omitted")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    text = emit(args)
    if args.output:
        args.output.write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
