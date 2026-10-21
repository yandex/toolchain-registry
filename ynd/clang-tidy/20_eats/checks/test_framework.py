#!/usr/bin/env python3
"""Runs clang-tidy on the eats-* test files and compares diagnostics with annotations.

Annotations are inline comments on the line the diagnostic points to:
- EXPECT-WARNING[: reason]            exactly one warning of the check
- EXPECT-WARNING-COUNT: N[: reason]   exactly N warnings of the check
- EXPECT-NO-WARNING[: reason]         no warnings of the check
Lines without an annotation must not get warnings either.
"""

import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

CHECKS_ROOT = Path(__file__).resolve().parent
TESTS_DIR = CHECKS_ROOT / "tests"
CHECKS_MAP_PATH = CHECKS_ROOT / "test_checks.json"

Location = Tuple[str, int]

_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
_WARNING_COUNT_MARKER = re.compile(r"EXPECT-WARNING-COUNT(?:\s*$|\s*:)")
_WARNING_COUNT = re.compile(r"EXPECT-WARNING-COUNT\s*:\s*([1-9]\d*)\s*(?::.*)?$")
_WARNING = re.compile(r"EXPECT-WARNING(?:\s*$|\s*:)")
_NO_WARNING = re.compile(r"EXPECT-NO-WARNING(?:\s*$|\s*:)")
_DIAGNOSTIC = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):\d+: (?P<severity>warning|error|fatal error): .*?(?:\[(?P<checks>[^\]]+)\])?$"
)


@dataclass
class TidyOutput:
    warnings: Counter = field(default_factory=Counter)
    messages: Dict[Location, List[str]] = field(default_factory=dict)
    failures: List[str] = field(default_factory=list)


@dataclass
class CheckResult:
    test_file: Path
    check: str
    missing: Counter
    unexpected: Counter
    failures: List[str]
    messages: Dict[Location, List[str]]

    @property
    def passed(self) -> bool:
        return not (self.missing or self.unexpected or self.failures)


def location(file: str, line: int) -> Location:
    return (str(Path(file).resolve()), line)


def annotated_files(test_file: Path) -> List[Path]:
    """The test file and the local headers it includes with quotes, recursively."""
    result: List[Path] = []
    pending = [test_file.resolve()]
    while pending:
        path = pending.pop()
        if path in result or not path.exists():
            continue
        result.append(path)
        for text in path.read_text().splitlines():
            include = _INCLUDE.match(text)
            if include:
                pending.append((path.parent / include.group(1)).resolve())
    return result


def parse_expectations(test_file: Path) -> Counter:
    """Expected number of warnings per location."""
    expected: Counter = Counter()
    for path in annotated_files(test_file):
        for number, text in enumerate(path.read_text().splitlines(), 1):
            key = location(str(path), number)
            if _WARNING_COUNT_MARKER.search(text):
                count = _WARNING_COUNT.search(text)
                if not count:
                    raise ValueError(f"{path}:{number}: expected EXPECT-WARNING-COUNT: N[: reason] with N > 0")
                expected[key] = int(count.group(1))
            elif _WARNING.search(text):
                expected[key] = 1
            if _NO_WARNING.search(text) and key in expected:
                raise ValueError(f"{path}:{number}: both EXPECT-WARNING and EXPECT-NO-WARNING")
    return expected


def parse_tidy_output(output: str, check: str) -> TidyOutput:
    """Collects warnings of the check; compile errors and unattributed diagnostics are failures."""
    result = TidyOutput()
    check_marker = re.compile(rf"\[{re.escape(check)}[,\]]")
    for text in output.splitlines():
        diagnostic = _DIAGNOSTIC.match(text)
        checks = (diagnostic.group("checks") or "").split(",") if diagnostic else []
        if diagnostic and check in checks:
            key = location(diagnostic.group("file"), int(diagnostic.group("line")))
            result.warnings[key] += 1
            result.messages.setdefault(key, []).append(text)
        elif diagnostic and diagnostic.group("severity") != "warning":
            result.failures.append(f"compile error: {text}")
        elif check_marker.search(text) and re.search(r"\b(?:warning|error):", text):
            result.failures.append(f"diagnostic without source location: {text}")
    return result


