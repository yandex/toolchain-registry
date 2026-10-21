import subprocess
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from unittest.mock import patch

from test_framework import ClangTidyRunner, location, parse_expectations, parse_tidy_output, run_test

CHECK = "eats-sample"


class SourceTestCase(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def write(self, name, source):
        path = self.root / name
        path.write_text(source)
        return path


class ParseExpectationsTest(SourceTestCase):
    def test_annotations_set_expected_counts(self):
        test_file = self.write(
            "test.cpp",
            "int a; // EXPECT-WARNING\n"
            "int b; // EXPECT-WARNING: with reason\n"
            "int c; // EXPECT-WARNING-COUNT: 2: two findings\n"
            "int d; // EXPECT-NO-WARNING\n"
            "int e;\n",
        )

        expected = parse_expectations(test_file)

        self.assertEqual(
            expected,
            Counter({location(str(test_file), 1): 1, location(str(test_file), 2): 1, location(str(test_file), 3): 2}),
        )

    def test_annotations_in_included_local_headers_are_collected(self):
        header = self.write("helper.h", "int h; // EXPECT-WARNING\n")
        test_file = self.write("test.cpp", '#include "helper.h"\n')

        self.assertEqual(parse_expectations(test_file), Counter({location(str(header), 1): 1}))

    def test_marker_name_in_prose_is_not_an_annotation(self):
        test_file = self.write("test.cpp", "// EXPECT-WARNING marks a line with one warning\n")

        self.assertEqual(parse_expectations(test_file), Counter())

    def test_count_marker_name_in_prose_is_not_an_annotation(self):
        test_file = self.write("test.cpp", "// use EXPECT-WARNING-COUNT for several findings\n")

        self.assertEqual(parse_expectations(test_file), Counter())

    def test_invalid_warning_count_is_rejected(self):
        for annotation in ("EXPECT-WARNING-COUNT: 0", "EXPECT-WARNING-COUNT: two", "EXPECT-WARNING-COUNT"):
            with self.subTest(annotation=annotation):
                test_file = self.write("test.cpp", f"int a; // {annotation}\n")
                with self.assertRaisesRegex(ValueError, "EXPECT-WARNING-COUNT"):
                    parse_expectations(test_file)

    def test_contradicting_annotations_are_rejected(self):
        test_file = self.write("test.cpp", "int a; // EXPECT-NO-WARNING: but EXPECT-WARNING\n")

        with self.assertRaisesRegex(ValueError, "both"):
            parse_expectations(test_file)


class ParseTidyOutputTest(unittest.TestCase):
    def test_only_warnings_of_the_check_are_counted(self):
        output = parse_tidy_output(
            "/src/test.cpp:3:5: warning: sample finding [eats-sample]\n"
            "/src/test.cpp:3:5: warning: other finding [eats-other]\n"
            "/src/test.cpp:4:1: error: sample finding [eats-sample,-warnings-as-errors]\n"
            "/src/test.cpp:3:5: note: some context\n",
            CHECK,
        )

        self.assertEqual(output.warnings, Counter({location("/src/test.cpp", 3): 1, location("/src/test.cpp", 4): 1}))
        self.assertEqual(output.failures, [])

    def test_compile_error_is_a_failure(self):
        output = parse_tidy_output("/src/test.cpp:1:1: error: unknown type name 'Foo' [clang-diagnostic-error]\n", CHECK)

        self.assertEqual(len(output.failures), 1)
        self.assertIn("compile error", output.failures[0])

    def test_fatal_error_is_a_failure(self):
        output = parse_tidy_output("/src/test.cpp:3:10: fatal error: 'exception' file not found\n", CHECK)

        self.assertEqual(len(output.failures), 1)
        self.assertIn("file not found", output.failures[0])

    def test_diagnostic_without_location_is_a_failure(self):
        output = parse_tidy_output("error: sample finding [eats-sample,-warnings-as-errors]\n", CHECK)

        self.assertEqual(output.warnings, Counter())
        self.assertEqual(len(output.failures), 1)


class FakeRunner(ClangTidyRunner):
    def __init__(self, warning_lines):
        super().__init__("clang-tidy", None, "c++20")
        self.warning_lines = warning_lines

    def run(self, test_file, check):
        return parse_tidy_output(
            "".join(f"{test_file}:{line}:1: warning: finding [{check}]\n" for line in self.warning_lines), check
        )


class RunTestTest(SourceTestCase):
    def run_source(self, source, warning_lines):
        return run_test(FakeRunner(warning_lines), self.write("test.cpp", source), CHECK)

    def test_matching_warnings_pass(self):
        self.assertTrue(self.run_source("int a; // EXPECT-WARNING\nint b; // EXPECT-NO-WARNING\n", [1]).passed)

    def test_missing_warning_fails(self):
        result = self.run_source("int a; // EXPECT-WARNING\n", [])

        self.assertFalse(result.passed)
        self.assertEqual(sum(result.missing.values()), 1)

    def test_warning_on_no_warning_line_fails(self):
        result = self.run_source("int a; // EXPECT-NO-WARNING\n", [1])

        self.assertFalse(result.passed)
        self.assertEqual(sum(result.unexpected.values()), 1)

    def test_warning_on_unannotated_line_fails(self):
        self.assertFalse(self.run_source("int a;\n", [1]).passed)

    def test_warning_count_is_compared_exactly(self):
        source = "int a; // EXPECT-WARNING-COUNT: 2\n"

        self.assertTrue(self.run_source(source, [1, 1]).passed)
        self.assertFalse(self.run_source(source, [1]).passed)
        self.assertFalse(self.run_source(source, [1, 1, 1]).passed)

    def test_invalid_annotation_fails_without_running_clang_tidy(self):
        result = self.run_source("int a; // EXPECT-WARNING-COUNT: 0\n", [])

        self.assertFalse(result.passed)


class ClangTidyRunnerTest(unittest.TestCase):
    def test_command_loads_plugin_and_sets_standard(self):
        runner = ClangTidyRunner("clang-tidy-20", "/build/libEatsTidyPlugin.so", "c++17", ["-DX"])

        self.assertEqual(
            runner.command(Path("/src/test.cpp"), CHECK),
            [
                "clang-tidy-20",
                "/src/test.cpp",
                "--checks=-*,eats-sample",
                "--header-filter=.*",
                "--config={}",
                "--load=/build/libEatsTidyPlugin.so",
                "--",
                "-std=c++17",
                "-DX",
            ],
        )

    def test_command_without_plugin_has_no_load(self):
        command = ClangTidyRunner("clang-tidy", None, "c++20").command(Path("/src/test.cpp"), CHECK)

        self.assertFalse(any(argument.startswith("--load") for argument in command))

    def test_nonzero_exit_status_is_a_failure(self):
        completed = subprocess.CompletedProcess([], 1, "", "Error: no checks enabled.\n")

        with patch("test_framework.subprocess.run", return_value=completed):
            output = ClangTidyRunner("clang-tidy", None, "c++20").run(Path("/src/test.cpp"), CHECK)

        self.assertEqual(output.failures, ["clang-tidy exited with status 1: Error: no checks enabled."])

    def test_timeout_is_a_failure(self):
        with patch("test_framework.subprocess.run", side_effect=subprocess.TimeoutExpired("clang-tidy", 1)):
            output = ClangTidyRunner("clang-tidy", None, "c++20", timeout=1).run(Path("/src/test.cpp"), CHECK)

        self.assertEqual(len(output.failures), 1)


if __name__ == "__main__":
    unittest.main()
