"""Validate, calibrate, and compare independent L^, Luau and official Lua VMs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys
import time
from dependencies import lua_source_hash

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ENGINES = ("lhat", "luau", "lua")
SUFFIXES = {"lhat": ".lh", "luau": ".luau", "lua": ".lua"}


def engine_order(first_order, index):
    """Rotate the first order so every VM occupies each position in a cycle."""
    offset = index % len(first_order)
    return first_order[offset:] + first_order[:offset]


def expected(case, rounds=1):
    """Independent closed-form oracles, not values copied from either VM."""
    n, seed = case["size"], case["seed"]
    kind = case["oracle"]
    if kind == "arithmetic":
        value = seed + n * (n - 1) // 2 + n + 2 * min(n, seed)
    elif kind == "calls":
        a, b = 0, 1
        for _ in range(seed):
            a, b = b, a + b
        value = n * a
    elif kind == "array":
        value = 4 * (n * (n - 1) // 2 + n * seed) + 10 * n
    elif kind == "fields":
        value = n * (2 * seed + 1) + 3 * n * (n + 1) // 2
    elif kind == "allocation":
        value = n * (n - 1) + n * (2 * seed + 1)
    elif kind == "entities":
        value = 32 * (n * (n - 1) // 2 + n * seed) + 528 * n
    else:
        raise ValueError(f"unknown oracle: {kind}")
    value *= rounds
    if not 0 <= value <= 2**53:
        raise ValueError("checksum must remain an exactly representable nonnegative integer")
    return value


def validate_result(result, engine, case, rounds, samples):
    if result.get("schema") != 1 or result.get("engine") != engine:
        raise ValueError("runner schema or engine mismatch")
    checksum = result.get("checksum")
    target = expected(case, rounds)
    if not isinstance(checksum, (int, float)) or not math.isfinite(checksum) or abs(checksum - target) > (
        case["abs_tolerance"] + case["rel_tolerance"] * abs(target)
    ):
        raise ValueError(f"checksum mismatch: {checksum!r}, expected {target}")
    times = result.get("samples_ns")
    if not isinstance(times, list) or len(times) != samples or any(
        not isinstance(t, (int, float)) or not math.isfinite(t) or t <= 0 for t in times
    ):
        raise ValueError("runner returned invalid timing samples")


def invoke(binary, engine, case, rounds, warmups, samples, timeout):
    source = HERE / "cases" / (case["id"] + SUFFIXES[engine])
    command = [str(binary), str(source), str(rounds), str(case["size"]), str(case["seed"]),
               str(warmups), str(samples), str(expected(case, rounds)),
               str(case["abs_tolerance"]), str(case["rel_tolerance"]), "v1"]
    started = time.perf_counter_ns()
    process = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    wall_ns = time.perf_counter_ns() - started
    if process.returncode:
        raise RuntimeError(f"{engine}/{case['id']} exited {process.returncode}: {process.stderr.strip()}")
    try:
        result = json.loads(process.stdout)
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid runner JSON: {process.stdout[:500]!r}; {process.stderr}") from error
    validate_result(result, engine, case, rounds, samples)
    result.update(rounds=rounds, size=case["size"], seed=case["seed"], warmups=warmups,
                  process_wall_ns=wall_ns, stderr=process.stderr)
    return result


def git_info(path):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(path), *args], text=True, encoding="utf-8", errors="replace").strip()
    return {"commit": git("rev-parse", "HEAD"), "status": git("status", "--porcelain"),
            "tracked_diff_sha256": hashlib.sha256(git("diff", "HEAD", "--", ".").encode()).hexdigest()}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_build_info(build, config):
    info_path = build / f"compare-build-{config}.json"
    if not info_path.is_file():
        raise ValueError(f"missing build manifest: {info_path}")
    info = json.loads(info_path.read_text())
    if info["configuration"] != "Release":
        raise ValueError("performance comparisons require a Release build")
    return info


def metadata(builds, binaries, config):
    info = read_build_info(builds["lhat"], config)
    info["engine_builds"] = {}
    for engine, build in builds.items():
        selected = read_build_info(build, config)
        if selected.get("jit") or selected.get("pgo") or selected.get("debugger"):
            raise ValueError(f"{engine} build must use the baseline JIT/PGO/debugger settings")
        selected["directory"] = str(build)
        selected["compiler"] = selected["cxx_compiler"] if engine == "luau" else selected["c_compiler"]
        info["engine_builds"][engine] = selected
    info["comparison_mode"] = "same_compilers" if len({b["compiler"] for b in info["engine_builds"].values()}) == 1 else "mixed_compilers"
    luau_path = None
    lua_path = None
    for line in (builds["luau"] / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("LUAU_SOURCE_DIR:PATH="):
            luau_path = Path(line.split("=", 1)[1])
    for line in (builds["lua"] / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("LUA_SOURCE_DIR:PATH="):
            lua_path = Path(line.split("=", 1)[1])
    if luau_path is None:
        raise ValueError("LUAU_SOURCE_DIR is missing in CMakeCache.txt")
    info["repositories"] = {"lhat": git_info(ROOT), "luau": git_info(luau_path)}
    lock = json.loads((HERE / "luau.lock.json").read_text())
    luau_build = info["engine_builds"]["luau"]
    if (info["repositories"]["luau"]["commit"] != lock["commit"] or info["repositories"]["luau"]["status"]
            or luau_build["luau_commit"] != lock["commit"]):
        raise ValueError("Luau dependency must be clean and match luau.lock.json")
    lua_lock = json.loads((HERE / "lua.lock.json").read_text())
    if lua_path is None or lua_source_hash(lua_path) != lua_lock["source_sha256"]:
        raise ValueError("Lua dependency must match lua.lock.json; run prepare.py and reconfigure")
    lua_build = info["engine_builds"]["lua"]
    if lua_build.get("lua_source_sha256") != lua_lock["source_sha256"] or lua_build.get("lua_version") != lua_lock["version"]:
        raise ValueError("Lua build manifest is stale; reconfigure and rebuild")
    info["luau_optimization"] = luau_build["luau_optimization"]
    info["lua_version"] = lua_build["lua_version"]
    info["lua_source"] = dict(lua_lock, path=str(lua_path))
    info.update(os=platform.platform(), architecture=platform.machine(), cpu=platform.processor(),
                python=sys.version, timer="QueryPerformanceCounter" if os.name == "nt" else "CLOCK_MONOTONIC",
                host_setup="core only; no standard libraries registered; Luau globals sandboxed",
                binaries={engine: {"path": str(path), "sha256": sha256(path)} for engine, path in binaries.items()})
    if os.name == "nt":
        cpu = subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_Processor).Name"],
                             capture_output=True, text=True, timeout=30)
        if cpu.returncode == 0:
            info["cpu"] = cpu.stdout.strip()
    info["source_sha256"] = {str(path.relative_to(HERE)): sha256(path)
                             for path in sorted(HERE.rglob("*"))
                             if path.is_file() and "results" not in path.relative_to(HERE).parts
                             and "__pycache__" not in path.parts and path.suffix in (".c", ".cpp", ".h", ".lh", ".luau", ".lua", ".py", ".json", ".txt")}
    return info


def write_json(path, data):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    # Windows file indexers can briefly hold a destination without delete
    # sharing. Preserve atomic writes and retry only that transient failure.
    for attempt in range(10):
        try:
            temporary.replace(path)
            return
        except PermissionError:
            if attempt == 9:
                raise
            time.sleep(0.05 * (attempt + 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/compare-clang")
    parser.add_argument("--lua-build-dir", type=Path, help="use Lua and luac from a separate Release build; label compilers per VM")
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify", action="store_true", help="only validate default and edge inputs")
    parser.add_argument("--quick", action="store_true", help="2 processes, 1 warmup, 3 samples, 50 ms target")
    parser.add_argument("--case", action="append", dest="case_ids")
    parser.add_argument("--processes", type=int)
    parser.add_argument("--warmups", type=int)
    parser.add_argument("--samples", type=int)
    parser.add_argument("--target-ms", type=float)
    parser.add_argument("--timeout", type=float, default=120, help="seconds per child process")
    parser.add_argument("--order-seed", type=int, default=20261026)
    args = parser.parse_args()
    processes = args.processes if args.processes is not None else (2 if args.quick else 5)
    warmups = args.warmups if args.warmups is not None else (1 if args.quick else 3)
    samples = args.samples if args.samples is not None else (3 if args.quick else 10)
    target_ms = args.target_ms if args.target_ms is not None else (50 if args.quick else 250)
    if not 1 <= processes <= 100 or not 0 <= warmups <= 1000 or not 1 <= samples <= 10000:
        parser.error("invalid process/warmup/sample count")
    if not math.isfinite(target_ms) or target_ms <= 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("target and timeout must be finite positive values")
    build = args.build_dir.resolve()
    builds = {engine: build for engine in ENGINES}
    if args.lua_build_dir:
        builds["lua"] = args.lua_build_dir.resolve()
    extension = ".exe" if os.name == "nt" else ""
    binaries = {engine: builds[engine] / "bin" / (engine + "_compare" + extension) for engine in ENGINES}
    for path in binaries.values():
        if not path.is_file():
            parser.error(f"build runner first: {path}")
    lua_bytecode = builds["lua"] / "bin" / ("lua_bytecode" + extension)
    if not lua_bytecode.is_file():
        parser.error(f"build Lua bytecode tool first: {lua_bytecode}")
    manifest = json.loads((HERE / "manifest.json").read_text())
    cases = manifest["cases"]
    if args.case_ids:
        unknown = set(args.case_ids) - {case["id"] for case in cases}
        if unknown:
            parser.error(f"unknown cases: {sorted(unknown)}")
        cases = [case for case in cases if case["id"] in args.case_ids]
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = (args.output or HERE / "results" / timestamp).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "results.json").exists():
        parser.error("output already contains results.json; choose a fresh directory")
    result = {"schema": 1, "timestamp": timestamp, "mode": "verify" if args.verify else "benchmark",
              "settings": {"engines": list(ENGINES), "processes": processes, "warmups": warmups, "samples": samples,
                           "target_ms_slower_engine": target_ms, "order_seed": args.order_seed, "timeout": args.timeout},
              "metadata": metadata(builds, binaries, args.configuration), "cases": []}
    for name in ("CMakeCache.txt", "compile_commands.json", f"compare-build-{args.configuration}.json"):
        if (build / name).is_file():
            shutil.copyfile(build / name, output / name)
    if len(set(builds.values())) > 1:
        for engine, selected in builds.items():
            destination = output / ("build-" + engine)
            destination.mkdir()
            for name in ("CMakeCache.txt", "compile_commands.json", f"compare-build-{args.configuration}.json"):
                if (selected / name).is_file():
                    shutil.copyfile(selected / name, destination / name)
    rng = random.Random(args.order_seed)
    failed = False
    for case in cases:
        record = {"spec": case, "status": "running", "verification": [], "calibration": [], "runs": []}
        result["cases"].append(record)
        write_json(output / "results.json", result)
        print(f"{case['id']}: verifying", flush=True)
        try:
            for engine in ENGINES:
                source = str(HERE / "cases" / (case["id"] + SUFFIXES[engine]))
                command = [str(lua_bytecode), "-l", "-p", source] if engine == "lua" else [str(binaries[engine]), "--dump-bytecode", source]
                dump = subprocess.run(command,
                                      capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=args.timeout)
                if dump.returncode or not dump.stdout.strip():
                    raise RuntimeError(f"bytecode dump failed: {engine}/{case['id']}: {dump.stderr}")
                (output / f"{case['id']}.{engine}.bytecode.txt").write_text(dump.stdout, encoding="utf-8")
            # Check whole workloads plus boundary cases and multiple batches.
            for size, seed in ((case["size"], case["seed"]), (1, 0), (7, 3)):
                spec = dict(case, size=size, seed=seed)
                for engine in ENGINES:
                    data = invoke(binaries[engine], engine, spec, 2, 0, 1, args.timeout)
                    record["verification"].append(data)
            if not args.verify:
                rounds = 1
                # Same amount of work on both VMs; calibrate against the slower
                # VM to bound total duration. Retain every pilot measurement.
                for _ in range(8):
                    order = list(ENGINES)
                    rng.shuffle(order)
                    pilots = [invoke(binaries[e], e, case, rounds, 1, 2, args.timeout) for e in order]
                    record["calibration"].extend(pilots)
                    slow_ns = max(statistics.median(p["samples_ns"]) for p in pilots)
                    desired = max(1, min(1000000000, int(rounds * target_ms * 1e6 / slow_ns)))
                    # Bound to exact integer checksums and modest growth per step.
                    desired = min(desired, (2**53) // max(1, expected(case)))
                    if slow_ns >= target_ms * 0.8e6 or desired <= rounds:
                        break
                    rounds = min(desired, rounds * 100)
                record["rounds"] = rounds
                print(f"{case['id']}: {rounds} rounds, {processes} process groups ({', '.join(ENGINES)})", flush=True)
                first_order = list(ENGINES)
                rng.shuffle(first_order)
                for index in range(processes):
                    order = engine_order(first_order, index)
                    for engine in order:
                        data = invoke(binaries[engine], engine, case, rounds, warmups, samples, args.timeout)
                        data["process_index"] = index
                        record["runs"].append(data)
                        write_json(output / "results.json", result)
                    print(f"  process group {index + 1}/{processes} complete", flush=True)
                # Fresh processes, zero warmups, one round: separate from the
                # calibrated steady-state runs (whose first sample does more work).
                record["cold_runs"] = []
                for index in range(processes):
                    for engine in engine_order(first_order, index):
                        data = invoke(binaries[engine], engine, case, 1, 0, 1, args.timeout)
                        data["process_index"] = index
                        record["cold_runs"].append(data)
            record["status"] = "ok"
        except (RuntimeError, ValueError, subprocess.TimeoutExpired, OSError) as error:
            record["status"] = "failed"
            record["error"] = str(error)
            failed = True
            print(f"  FAILED: {error}", file=sys.stderr, flush=True)
        write_json(output / "results.json", result)
    print(output / "results.json", flush=True)
    if not args.verify:
        from report import render
        report = render(result)
        (output / "report.md").write_text(report, encoding="utf-8")
        print(report, flush=True)
    return int(failed)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