class ClangTidyRunner:
    def __init__(
        self,
        clang_tidy: str,
        plugin: Optional[str],
        std: str,
        extra_args: Sequence[str] = (),
        timeout: Optional[float] = None,
    ):
        self.clang_tidy = clang_tidy
        self.plugin = plugin
        self.std = std
        self.extra_args = list(extra_args)
        self.timeout = timeout

    def command(self, test_file: Path, check: str) -> List[str]:
        command = [self.clang_tidy, str(test_file), f"--checks=-*,{check}", "--header-filter=.*", "--config={}"]
        if self.plugin:
            command.append(f"--load={self.plugin}")
        return command + ["--", f"-std={self.std}", *self.extra_args]

    def run(self, test_file: Path, check: str) -> TidyOutput:
        try:
            completed = subprocess.run(
                self.command(test_file, check), capture_output=True, text=True, timeout=self.timeout
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return TidyOutput(failures=[f"clang-tidy did not finish: {error}"])
        output = parse_tidy_output(completed.stdout + completed.stderr, check)
        if completed.returncode != 0:
            output.failures.append(f"clang-tidy exited with status {completed.returncode}: {completed.stderr.strip()}")
        return output


def run_test(runner: ClangTidyRunner, test_file: Path, check: str) -> CheckResult:
    try:
        expected = parse_expectations(test_file)
    except (OSError, ValueError) as error:
        return CheckResult(test_file, check, Counter(), Counter(), [str(error)], {})
    output = runner.run(test_file, check)
    return CheckResult(
        test_file=test_file,
        check=check,
        missing=expected - output.warnings,
        unexpected=output.warnings - expected,
        failures=output.failures,
        messages=output.messages,
    )


def load_checks_map(path: Path = CHECKS_MAP_PATH) -> Dict[str, str]:
    with path.open() as file:
        return json.load(file)


def select_tests(files: Sequence[Path], checks_map: Dict[str, str]) -> List[Tuple[Path, str]]:
    if not files:
        return [(TESTS_DIR / name, check) for name, check in sorted(checks_map.items())]
    missing = [str(file) for file in files if not file.is_file()]
    if missing:
        raise SystemExit(f"no such test file: {', '.join(missing)}")
    unmapped = [str(file) for file in files if file.name not in checks_map]
    if unmapped:
        raise SystemExit(f"not in {CHECKS_MAP_PATH.name}: {', '.join(unmapped)}")
    return [(file.resolve(), checks_map[file.name]) for file in files]


def format_result(result: CheckResult) -> str:
    status = "PASSED" if result.passed else "FAILED"
    lines = [f"{status} {result.test_file.name} [{result.check}]"]
    for (file, line), count in sorted(result.missing.items()):
        lines.append(f"  missing {count} warning(s) at {file}:{line}")
    for (file, line), count in sorted(result.unexpected.items()):
        lines.append(f"  unexpected {count} warning(s) at {file}:{line}")
        lines.extend(f"    {message}" for message in result.messages.get((file, line), []))
    lines.extend(f"  {failure}" for failure in result.failures)
    return "\n".join(lines)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run eats-* clang-tidy check tests")
    parser.add_argument("--clang-tidy", default="clang-tidy-20", help="clang-tidy 20 binary (default: clang-tidy-20)")
    parser.add_argument("--plugin", help="libEatsTidyPlugin.so; omit for a binary with the checks built in")
    parser.add_argument("--std", default="c++20", help="C++ standard of test files (default: c++20)")
    parser.add_argument("--extra-arg", action="append", default=[], help="extra compiler argument, repeatable")
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds per clang-tidy run")
    parser.add_argument("files", nargs="*", type=Path, help="test files to run; all mapped tests by default")
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    runner = ClangTidyRunner(args.clang_tidy, args.plugin, args.std, args.extra_arg, args.timeout)
    results = [run_test(runner, file, check) for file, check in select_tests(args.files, load_checks_map())]
    for result in results:
        print(format_result(result))
    failed = sum(not result.passed for result in results)
    print(f"{len(results) - failed}/{len(results)} passed")
    return 1 if failed or not results else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
