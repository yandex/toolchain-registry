import re
import unittest
from pathlib import Path
from typing import Dict, Iterable, List, Set

from test_framework import CHECKS_ROOT, TESTS_DIR, load_checks_map

TIDY_MODULE_PATH = CHECKS_ROOT / "tidy_module_eats.cpp"

_REGISTERED_CHECK = re.compile(r'registerCheck<[^>]+>\(\s*"([^"]+)"\s*\)')


def registered_checks(module_source: str) -> Set[str]:
    return set(_REGISTERED_CHECK.findall(module_source))


def catalog_problems(checks: Set[str], checks_map: Dict[str, str], test_files: Iterable[str]) -> List[str]:
    test_files = set(test_files)
    problems = [f"{check}: name must start with eats-" for check in sorted(checks) if not check.startswith("eats-")]
    tested = set(checks_map.values())
    problems += [f"{check}: no test file in test_checks.json" for check in sorted(checks - tested)]
    problems += [f"{check}: mapped in test_checks.json but not registered" for check in sorted(tested - checks)]
    problems += [f"{name}: mapped in test_checks.json but missing in tests/" for name in sorted(set(checks_map) - test_files)]
    problems += [f"{name}: not mapped in test_checks.json, so never run" for name in sorted(test_files - set(checks_map))]
    return problems


class CatalogTest(unittest.TestCase):
    def test_every_eats_check_has_a_test_file(self):
        checks = registered_checks(TIDY_MODULE_PATH.read_text())
        test_files = (path.name for path in TESTS_DIR.glob("test_*.cpp"))

        self.assertTrue(checks)
        self.assertEqual(catalog_problems(checks, load_checks_map(), test_files), [])


class CatalogProblemsTest(unittest.TestCase):
    def test_check_without_test_file_is_reported(self):
        problems = catalog_problems({"eats-a", "eats-b"}, {"test_a.cpp": "eats-a"}, ["test_a.cpp"])

        self.assertEqual(problems, ["eats-b: no test file in test_checks.json"])

    def test_stale_mapping_is_reported(self):
        problems = catalog_problems(set(), {"test_a.cpp": "eats-a"}, [])

        self.assertEqual(
            problems,
            [
                "eats-a: mapped in test_checks.json but not registered",
                "test_a.cpp: mapped in test_checks.json but missing in tests/",
            ],
        )

    def test_unmapped_test_file_is_reported(self):
        problems = catalog_problems({"eats-a"}, {"test_a.cpp": "eats-a"}, ["test_a.cpp", "test_b.cpp"])

        self.assertEqual(problems, ["test_b.cpp: not mapped in test_checks.json, so never run"])

    def test_check_outside_eats_namespace_is_reported(self):
        problems = catalog_problems({"misc-a"}, {"test_a.cpp": "misc-a"}, ["test_a.cpp"])

        self.assertEqual(problems, ["misc-a: name must start with eats-"])

    def test_registered_checks_are_read_from_module_source(self):
        source = """
            checkFactories.registerCheck<FirstCheck>("eats-first");
            checkFactories.registerCheck<SecondCheck>(
                "eats-second");
        """

        self.assertEqual(registered_checks(source), {"eats-first", "eats-second"})


if __name__ == "__main__":
    unittest.main()
