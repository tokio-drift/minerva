#!/usr/bin/env python3
import argparse
import csv
import difflib
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
EXPECTED = ROOT / "test/expected"
STAGES = {"lex", "ast", "typed-ast", "semantic", "ir", "mips", "verify-ir", "run"}


def load_cases():
    with (ROOT / "test/cases.tsv").open(newline="") as manifest:
        cases = list(csv.DictReader(manifest, delimiter="\t"))
    names = set()
    for case in cases:
        if not case["id"] or case["id"] in names or case["stage"] not in STAGES:
            raise ValueError(f"invalid or duplicate case: {case}")
        case["exit"] = int(case["exit"])
        names.add(case["id"])
    return cases


def command_for(compiler, case, optimization="0"):
    stage, source = case["stage"], case["source"]
    if stage == "run":
        return [compiler, "--mips", "-O" + optimization, source]
    return [compiler, f"--{stage}", source]


def compare_output(case_id, actual, stderr):
    errors = []
    golden = EXPECTED / f"{case_id}.stdout"
    if golden.exists() and golden.read_bytes() != actual:
        diff = difflib.unified_diff(
            golden.read_text().splitlines(True), actual.decode(errors="replace").splitlines(True),
            fromfile=str(golden.relative_to(ROOT)), tofile="actual",
        )
        errors.append("stdout differs:\n" + "".join(diff))
    diagnostic = EXPECTED / f"{case_id}.stderr"
    if diagnostic.exists():
        for fragment in diagnostic.read_text().splitlines():
            if fragment and fragment not in stderr.decode(errors="replace"):
                errors.append(f"missing diagnostic: {fragment}")
    return errors


def run_case(compiler, case, optimization="both"):
    if case["stage"] == "run":
        errors, baseline = [], None
        for level in ["0", "1"] if optimization == "both" else [optimization]:
            failures, stdout, status = run_program(compiler, case, level)
            errors.extend(f"-O{level}: {failure}" for failure in failures)
            observed = (stdout, status)
            if baseline is not None and baseline != observed:
                errors.append("-O0 and -O1 produced different program output or exit status")
            baseline = observed
        return errors
    command = command_for(compiler, case)
    result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=10)
    errors = []
    if result.returncode != case["exit"]:
        errors.append(f"expected exit {case['exit']}, got {result.returncode}")
    errors.extend(compare_output(case["id"], result.stdout, result.stderr))
    if errors and result.stderr:
        errors.append(result.stderr.decode(errors="replace").rstrip())
    return errors


# Execute one mode and remove only the simulator's startup banner.
def run_program(compiler, case, optimization):
    if not shutil.which("spim"):
        raise ValueError("SPIM is required for runtime cases; install spim or use --frontend")
    result = subprocess.run(command_for(compiler, case, optimization), cwd=ROOT, capture_output=True, timeout=10)
    if result.returncode:
        return ["MIPS compilation failed:\n" + result.stderr.decode(errors="replace")], b"", None
    case_id = case["id"]
    inputs = ROOT / "test/input"
    arguments = inputs / f"{case_id}.args"
    stdin = inputs / f"{case_id}.stdin"
    with tempfile.TemporaryDirectory(prefix="minerva-spim-") as directory:
        assembly = Path(directory) / "program.s"
        assembly.write_bytes(result.stdout)
        command = ["spim", "-file", str(assembly)]
        if arguments.exists():
            command.extend(arguments.read_text().splitlines())
        result = subprocess.run(command, input=stdin.read_bytes() if stdin.exists() else b"",
                                capture_output=True, timeout=10)
    stdout = result.stdout
    if stdout.startswith(b"SPIM Version "):
        banner = stdout.split(b"\n", 5)
        if len(banner) != 6 or not banner[4].startswith(b"Loaded: "):
            return ["unrecognized SPIM startup banner"], stdout, result.returncode
        stdout = banner[5]
    errors = []
    if result.returncode != case["exit"]:
        errors.append(f"expected program exit {case['exit']}, got {result.returncode}")
    if result.stderr or any(marker in stdout for marker in [b"Exception occurred", b"  Exception ",
                                                            b"Attempt to execute", b"Unknown system call"]):
        errors.append("SPIM error:\n" + (result.stderr + stdout).decode(errors="replace"))
    errors.extend(compare_output(case_id, stdout, result.stderr))
    return errors, stdout, result.returncode


