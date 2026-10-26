"""Check independent oracles, aggregation and rejection of broken runs."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from report import render, summarize
from run import ENGINES, SUFFIXES, HERE, ROOT, engine_order, expected, validate_result
from prepare import prepare_lua


class HarnessTests(unittest.TestCase):
    def test_modified_lua_source_is_preserved_and_rejected(self):
        lock = json.loads((HERE / "lua.lock.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            dest = Path(temporary) / "lua"
            (dest / "src").mkdir(parents=True)
            source = dest / "src/lua.h"
            source.write_text("modified local source", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "differs from the pinned release"):
                prepare_lua(dest, lock)
            self.assertEqual(source.read_text(), "modified local source")

    def test_oracles_against_hand_calculated_examples(self):
        examples = {"arithmetic": 12, "calls": 3, "array": 66,
                    "fields": 33, "allocation": 21, "entities": 1872}
        for oracle, value in examples.items():
            with self.subTest(oracle=oracle):
                case = {"oracle": oracle, "size": 3, "seed": 2}
                self.assertEqual(expected(case), value)
                self.assertEqual(expected(case, 2), 2 * value)

    def test_exact_number_limit(self):
        with self.assertRaises(ValueError):
            expected({"oracle": "fields", "size": 1000000, "seed": 1}, 1000000000)

    def test_process_medians_not_pooled_samples(self):
        runs = [{"engine": "lhat", "rounds": 2, "samples_ns": [2, 2, 2000]},
                {"engine": "lhat", "rounds": 2, "samples_ns": [20, 20, 20]}]
        self.assertEqual(summarize(runs, "lhat"), (5.5, 1, 10))

    def test_three_engine_order_balances_positions(self):
        first = list(ENGINES)
        orders = [engine_order(first, i) for i in range(3)]
        for position in range(3):
            self.assertEqual({order[position] for order in orders}, set(ENGINES))
        self.assertEqual(first, list(ENGINES))

    def test_reports_new_and_legacy_results(self):
        for engines in (("lhat", "luau"), ENGINES):
            info = {"cpu": "test", "os": "test", "c_compiler": "test", "cxx_compiler": "test",
                    "luau_optimization": 2, "lua_version": "5.5.1", "binaries": dict.fromkeys(engines),
                    "repositories": {"lhat": {"commit": "lh", "status": ""}, "luau": {"commit": "lu"}}}
            runs = [{"engine": e, "rounds": 1, "samples_ns": [1000 * (i + 1)]} for i, e in enumerate(engines)]
            case = {"spec": {"id": "example"}, "status": "ok", "runs": runs}
            data = {"metadata": info, "cases": [case]}
            report = render(data)
            self.assertIn("L^ / Luau", report)
            if "lua" in engines:
                self.assertIn("L^ / Lua", report)
                self.assertIn("0.333", report)
            else:
                self.assertNotIn("L^ / Lua |", report)
            failed = dict(case, status="failed", error="checksum mismatch")
            self.assertIn("checksum mismatch", render(dict(data, cases=[failed])))
            if "lua" in engines:
                info["engine_builds"] = {e: {"compiler": "MSVC 19" if e == "lua" else "Clang 22"} for e in engines}
                info["comparison_mode"] = "mixed_compilers"
                mixed = render(data)
                self.assertIn("L^: Clang 22; Luau: Clang 22; Lua: MSVC 19", mixed)
                self.assertIn("Comparison mode: mixed compilers", mixed)

    def test_reject_bad_results(self):
        case = {"oracle": "arithmetic", "size": 1, "seed": 0, "abs_tolerance": 0, "rel_tolerance": 0}
        valid = {"schema": 1, "engine": "lhat", "checksum": 1, "samples_ns": [100]}
        validate_result(valid, "lhat", case, 1, 1)
        for change in ({"engine": "luau"}, {"schema": 0}, {"checksum": 2}, {"checksum": float("nan")},
                       {"samples_ns": []}, {"samples_ns": [0]}, {"samples_ns": [-1]}, {"samples_ns": [float("inf")]}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                validate_result(dict(valid, **change), "lhat", case, 1, 1)


class NativeFailureTests(unittest.TestCase):
    def test_all_runners_reject_invalid_work(self):
        build = Path(os.environ.get("COMPARE_BUILD_DIR", str(ROOT / "build/compare-clang")))
        extension = ".exe" if os.name == "nt" else ""
        if not all((build / "bin" / (e + "_compare" + extension)).exists() for e in ENGINES):
            self.skipTest("native runners have not been built")
        for engine in ENGINES:
            binary = str(build / "bin" / (engine + "_compare" + extension))
            source = str(HERE / "cases" / ("arithmetic" + SUFFIXES[engine]))
            # Missing arguments, invalid size, wrong checksum, missing source.
            good = [source, "1", "1", "0", "0", "1", "1", "0", "0", "v1"]
            invalid_size = good.copy()
            invalid_size[2] = "0"
            wrong_checksum = good.copy()
            wrong_checksum[6] = "2"
            missing_source = good.copy()
            missing_source[0] = str(HERE / "cases" / "missing-source")
            for arguments in ([], invalid_size, wrong_checksum, missing_source):
                with self.subTest(engine=engine, arguments=arguments):
                    process = subprocess.run([binary, *arguments], capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertTrue(process.stderr)
                    self.assertEqual(process.stdout, "")


if __name__ == "__main__":
    unittest.main()
