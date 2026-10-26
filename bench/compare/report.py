"""Render case-level medians without treating within-process samples as independent."""
import argparse
import json
from pathlib import Path
import statistics


def summarize(runs, engine):
    medians = [statistics.median(run["samples_ns"]) / run["rounds"]
               for run in runs if run["engine"] == engine]
    if not medians:
        raise ValueError(f"no runs for {engine}")
    return statistics.median(medians), min(medians), max(medians)


def render(result):
    info = result["metadata"]
    # Old two-VM result files retain their original set of columns.
    engines = result.get("settings", {}).get("engines", list(info.get("binaries", {"lhat": {}, "luau": {}})))
    labels = {"lhat": "L^", "luau": "Luau", "lua": "Lua"}
    names = " / ".join(labels[e] for e in engines)
    ratios = [engine for engine in engines if engine != "lhat"]
    compiler_summary = f"C / C++: {info['c_compiler']} / {info['cxx_compiler']}"
    if "engine_builds" in info:
        compiler_summary = "Compilers: " + "; ".join(f"{labels[e]}: {info['engine_builds'][e]['compiler']}" for e in engines)
    lines = [f"# {names} VM comparison", "", f"CPU: {info['cpu']}",
             f"OS: {info['os']}", compiler_summary,
             f"Luau bytecode: O{info['luau_optimization']}; Release; PGO/JIT/debugger off.",
             f"Host setup: {info.get('host_setup', 'see runner source snapshot')}.",
             f"L^ commit: `{info['repositories']['lhat']['commit']}`; local changes: {bool(info['repositories']['lhat']['status'])}.",
             f"Luau commit: `{info['repositories']['luau']['commit']}`."]
    if "lua" in engines:
        lines.append(f"Official Lua: {info['lua_version']} (pinned release, source SHA-256 in results.json).")
    if info.get("comparison_mode") == "mixed_compilers":
        lines.append("Comparison mode: mixed compilers (selected build per VM); this is separate from the uniform-compiler baseline.")
    lines += ["",
             "Times are microseconds per workload round. Brackets show the range of process medians, not a confidence interval.",
             "Ratios = L^ time / the named VM time; below 1 means L^ is faster. Each round includes the setup specified in README.md.", "",
             "| Case | " + " | ".join([f"{labels[e]} µs [min–max]" for e in engines] + [f"L^ / {labels[e]}" for e in ratios]) + " |",
             "| --- | " + " | ".join(["---:"] * (len(engines) + len(ratios))) + " |"]
    for case in result["cases"]:
        name = case["spec"]["id"]
        if case["status"] != "ok" or not case["runs"]:
            lines.append(f"| {name} | " + " | ".join(["FAILED / no timing"] + ["—"] * (len(engines) + len(ratios) - 1)) + " |")
            continue
        times = {e: summarize(case["runs"], e) for e in engines}
        columns = [f"{times[e][0]/1000:.2f} [{times[e][1]/1000:.2f}–{times[e][2]/1000:.2f}]" for e in engines]
        columns += [f"{times['lhat'][0]/times[e][0]:.3f}" for e in ratios]
        lines.append(f"| {name} | " + " | ".join(columns) + " |")
    lines += ["", "Cold runs use a fresh process, one workload round, zero warmups. OS file caches are not flushed.",
              "Main → first result excludes OS process creation; process wall time includes creation, JSON output and teardown.", "",
              f"| Case | Main → result {names} ms | Process wall {names} ms | Peak process {names} MiB |",
              "| --- | ---: | ---: | ---: |"]
    for case in result["cases"]:
        if case["status"] != "ok" or not case.get("cold_runs"):
            continue
        pairs = []
        for field, scale in (("first_result_from_main_ns", 1e6), ("process_wall_ns", 1e6), ("peak_process_bytes", 1024**2)):
            pairs.append(" / ".join(f"{statistics.median(r[field] for r in case['cold_runs'] if r['engine'] == e)/scale:.2f}"
                                    for e in engines))
        lines.append(f"| {case['spec']['id']} | {' | '.join(pairs)} |")
    lines += ["", "Peak process memory includes compiler and VM setup; this is not VM heap allocation or a steady-state memory measurement.",
              "L^ check_load_ns includes source loading, parsing and strict checking. Luau/Lua compile_ns includes parsing and bytecode compilation, with no type analysis.",
              "These preparation timings are retained in results.json and are not presented as equivalent frontend work.", ""]
    failed = [case for case in result["cases"] if case["status"] != "ok"]
    if failed:
        lines += ["Failures:", ""] + [f"- {c['spec']['id']}: {c.get('error', 'incomplete')}" for c in failed] + [""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    content = render(json.loads(args.results.read_text(encoding="utf-8")))
    if args.output:
        args.output.write_text(content, encoding="utf-8")
    else:
        print(content)


if __name__ == "__main__":
    main()
