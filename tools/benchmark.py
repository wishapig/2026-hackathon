#!/usr/bin/env python3
"""Run independent sequential process samples; preserve raw samples and failures.

The executable supplies score time excluding only matrix I/O; process wall time is
recorded separately. No ranking/score is inferred from local measurements.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import time


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def public_rows(stdout, expected):
    """Apply the supplied verifier's per-case acceptance contract."""
    rows = []
    pattern = re.compile(r'case=(\d+) n=(\d+) b=(\d+) scaled_residual=(\S+) status=(PASS|FAIL)')
    for line in stdout.splitlines():
        match = pattern.fullmatch(line.strip())
        if not match:
            raise ValueError('unexpected verifier output: ' + line)
        index, n, b, residual, status = match.groups()
        rows.append({'index': int(index), 'n': int(n), 'b': int(b),
                     'scaled_residual': residual, 'status': status})
    if len(rows) != 20 or len(expected) != 20:
        raise ValueError('acceptance requires exactly 20 verifier case records')
    for index, (row, shape) in enumerate(zip(rows, expected)):
        if row['index'] != index or (row['n'], row['b']) != shape or row['status'] != 'PASS':
            raise ValueError('verifier did not report matching PASS for case ' + str(index))
    return rows


def accept_public20(argv):
    """One end-to-end entry: build, generate, execute, supplied verifier."""
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=accept_public20.__doc__)
    p.add_argument('--build-dir', type=Path, default=root / 'build')
    p.add_argument('--output-dir', type=Path, default=root / 'artifacts/acceptance')
    p.add_argument('--official-library', type=Path)
    p.add_argument('--threads', type=int, default=0,
                   help='0 uses the runtime-selected allowed single-NUMA CPU set')
    p.add_argument('--mode', choices=['auto', 'parallel', 'serial'], default='auto')
    p.add_argument('--batch', type=int, default=1)
    p.add_argument('--backend', choices=['atomic', 'owner'],
                   default=os.environ.get('DAG_BACKEND', 'atomic'))
    p.add_argument('--jobs', type=int, default=8)
    p.add_argument('--timeout', type=float, default=600)
    a = p.parse_args(argv)
    if a.threads < 0 or a.batch < 1 or a.jobs < 1 or not (0 < a.timeout < float('inf')):
        p.error('invalid threads/jobs/timeout')
    build = a.build_dir.resolve()
    a.output_dir.mkdir(parents=True, exist_ok=True)
    result_dir = Path(tempfile.mkdtemp(prefix='public20-', dir=a.output_dir.resolve()))
    report_path = result_dir / 'acceptance.json'
    report = {'passed': False, 'public_acceptance_passed': False,
              'scope': 'Initial local public20 validation; not the complete hidden evaluation.',
              'requirements': ['R-12', 'R-13', 'R-14', 'R-15', 'R-18', 'R-19', 'R-21', 'R-22', 'R-24'],
              'build_dir': str(build), 'result_dir': str(result_dir), 'stages': {},
              'configuration': {'threads': a.threads, 'mode': a.mode,
                                'batch': a.batch, 'backend': a.backend}}
    env = os.environ.copy()
    env.update(DAG_IO_FORMAT='public', DAG_APPLICATION='blocked', DAG_MODE=a.mode,
               DAG_BATCH=str(a.batch), DAG_BACKEND=a.backend,
               DAG_THREADS=str(a.threads), DAG_METADATA=str(build / 'block_cholesky.meta'),
               DAG_STATS_FILE=str(result_dir / 'runtime.json'),
               COMPILER2026_TIMING_FILE=str(result_dir / 'time.txt'))
    for key in ('DAG_BLOCK_SIZE', 'DAG_BLOCK_SIZES'):
        env.pop(key, None)
    helper_threads = str(min(8, len(os.sched_getaffinity(0))))
    for key in ('SPD_GENERATOR_THREADS', 'VERIFIER_THREADS', 'DAG_CHECK_THREADS'):
        env.setdefault(key, helper_threads)

    def run_stage(name, command, show=False):
        print('[' + name + ']', flush=True)
        started = time.perf_counter()
        run = subprocess.run([str(x) for x in command], cwd=root, env=env,
                             capture_output=True, text=True, timeout=a.timeout)
        log = result_dir / (name + '.log')
        log.write_text(run.stdout + run.stderr)
        report['stages'][name] = {'command': [str(x) for x in command],
                                 'returncode': run.returncode,
                                 'wall_seconds': time.perf_counter() - started,
                                 'log': str(log)}
        if show or run.returncode:
            print(run.stdout + run.stderr, end='', flush=True)
        if run.returncode:
            raise RuntimeError(name + ' exited with status ' + str(run.returncode))
        return run

    try:
        spec = root / 'vendor/public20/cases/preliminary_public_20.txt'
        expected = []
        for line in spec.read_text().splitlines():
            line = line.split('#', 1)[0].strip()
            if line:
                fields = line.replace(':', ' ').split()
                expected.append((int(fields[0]), int(fields[1])))
        if len(expected) != 20:
            raise ValueError('provided public specification must contain 20 cases')
        configure = ['cmake', '-S', root, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
                     '-DDAG_SANITIZE=OFF', '-DDAG_TSAN=OFF']
        if a.official_library:
            configure += ['-DDAG_REQUIRE_OFFICIAL=ON',
                          '-DDAG_OFFICIAL_LIBRARY=' + str(a.official_library.resolve())]
        run_stage('configure', configure)
        run_stage('build', ['cmake', '--build', build, '--parallel', str(a.jobs), '--target',
                            'dag_run', 'spd_generator', 'public_verifier', 'dag_check'])
        metadata = build / 'block_cholesky.meta'
        report['metadata'] = str(metadata)
        report['metadata_sha256'] = digest(metadata)
        report['metadata_bytes'] = metadata.stat().st_size
        report['summary'] = {'metadata_bytes': report['metadata_bytes']}
        inp, out = result_dir / 'public20.bin', result_dir / 'public20.out'
        run_stage('generate', [build / 'spd_generator', inp, '--spec', spec])
        # Exactly the two positional arguments required by the supplied workflow.
        run_stage('execute', [build / 'dag_run', inp, out], show=True)
        verified = run_stage('public_verifier', [build / 'public_verifier', inp, out], show=True)
        report['cases'] = public_rows(verified.stdout, expected)
        report['public_acceptance_passed'] = True
        # Retain the user's finite-value/positive-diagonal checks as a separate
        # guard, never as a replacement for the provided verifier.
        strict = run_stage('additional_checks', [build / 'dag_check', inp, out])
        public_rows(strict.stdout, expected)
        report['additional_checks_passed'] = True
        report['runtime'] = json.loads((result_dir / 'runtime.json').read_text())
        score = float((result_dir / 'time.txt').read_text())
        if not (0 <= score < float('inf')):
            raise ValueError('invalid runtime timing')
        report['score_seconds'] = score
        report['summary']['score_seconds'] = score
        report['sha256'] = {str(path): digest(path) for path in (
            spec, build / 'dag_run', build / 'block_cholesky.meta',
            build / 'spd_generator', build / 'public_verifier', build / 'dag_check', inp, out)}
        report['metadata_bytes'] = (build / 'block_cholesky.meta').stat().st_size
        report['passed'] = True
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
        print('PUBLIC20 ACCEPTANCE: FAIL — ' + str(error), file=sys.stderr, flush=True)
    finally:
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        print('Report: ' + str(report_path), flush=True)
        print(json.dumps(report.get('summary', {'passed': False})), flush=True)
    if report['passed']:
        print('PUBLIC20 ACCEPTANCE: PASS (20/20; supplied verifier exit=0)', flush=True)
        print('Additional checks: PASS; compute seconds=' + str(report['score_seconds']), flush=True)
        print('Kernel mode: ' + report['runtime']['kernel_mode'] +
              '; allowed single-NUMA CPUs=' + str(report['runtime']['cpus']), flush=True)
    return int(not report['passed'])


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'accept-public20':
        return accept_public20(sys.argv[2:])
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('program', type=Path)
    p.add_argument('input', type=Path)
    p.add_argument('--metadata', type=Path, required=True)
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--warmups', type=int, default=1)
    p.add_argument('--threads', type=int, default=0)
    p.add_argument('--batch', type=int, default=1)
    p.add_argument('--mode', choices=['auto', 'parallel', 'serial'], default='parallel')
    p.add_argument('--backend', choices=['atomic', 'owner'],
                   default=os.environ.get('DAG_BACKEND', 'atomic'))
    p.add_argument('--checker', type=Path, required=True)
    p.add_argument('--io-format', choices=['public', 'original'],
                   default=os.environ.get('DAG_IO_FORMAT', 'public'))
    p.add_argument('--application', choices=['blocked', 'scalar'],
                   default=os.environ.get('DAG_APPLICATION', 'blocked'))
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=600)
    a = p.parse_args()
    if a.repeats < 1 or a.warmups < 0 or a.threads < 0 or a.batch < 1 or a.timeout <= 0:
        p.error('invalid positive measurement parameters')
    report = {'program': str(a.program.resolve()), 'program_sha256': digest(a.program),
              'input': str(a.input.resolve()), 'input_sha256': digest(a.input),
              'metadata': str(a.metadata.resolve()),
              'metadata_sha256': digest(a.metadata),
              'metadata_bytes': a.metadata.stat().st_size,
              'checker': str(a.checker.resolve()), 'checker_sha256': digest(a.checker),
              'requirements': ['R-06', 'R-11' if a.io_format == 'original' else 'R-12',
                               'R-15', 'R-18', 'R-21', 'R-22', 'R-23', 'R-24'],
              'protocol': a.io_format,
              'application': a.application,
              'environment': {key: value for key, value in os.environ.items()
                              if key in ('DAG_MEMORY_MB', 'DAG_GRAPH_MEMORY_MB', 'DAG_MAX_TASKS',
                                         'DAG_MAX_STEPS', 'DAG_BACKEND', 'DAG_AUDIT',
                                         'DAG_BLOCK_SIZE', 'DAG_BLOCK_SIZES',
                                         'DAG_CHECK_MEMORY_MB', 'DAG_CHECK_THREADS')},
              'validation_scope': 'Selected checker. Use accept-public20 for the supplied verifier workflow.',
              'configuration': {'threads': a.threads, 'batch': a.batch, 'mode': a.mode,
                                'backend': a.backend},
              'scope': 'Each sample is a fresh process; warmups affect OS caches, not reusable process resources.',
              'summary': {'metadata_bytes': a.metadata.stat().st_size},
              'samples': []}
    failed = False
    with tempfile.TemporaryDirectory(prefix='dag-benchmark-') as d:
        d = Path(d)
        for i in range(a.warmups + a.repeats):
            timing, stats, out = d / 'time.txt', d / 'stats.json', d / 'output.bin'
            for path in [timing, stats, out]:
                path.unlink(missing_ok=True)
            env = os.environ.copy()
            env.update(DAG_THREADS=str(a.threads), DAG_BATCH=str(a.batch), DAG_MODE=a.mode,
                       DAG_BACKEND=a.backend,
                       DAG_IO_FORMAT=a.io_format, DAG_APPLICATION=a.application,
                       COMPILER2026_TIMING_FILE=str(timing), DAG_STATS_FILE=str(stats))
            if a.metadata:
                env['DAG_METADATA'] = str(a.metadata.resolve())
            start = time.perf_counter()
            sample = {'index': i, 'warmup': i < a.warmups}
            try:
                run = subprocess.run([str(a.program.resolve()), str(a.input.resolve()), str(out)],
                                     env=env, capture_output=True, text=True, timeout=a.timeout)
                sample.update(returncode=run.returncode, stdout=run.stdout, stderr=run.stderr,
                              process_wall_seconds=time.perf_counter() - start)
                if run.returncode:
                    failed = True
                elif not timing.exists():
                    sample['error'] = 'program did not emit timing file'
                    failed = True
                else:
                    score = float(timing.read_text().strip())
                    if not (0 <= score < float('inf')):
                        raise ValueError('invalid score time')
                    sample['score_seconds'] = score
                    if stats.exists():
                        sample['runtime_stats'] = json.loads(stats.read_text())
                    if a.checker:
                        check = subprocess.run([str(a.checker.resolve()), str(a.input.resolve()), str(out)],
                                               env=env, capture_output=True, text=True, timeout=a.timeout)
                        sample['validation'] = {'returncode': check.returncode, 'stdout': check.stdout,
                                                'stderr': check.stderr}
                        failed |= check.returncode != 0
            except (subprocess.TimeoutExpired, ValueError, OSError) as error:
                sample['error'] = str(error)
                # A checker failure occurs after the program has finished. Keep
                # its wall time separate from validation time even on failure.
                sample.setdefault('process_wall_seconds', time.perf_counter() - start)
                failed = True
            report['samples'].append(sample)
            # Persist after every sample, including failed or interrupted runs.
            a.report.parent.mkdir(parents=True, exist_ok=True)
            a.report.write_text(json.dumps(report, indent=2) + '\n')
    times = [s['score_seconds'] for s in report['samples']
             if not s['warmup'] and 'score_seconds' in s and 'error' not in s
             and s.get('returncode') == 0
             and s.get('validation', {}).get('returncode') == 0]
    if times:
        report['summary'].update(successful_samples=len(times), median_seconds=statistics.median(times),
                                 min_seconds=min(times), max_seconds=max(times))
    report['passed'] = not failed
    a.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], **report['summary']}))
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
