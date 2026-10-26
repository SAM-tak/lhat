"""Optional Windows scheduling controls; all workload/timing logic stays in run.py."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

def main():
    if os.name != 'nt':
        raise SystemExit('run_controlled.py requires Windows; use run.py on other platforms')
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--affinity-mask', type=lambda s: int(s, 0))
    own, remaining = parser.parse_known_args()
    if '--output' not in remaining:
        raise SystemExit('An explicit --output directory is required')
    output = Path(remaining[remaining.index('--output') + 1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / 'results.json').exists():
        raise SystemExit('Output already contains results.json; choose a fresh directory')
    # Preserve the full candidate, including new headers not yet committed.
    patch = subprocess.check_output(['git', '-C', str(ROOT), 'diff', '--binary', 'HEAD', '--', '.'])
    (output / 'lhat.patch').write_bytes(patch)
    untracked = subprocess.check_output([
        'git', '-C', str(ROOT), 'ls-files', '-z', '--others', '--exclude-standard'
    ]).decode('utf-8').split('\0')
    snapshot = {}
    for name in untracked:
        if not name:
            continue
        source = ROOT / name
        # Snapshot candidate sources, rather than unrelated personal files.
        if (Path(name).parts[0] not in ('src', 'include', 'jit', 'tests', 'bench') or
                source.suffix.lower() not in ('.c', '.h', '.cpp', '.hpp', '.py',
                                               '.lh', '.lua', '.luau', '.ps1', '.s') or
                not source.resolve().is_relative_to(ROOT) or
                source.resolve().is_relative_to(output)):
            continue
        target = output / 'new-files' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        snapshot[name] = hashlib.sha256(source.read_bytes()).hexdigest()
    (output / 'new-files-sha256.json').write_text(json.dumps(snapshot, indent=2))
    from windows_controls import configure
    configure(own.affinity_mask)
    import run
    from report import render
    sys.argv = [str(HERE / 'run.py'), *remaining]
    status = run.main()
    result = json.loads((output / 'results.json').read_text())
    measured = [r for case in result['cases'] for r in case['runs']]
    scheduling = {
        'affinity_mask_requested': None if own.affinity_mask is None else hex(own.affinity_mask),
        'runner_priority': 'ABOVE_NORMAL_PRIORITY_CLASS',
        'power_throttling': 'execution-speed throttling opted out for each runner',
        'observed_affinity_masks': sorted({r.get('process_affinity_mask', 'unavailable') for r in measured}),
        'observed_priority_classes': sorted({r.get('priority_class', 0) for r in measured}),
        'power_opt_out_succeeded_for_all_measured': all(r.get('power_throttling_opt_out') for r in measured),
        'process_cpu_times': 'diagnostic whole-process times include frontend/startup; samples_ns remain the primary wall-clock metric',
        'driver_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'controls_sha256': hashlib.sha256((HERE / 'windows_controls.py').read_bytes()).hexdigest(),
    }
    result['metadata']['scheduling'] = scheduling
    (output / 'results.json').write_text(json.dumps(result, indent=2))
    if measured:
        note = '\nScheduling: ' + json.dumps(scheduling) + '\n'
        (output / 'report.md').write_text(render(result) + note, encoding='utf-8')
    for name in ('run_controlled.py', 'windows_controls.py'):
        shutil.copyfile(HERE / name, output / name)
    if measured:
        if scheduling['observed_priority_classes'] != [subprocess.ABOVE_NORMAL_PRIORITY_CLASS]:
            raise RuntimeError('Runner priority controls were not applied consistently')
        if not scheduling['power_opt_out_succeeded_for_all_measured']:
            raise RuntimeError('Runner power controls were not applied consistently')
        if own.affinity_mask is not None and scheduling['observed_affinity_masks'] != [hex(own.affinity_mask)]:
            raise RuntimeError('Runner affinity controls were not applied consistently')
    return status

if __name__ == '__main__':
    raise SystemExit(main())