def report(name, errors):
    print(f"{'FAIL' if errors else 'PASS'} {name}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
    return int(bool(errors))


def run_cases(compiler, cases, optimization="both"):
    failed = 0
    for case in cases:
        try:
            errors = run_case(compiler, case, optimization)
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            errors = [str(error)]
        failed += report(case["id"], errors)
    return failed


def invoke(compiler, arguments, exit_code=0):
    result = subprocess.run([compiler, *map(str, arguments)], cwd=ROOT, capture_output=True, timeout=10)
    assert result.returncode == exit_code, (
        f"{arguments}: expected exit {exit_code}, got {result.returncode}\n"
        + result.stderr.decode(errors="replace")
    )
    assert b"\x1b" not in result.stdout + result.stderr, "unexpected ANSI escape in compiler output"
    return result


def invalid_invocations():
    # Keep option errors independent of source-file contents.
    source = "test/positive/ir_arithmetic.min"
    ir = "test/expected/ir-arithmetic.stdout"
    cases = [
        ("no-input", []), ("stage-no-input", ["--ast"]),
        ("unknown-option", ["--wat", source]), ("extra-input", [source, source]),
        ("missing-output", [source, "-o"]), ("empty-output", [source, "-o", ""]),
        ("output-option-as-value", [source, "-o", "--ast"]),
        ("duplicate-output", [source, "-o", "one", "-o", "two"]),
        ("missing-ir-file", ["--verify-ir"]), ("empty-ir-file", ["--verify-ir", ""]),
        ("verify-option-as-value", ["--verify-ir", "--ast"]),
        ("verify-with-source", ["--verify-ir", ir, source]),
        ("source-with-verify", [source, "--verify-ir", ir]),
        ("verify-with-output", ["--verify-ir", ir, "-o", "artifact"]),
        ("verify-with-optimization", ["--verify-ir", ir, "-O0"]),
        ("verify-with-stage", ["--verify-ir", ir, "--ir"]),
        ("verify-with-alias", ["--dump-ast", "--verify-ir", ir]),
        ("unknown-optimization", ["-O2", "--ir", source]),
        ("optimization-before-default-lex", ["-O1", source]),
        ("help-with-source", ["--help", source]),
        ("help-with-stage", ["--help", "--lex"]),
    ]
    stages = ["lex", "ast", "typed-ast", "semantic", "ir", "mips"]
    for index, stage in enumerate(stages):
        for other in stages[index + 1:]:
            cases.append((f"conflict-{stage}-{other}", [f"--{stage}", f"--{other}", source]))
    for stage in stages[:4]:
        for level in ["-O0", "-O1"]:
            cases.append((f"{stage}-rejects-{level}", [f"--{stage}", level, source]))
    return cases


def check_usage(compiler, arguments):
    result = invoke(compiler, arguments, 1)
    assert not result.stdout, "usage error wrote to stdout"
    assert b"Usage:" in result.stderr, "usage error did not explain the invocation"


def check_stage_isolation(compiler):
    syntax = "test/negative/bad_syntax.min"
    lexical = "test/negative/bad_lex.min"
    assert invoke(compiler, ["--lex", syntax]).stdout.startswith(b"TOKEN TABLE\n")
    invoke(compiler, ["--ast", syntax], 2)
    for stage in ["lex", "ast"]:
        result = invoke(compiler, [f"--{stage}", lexical], 2)
        assert not result.stdout and b"Unrecognized character" in result.stderr
    invoke(compiler, ["--ast", "test/negative/undeclared.min"])
    invoke(compiler, ["--semantic", "test/negative/undeclared.min"], 3)
    tokens = invoke(compiler, ["--lex", "test/12_feature_pointers.min"]).stdout
    assert b"ARITHMETIC_OPERATOR" in tokens
    assert b"POINTER_DECLARATOR" not in tokens and b"DEREFERENCE" not in tokens


def check_routing(compiler, directory, stage):
    source = "test/positive/ir_arithmetic.min"
    arguments = [f"--{stage}", source]
    result = invoke(compiler, arguments)
    expected = result.stdout
    assert expected.endswith(b"\n") and not expected.endswith(b"\n\n"), "bad trailing newline"
    assert not result.stderr, "successful compilation wrote unrelated stderr"
    assert invoke(compiler, arguments).stdout == expected, "output is not deterministic"
    output = directory / f"{stage} output.txt"
    for option in [["-o", output], [f"-o{output}"]]:
        output.write_bytes(b"old artifact\n")
        result = invoke(compiler, [*arguments, *option])
        assert not result.stdout and not result.stderr
        assert output.read_bytes() == expected, "file differs from stdout"
    for option in [["-o", "-"], ["-o-"]]:
        assert invoke(compiler, [*arguments, *option]).stdout == expected


def check_defaults_and_aliases(compiler, directory):
    source = "test/positive/ir_arithmetic.min"
    assert invoke(compiler, [source]).stdout == invoke(compiler, ["--lex", source]).stdout
    for alias, stage in [("--dump-ast", "--ast"), ("--dump-typed-ast", "--typed-ast")]:
        expected = invoke(compiler, [stage, source]).stdout
        assert invoke(compiler, [alias, source]).stdout == expected
        assert invoke(compiler, [alias, stage, source]).stdout == expected
    expected = invoke(compiler, ["--ir", source]).stdout
    output = directory / "legacy.s"
    for option in [["-o", output], [f"-o{output}"]]:
        assert not invoke(compiler, [source, *option]).stdout
        assert output.read_bytes() == expected, "legacy -o did not select IR"
    assert invoke(compiler, [source, "-o", "-"]).stdout == expected
    assert invoke(compiler, [source, "-o-"]).stdout == expected
    output = directory / "tree.ir"
    invoke(compiler, ["--ast", source, "-o", output])
    assert output.read_bytes() == invoke(compiler, ["--ast", source]).stdout


def check_preserved_output(compiler, directory, stage, source, exit_code):
    output = directory / f"failed-{stage}.txt"
    output.write_bytes(b"keep this artifact\n")
    result = invoke(compiler, [f"--{stage}", source, "-o", output], exit_code)
    assert not result.stdout and result.stderr, "failure did not stay on stderr"
    assert output.read_bytes() == b"keep this artifact\n", "failure replaced the previous artifact"
    assert not list(directory.glob(".*.tmp*")), "temporary output was left behind"


def check_output_errors(compiler, directory):
    source = "test/positive/ir_arithmetic.min"
    locked = directory / "read-only"
    locked.mkdir()
    locked.chmod(0o500)
    try:
        outputs = [directory / "missing" / "output.txt", directory]
        if not os.access(locked, os.W_OK):
            outputs.append(locked / "output.txt")
        for stage in ["lex", "ast", "typed-ast", "semantic", "ir"]:
            for output in outputs:
                result = invoke(compiler, [f"--{stage}", source, "-o", output], 1)
                assert not result.stdout and b"Error:" in result.stderr
            result = invoke(compiler, [f"--{stage}", source, "-o", str(directory) + "/"], 1)
            assert not result.stdout and b"not a valid output path" in result.stderr
    finally:
        locked.chmod(0o700)
    assert directory.is_dir(), "output error replaced the destination directory"
    assert not list(directory.glob(".*.tmp*")), "failed publishing left a temporary file"


def check_pipeline_and_optimization(compiler):
    source = "test/positive/ir_arithmetic.min"
    expected = invoke(compiler, ["--ir", source]).stdout
    for level in ["-O0", "-O1"]:
        artifact = invoke(compiler, ["--ir", level, source]).stdout
        if level == "-O0": assert artifact == expected
        else: assert b"func main() -> int" in artifact
        result = invoke(compiler, ["--mips", level, source])
        assert result.stdout.startswith(b".data\n") and not result.stderr
    for source, code in [("bad_lex", 2), ("bad_syntax", 2), ("undeclared", 3), ("ir_no_function", 4)]:
        for stage in ["ir", "mips"]:
            expected = 5 if stage == "mips" and source == "ir_no_function" else code
            result = invoke(compiler, [f"--{stage}", f"test/negative/{source}.min"], expected)
            assert not result.stdout and result.stderr


def check_help_and_files(compiler, directory):
    result = invoke(compiler, ["--help"])
    assert not result.stderr and b"Usage:" in result.stdout
    for word in [b"--lex", b"--semantic", b"--mips", b"--verify-ir", b"-o", b"-O1", b"5 backend"]:
        assert word in result.stdout, f"help is missing {word!r}"
    ir = "test/expected/ir-arithmetic.stdout"
    assert invoke(compiler, ["--verify-ir", ir]).stdout == f"verified {ir}\n".encode()
    for args in [["--lex", directory / "missing.min"], ["--verify-ir", directory / "missing.ir"],
                 ["--lex", directory], ["--verify-ir", directory]]:
        result = invoke(compiler, args, 1)
        assert not result.stdout and b"cannot open file" in result.stderr


def check_target_layout(compiler):
    module = invoke(compiler, ["--ir", "test/positive/target_layout.min"]).stdout
    for fragment in [b"layout struct Mixed size 12 align 4\n  field char tag offset 0\n"
                     b"  field int value offset 4\n  field short tail offset 8\n",
                     b"layout struct Child size 8 align 4\n  field char base offset 0\n"
                     b"  field short middle offset 2\n  field int last offset 4\n",
                     b"layout struct Holder size 8 align 4", b"layout union Overlap size 4 align 4",
                     b"field char c offset 0", b"field int value offset 0", b"field short s offset 0"]:
        assert fragment in module, f"incorrect target layout: {fragment!r}"
    literals = invoke(compiler, ["--ir", "test/positive/literal_bytes.min"]).stdout
    for fragment in [b"decl char[7] main.inferred", b"decl char[2] main.exact", b"const char 10",
                     b"const char 39", b"const char 92", b"const char 0", b"const int 16", b"const int 3"]:
        assert fragment in literals, f"incorrect literal bytes: {fragment!r}"


def check_global_data(compiler):
    module = invoke(compiler, ["--ir", "test/positive/global_arrays.min"]).stdout
    for fragment in [b"global int[4] counts", b"data int counts + 0 = 1", b"data int counts + 4 = 2",
                     b"data int matrix + 12 = 5", b"global char[5] message", b"data char message + 1 = 10",
                     b"data char message + 2 = 0", b"data char message + 3 = 98", b"global char[2] exact"]:
        assert fragment in module, f"incorrect initialized data: {fragment!r}"
    assert b"data int counts + 8" not in module and b"data int counts + 12" not in module
    reference = invoke(compiler, ["--ir", "test/positive/references.min"]).stdout
    assert b"&" not in reference, "source references escaped into serialized IR"


def check_switch_strategies(compiler):
    for fixture, strategy in [("chain", "chain"), ("dense", "table"), ("sparse", "tree")]:
        assembly = invoke(compiler, ["--mips", f"test/positive/switch_{fixture}.min"]).stdout
        assert f"_switch_{strategy}".encode() in assembly, f"incorrect switch strategy for {fixture}"
        for other in {"chain", "table", "tree"} - {strategy}:
            assert f"_switch_{other}".encode() not in assembly, f"unexpected {other} strategy for {fixture}"


def check_runtime_emission(compiler):
    literal = invoke(compiler, ["--mips", "test/positive/io_formats.min"]).stdout
    dynamic = invoke(compiler, ["--mips", "test/positive/dynamic_format.min"]).stdout
    assert b"    jal __mv_rt_printf\n" not in literal, "literal printf used the format walker"
    assert b"    jal __mv_rt_printf\n" in dynamic, "dynamic printf did not use the format walker"
    for name in ["printf", "scanf", "malloc", "free", "snapshot", "rewind", "history_clear"]:
        assert literal.count(f"\n__mv_rt_{name}:\n".encode()) == 1, f"duplicate or missing {name} helper"


# Group the existing CLI, IR and assembly assertions in one pass.
def run_cli_checks(compiler, directory):
    checks = [(f"cli-{name}", check_usage, (args,)) for name, args in invalid_invocations()]
    for stage in ["lex", "ast", "typed-ast", "semantic", "ir", "mips"]:
        checks.append((f"cli-routing-{stage}", check_routing, (directory, stage)))
    failures = [("lex", "bad_lex", 2), ("ast", "bad_syntax", 2), ("typed-ast", "undeclared", 3),
                ("semantic", "undeclared", 3), ("ir", "ir_no_function", 4), ("mips", "ir_no_function", 5)]
    for stage, name, code in failures:
        folder = "negative/"
        checks.append((f"cli-preserve-{stage}", check_preserved_output,
                       (directory, stage, f"test/{folder}{name}.min", code)))
    checks.extend([
        ("cli-stage-isolation", check_stage_isolation, ()),
        ("cli-defaults-aliases", check_defaults_and_aliases, (directory,)),
        ("cli-output-errors", check_output_errors, (directory,)),
        ("cli-pipeline-optimization", check_pipeline_and_optimization, ()),
        ("cli-help-files", check_help_and_files, (directory,)),
        ("ir-target-layout-literals", check_target_layout, ()),
        ("ir-global-data-references", check_global_data, ()),
        ("mips-switch-strategies", check_switch_strategies, ()),
        ("mips-runtime-emission", check_runtime_emission, ()),
    ])
    failed = 0
    for name, check, args in checks:
        try:
            check(compiler, *args)
            errors = []
        except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
            errors = [str(error)]
        failed += report(name, errors)
    return len(checks), failed


# Run the existing compiler checks and the selected execution modes.
def main():
    parser = argparse.ArgumentParser(description="Run Minerva's manifest assertions.")
    parser.add_argument("--compiler", type=Path, default=ROOT / "minerva")
    parser.add_argument("--frontend", action="store_true", help="exclude program execution cases")
    parser.add_argument("--optimization", choices=["0", "1", "both"], default="both",
                        help="runtime optimization modes (default: both)")
    options = parser.parse_args()
    try:
        cases = load_cases()
        if options.frontend:
            cases = [case for case in cases if case["stage"] != "run"]
        runtime_count = sum(case["stage"] == "run" for case in cases)
        if runtime_count and not shutil.which("spim"):
            raise ValueError("SPIM is required for runtime cases; install spim or use --frontend")
        compiler = str(options.compiler.resolve())
        with tempfile.TemporaryDirectory(prefix="minerva-tests-") as directory:
            failed = run_cases(compiler, cases, options.optimization)
            cli_count, cli_failed = run_cli_checks(compiler, Path(directory))
    except (OSError, ValueError, KeyError) as error:
        print(f"test configuration error: {error}", file=sys.stderr)
        return 1
    total, failed = len(cases) + cli_count, failed + cli_failed
    print(f"Total: {total}, passed: {total - failed}, failed: {failed}")
    if runtime_count:
        levels = "-O0 and -O1" if options.optimization == "both" else "-O" + options.optimization
        print(f"Runtime: {runtime_count} programs checked under {levels}")
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())
