#!/usr/bin/env python3
"""Run independent sequential process samples; preserve raw samples and failures.

The executable supplies score time excluding only matrix I/O; process wall time is
recorded separately. No ranking/score is inferred from local measurements.
"""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import statistics
import struct
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


# Only application controls are isolated: OS affinity, cgroups and rlimits remain
# inherited constraints. Explicit CLI controls are applied after this filtering.
CONTROL_PREFIXES = ('DAG_', 'COMPILER2026_')
HELPER_CONTROLS = {'SPD_GENERATOR_THREADS', 'VERIFIER_THREADS'}
RESOURCE_OPTIONS = {
    'memory_mb': 'DAG_MEMORY_MB', 'graph_memory_mb': 'DAG_GRAPH_MEMORY_MB',
    'max_tasks': 'DAG_MAX_TASKS', 'max_steps': 'DAG_MAX_STEPS',
    'check_memory_mb': 'DAG_CHECK_MEMORY_MB', 'check_threads': 'DAG_CHECK_THREADS',
    'block_size': 'DAG_BLOCK_SIZE', 'block_sizes': 'DAG_BLOCK_SIZES',
}


def is_control(key):
    return key.startswith(CONTROL_PREFIXES) or key in HELPER_CONTROLS


def add_controls(parser, default_mode='auto', public_only=False):
    parser.add_argument('--clean-env', dest='clean_env', action='store_true', default=True,
                        help='default: remove inherited DAG/COMPILER2026 and helper controls')
    parser.add_argument('--inherit-env', dest='clean_env', action='store_false',
                        help='explicitly retain inherited application controls')
    parser.add_argument('--threads', type=int)
    parser.add_argument('--mode', choices=['auto', 'parallel', 'serial'])
    parser.add_argument('--batch', type=int)
    parser.add_argument('--backend', choices=['atomic', 'owner'])
    if not public_only:
        parser.add_argument('--io-format', choices=['public', 'original'])
        parser.add_argument('--application', choices=['blocked', 'scalar'])
    for name in RESOURCE_OPTIONS:
        parser.add_argument('--' + name.replace('_', '-'), type=str if name == 'block_sizes' else int)
    parser.set_defaults(default_mode=default_mode, public_only=public_only)


def execution_environment(args, parser):
    original = os.environ.copy()
    controls = {k: v for k, v in original.items() if is_control(k)}
    env = {k: v for k, v in original.items() if not (args.clean_env and is_control(k))}
    inherited = {} if args.clean_env else controls.copy()
    explicit, defaults = {}, {}
    options = [('threads', 'DAG_THREADS', '0'), ('mode', 'DAG_MODE', args.default_mode),
               ('batch', 'DAG_BATCH', '1'), ('backend', 'DAG_BACKEND', 'atomic'),
               ('io_format', 'DAG_IO_FORMAT', 'public'),
               ('application', 'DAG_APPLICATION', 'blocked')]
    for attr, key, fallback in options:
        value = getattr(args, attr, None)
        if args.public_only and attr in ('io_format', 'application'):
            value = fallback
        if value is not None:
            env[key] = str(value)
            explicit[key] = str(value)
        elif key not in env:
            env[key] = fallback
            defaults[key] = fallback
        setattr(args, attr, int(env[key]) if attr in ('threads', 'batch') else env[key])
    for attr, key in RESOURCE_OPTIONS.items():
        value = getattr(args, attr)
        if value is not None:
            if attr != 'block_sizes' and value < 0:
                parser.error('--' + attr.replace('_', '-') + ' must be nonnegative')
            env[key] = str(value)
            explicit[key] = str(value)
    if args.threads < 0 or args.batch < 1:
        parser.error('threads must be nonnegative and batch must be positive')
    if args.mode not in ('auto', 'parallel', 'serial') or args.backend not in ('atomic', 'owner'):
        parser.error('invalid inherited mode/backend')
    if args.io_format not in ('public', 'original') or args.application not in ('blocked', 'scalar'):
        parser.error('invalid inherited protocol/application')
    # Required harness paths are always replaced later; preserve the discarded
    # inherited values in provenance rather than using them for the child process.
    for key in ('DAG_METADATA', 'DAG_STATS_FILE', 'COMPILER2026_TIMING_FILE'):
        env.pop(key, None)
    policy = {'clean': args.clean_env, 'removed': controls if args.clean_env else {},
              'inherited': inherited, 'explicit': explicit, 'defaults': defaults,
              'effective_controls': {k: v for k, v in env.items() if is_control(k)}}
    return env, policy


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def environment_identity(env):
    # Runtime worker counts and algorithm knobs may differ across candidates;
    # machine/allocation and externally imposed resource budgets may not.
    cpu_model = ''
    try:
        for line in Path('/proc/cpuinfo').read_text().splitlines():
            if line.startswith(('model name', 'Hardware', 'CPU implementer')):
                cpu_model = line.split(':', 1)[-1].strip()
                break
    except OSError:
        pass
    import resource
    allocation = {'platform': platform.platform(), 'hostname': platform.node(),
                  'cpu_model': cpu_model, 'affinity': sorted(os.sched_getaffinity(0)),
                  'rlimit_as': list(resource.getrlimit(resource.RLIMIT_AS)),
                  'rlimit_data': list(resource.getrlimit(resource.RLIMIT_DATA)),
                  'budgets': {k: env.get(k) for k in (
                      'DAG_MEMORY_MB', 'DAG_GRAPH_MEMORY_MB', 'DAG_MAX_TASKS', 'DAG_MAX_STEPS')}}
    for key in ('DAG_GRAPH_MEMORY_MB', 'DAG_MAX_TASKS', 'DAG_MAX_STEPS'):
        allocation['budgets'][key] = str(int(env.get(key, '0')))
    for name in ('/proc/self/cgroup', '/sys/fs/cgroup/memory.max',
                 '/sys/fs/cgroup/cpuset.cpus.effective', '/sys/fs/cgroup/cpuset.mems.effective'):
        try:
            allocation[name] = Path(name).read_text().strip()
        except OSError:
            allocation[name] = None
    return allocation


def kernel_identity(program, explicit=None):
    library = explicit
    cache = program.resolve().parent / 'CMakeCache.txt'
    if library is None and cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith('DAG_OFFICIAL_LIBRARY:FILEPATH='):
                name = line.split('=', 1)[1]
                if name:
                    library = Path(name)
    if library is None:
        library = program.resolve().parent / 'libcontest_kernels.a'
    return {'path': str(library.resolve()), 'sha256': digest(library)} if library.is_file() else None


def input_cases(path, protocol):
    """Read record headers only; data stays in the supplied input file."""
    size = path.stat().st_size
    cases = []
    with path.open('rb') as stream:
        raw = stream.read(4)
        if len(raw) != 4:
            raise ValueError('truncated input case count')
        count, = struct.unpack('=I', raw)
        for index in range(count):
            raw = stream.read(8 if protocol == 'public' else 4)
            if len(raw) != (8 if protocol == 'public' else 4):
                raise ValueError('truncated input record header')
            dims = struct.unpack('=II' if protocol == 'public' else '=I', raw)
            n = dims[0]
            cases.append({'index': index, 'n': n, 'b': dims[1] if protocol == 'public' else None})
            target = stream.tell() + 8 * n * n
            if target > size:
                raise ValueError('truncated input matrix')
            stream.seek(target)
        if stream.tell() != size:
            raise ValueError('trailing input bytes')
    return cases


def explicit_resource_failures(stderr):
    failures = []
    for line in stderr.splitlines():
        if not line.startswith('DAG_RESOURCE_LIMIT '):
            continue
        value = json.loads(line[len('DAG_RESOURCE_LIMIT '):])
        if not isinstance(value, dict) or not all(k in value for k in ('index', 'n', 'b', 'stage', 'reason')):
            raise ValueError('invalid explicit resource failure record')
        if not isinstance(value['reason'], str) or not value['reason'] or not isinstance(value['stage'], str):
            raise ValueError('resource failure requires stage and reason')
        if value['index'] is not None and (type(value['index']) is not int or value['index'] < 0):
            raise ValueError('invalid resource failure case index')
        failures.append(value)
    return failures


def classify_sample(sample, cases):
    """Failed/unchecked runs never contribute valid cases, even with a marker."""
    sample['resource_failures'] = explicit_resource_failures(sample.get('stderr', ''))
    for failure in sample['resource_failures']:
        index = failure['index']
        if index is not None and (index >= len(cases) or failure['n'] != cases[index]['n'] or
                                 (cases[index]['b'] is not None and failure['b'] != cases[index]['b'])):
            raise ValueError('resource failure record does not match input case')
    sample['resource_exclusions'] = []
    sample['valid_case_indices'] = []
    good = (sample.get('returncode') == 0 and not sample.get('error') and not sample['resource_failures'] and
            sample.get('validation', {}).get('returncode') == 0)
    stats = sample.get('runtime_stats', {})
    runtime_cases = stats.get('cases')
    if good or runtime_cases is not None:
        if not isinstance(runtime_cases, list) or len(runtime_cases) != len(cases):
            raise ValueError('missing or inconsistent runtime case statistics')
        for expected, actual in zip(cases, runtime_cases):
            if (actual.get('index') != expected['index'] or actual.get('n') != expected['n'] or
                    (expected['b'] is not None and actual.get('b') != expected['b'])):
                raise ValueError('runtime case statistics do not match input')
            # Older reports lack explicit status and cannot prove unrestricted
            # execution. Never infer resource exhaustion from a return code.
            if type(actual.get('resource_limited')) is not bool:
                raise ValueError('runtime case missing explicit resource_limited status')
            seconds = actual.get('compute_seconds')
            if not isinstance(seconds, (int, float)) or not (0 <= seconds < float('inf')):
                raise ValueError('invalid per-case compute time')
            if actual['resource_limited']:
                if not actual.get('resource_reason') or not actual.get('resource_stage'):
                    raise ValueError('resource-limited case lacks reason/stage')
                sample['resource_exclusions'].append({
                    **expected, 'reason': actual['resource_reason'], 'stage': actual['resource_stage'],
                    'effective_graph_budget_bytes': actual.get('effective_graph_budget_bytes'),
                    'compute_seconds': seconds})
            elif good:
                sample['valid_case_indices'].append(expected['index'])
    sample['correctness_passed'] = good
    sample['resource_limited'] = bool(sample['resource_exclusions'] or sample['resource_failures'])
    sample['full_batch_eligible'] = good and not sample['resource_limited']
    sample['failure_class'] = ('none' if good else
                               'explicit_resource_failure' if sample['resource_failures'] else 'error')


def diagnostic_scope(report, indices):
    case_rows = [c for c in report['input_cases'] if c['index'] in indices]
    identity = {'input_sha256': report['input_sha256'], 'protocol': report['protocol'],
                'application': report['application'], 'environment_fingerprint': report['environment_fingerprint'],
                'kernel_sha256': (report.get('kernel_library') or {}).get('sha256'),
                'cases': case_rows}
    return identity, fingerprint(identity)


def common_diagnostic(report, indices=None):
    samples = [s for s in report['samples'] if not s.get('warmup')]
    available = set(c['index'] for c in report['input_cases'])
    for sample in samples:
        available.intersection_update(sample.get('valid_case_indices', []))
    if not samples:
        available.clear()
    if indices is not None:
        available.intersection_update(indices)
    scope, scope_hash = diagnostic_scope(report, available)
    total = len(report['input_cases'])
    result = {'kind': 'common_valid_case_compute_sum_diagnostic',
              'not_full_batch_score': True, 'scope': scope, 'scope_fingerprint': scope_hash,
              'case_indices': sorted(available), 'case_count': len(available),
              'total_case_count': total, 'coverage': len(available) / total if total else 0,
              'excluded_case_indices': sorted(set(range(total)) - available),
              'samples': [], 'resource_exclusions': [
                  {'sample': s['index'], 'cases': s.get('resource_exclusions', []),
                   'failures': s.get('resource_failures', [])} for s in samples
                  if s.get('resource_limited')],
              'invalid_samples': [s['index'] for s in samples if not s.get('correctness_passed')]}
    if available:
        for sample in samples:
            seconds = sum(c['compute_seconds'] for c in sample['runtime_stats']['cases']
                          if c['index'] in available)
            result['samples'].append({'index': sample['index'], 'case_compute_sum_seconds': seconds})
        result['median_case_compute_sum_seconds'] = statistics.median(
            x['case_compute_sum_seconds'] for x in result['samples'])
    return result


def summarize(report):
    measured = [s for s in report['samples'] if not s.get('warmup')]
    times = [s['score_seconds'] for s in measured if s.get('full_batch_eligible')]
    report['summary'] = {'metadata_bytes': report['metadata_bytes'], 'successful_samples': len(times),
                         'full_batch_samples': len(times), 'measured_samples': len(measured),
                         'resource_limited_samples': sum(s.get('resource_limited', False) for s in measured),
                         'complete_performance': bool(measured) and len(times) == len(measured)}
    if times:
        report['summary'].update(median_seconds=statistics.median(times), min_seconds=min(times),
                                 max_seconds=max(times))
    report['common_case_diagnostic'] = common_diagnostic(report)
    report['correctness_passed'] = bool(report['samples']) and all(
        s.get('correctness_passed') for s in report['samples'])
    report['passed'] = report['correctness_passed'] and all(
        s.get('full_batch_eligible') for s in report['samples'])


def compare_reports(argv):
    parser = argparse.ArgumentParser(description='Compare only identical-input/environment common valid case scopes.')
    parser.add_argument('reports', type=Path, nargs='+')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args(argv)
    if len(args.reports) < 2:
        parser.error('at least two reports required')
    reports = [json.loads(p.read_text()) for p in args.reports]
    first = reports[0]
    for report in reports:
        if not report.get('environment_fingerprint') or not report.get('input_cases'):
            parser.error('reports must contain current environment and input-case identities')
        for key in ('input_sha256', 'protocol', 'application', 'environment_fingerprint', 'input_cases'):
            if report[key] != first[key]:
                parser.error('incompatible comparison scope: ' + key)
        if report.get('checker_sha256') != first.get('checker_sha256'):
            parser.error('different checker identities')
        kernel_hash = (report.get('kernel_library') or {}).get('sha256')
        if not kernel_hash or kernel_hash != (first.get('kernel_library') or {}).get('sha256'):
            parser.error('unknown or different kernel library identity; use --kernel-library when measuring')
        kernels = {s.get('runtime_stats', {}).get('kernel_mode') for s in report['samples']}
        first_kernels = {s.get('runtime_stats', {}).get('kernel_mode') for s in first['samples']}
        if kernels != first_kernels or None in kernels:
            parser.error('different or unknown kernel modes')
    indices = set(range(len(first['input_cases'])))
    for report in reports:
        indices.intersection_update(common_diagnostic(report)['case_indices'])
    scope, scope_hash = diagnostic_scope(first, indices)
    result = {'scope': scope, 'scope_fingerprint': scope_hash, 'not_full_batch_score': True,
              'case_indices': sorted(indices), 'case_count': len(indices),
              'coverage': len(indices) / len(first['input_cases']), 'candidates': []}
    for path, report in zip(args.reports, reports):
        diag = common_diagnostic(report, indices)
        result['candidates'].append({'report': str(path.resolve()), 'program_sha256': report['program_sha256'],
                                     'metadata_bytes': report['metadata_bytes'],
                                     'configuration': report['configuration'], 'diagnostic': diag})
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    return 0


def submit(argv):
    parser = argparse.ArgumentParser(description='Clean deployment launcher; child receives exactly input/output paths.')
    parser.add_argument('program', type=Path)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--configuration-report', type=Path)
    add_controls(parser)
    args = parser.parse_args(argv)
    env, policy = execution_environment(args, parser)
    env['DAG_METADATA'] = str(args.metadata.resolve())
    policy['explicit']['DAG_METADATA'] = env['DAG_METADATA']
    policy['effective_controls'] = {k: v for k, v in env.items() if is_control(k)}
    if args.configuration_report:
        args.configuration_report.parent.mkdir(parents=True, exist_ok=True)
        args.configuration_report.write_text(json.dumps(policy, indent=2) + '\n')
    return subprocess.run([str(args.program.resolve()), str(args.input.resolve()),
                           str(args.output.resolve())], env=env).returncode


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
    add_controls(p, public_only=True)
    p.add_argument('--jobs', type=int, default=8)
    p.add_argument('--timeout', type=float, default=600)
    a = p.parse_args(argv)
    env, env_policy = execution_environment(a, p)
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
                                'batch': a.batch, 'backend': a.backend},
              'environment_policy': env_policy}
    env.update(DAG_IO_FORMAT='public', DAG_APPLICATION='blocked', DAG_MODE=a.mode,
               DAG_BATCH=str(a.batch), DAG_BACKEND=a.backend,
               DAG_THREADS=str(a.threads), DAG_METADATA=str(build / 'block_cholesky.meta'),
               DAG_STATS_FILE=str(result_dir / 'runtime.json'),
               COMPILER2026_TIMING_FILE=str(result_dir / 'time.txt'))
    if any(k in env for k in ('DAG_BLOCK_SIZE', 'DAG_BLOCK_SIZES')):
        p.error('public protocol does not accept block-size overrides')
    helper_threads = str(min(8, len(os.sched_getaffinity(0))))
    for key in ('SPD_GENERATOR_THREADS', 'VERIFIER_THREADS', 'DAG_CHECK_THREADS'):
        env.setdefault(key, helper_threads)
    env_policy['explicit']['DAG_METADATA'] = env['DAG_METADATA']
    env_policy['effective_controls'] = {k: v for k, v in env.items() if is_control(k)}
    report['requirements'] += ['R-25', 'R-26']

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
        if name == 'execute':
            report['stages'][name]['resource_failures'] = explicit_resource_failures(run.stderr)
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
        report['kernel_library'] = kernel_identity(build / 'dag_run', a.official_library)
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
        sample = {'index': 0, 'warmup': False, 'returncode': 0, 'stderr': '',
                  'score_seconds': score, 'runtime_stats': report['runtime'],
                  'validation': {'returncode': 0}}
        cases = [{'index': i, 'n': n, 'b': b} for i, (n, b) in enumerate(expected)]
        classify_sample(sample, cases)
        report['resource_exclusions'] = sample['resource_exclusions']
        report['complete_performance'] = sample['full_batch_eligible']
        if sample['full_batch_eligible']:
            report['summary']['score_seconds'] = score
        else:
            report['summary']['complete_performance'] = False
        report['sha256'] = {str(path): digest(path) for path in (
            spec, build / 'dag_run', build / 'block_cholesky.meta',
            build / 'spd_generator', build / 'public_verifier', build / 'dag_check', inp, out)}
        report['metadata_bytes'] = (build / 'block_cholesky.meta').stat().st_size
        report['passed'] = sample['full_batch_eligible']
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
    elif report.get('public_acceptance_passed') and report.get('additional_checks_passed'):
        print('PUBLIC20 correctness: PASS; complete performance: INELIGIBLE (resource-limited cases)', flush=True)
    return int(not report['passed'])


def main():
    commands = {'accept-public20': accept_public20, 'compare-reports': compare_reports, 'submit': submit}
    if len(sys.argv) > 1 and sys.argv[1] in commands:
        return commands[sys.argv[1]](sys.argv[2:])
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('program', type=Path)
    p.add_argument('input', type=Path)
    p.add_argument('--metadata', type=Path, required=True)
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--warmups', type=int, default=1)
    add_controls(p, default_mode='parallel')
    p.add_argument('--checker', type=Path, required=True)
    p.add_argument('--kernel-library', type=Path,
                   help='library identity for cross-report comparisons; inferred from adjacent CMake build when possible')
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=600)
    a = p.parse_args()
    env_base, env_policy = execution_environment(a, p)
    env_base['DAG_METADATA'] = str(a.metadata.resolve())
    env_policy['explicit']['DAG_METADATA'] = env_base['DAG_METADATA']
    env_policy['effective_controls'] = {k: v for k, v in env_base.items() if is_control(k)}
    if a.repeats < 1 or a.warmups < 0 or a.threads < 0 or a.batch < 1 or not (0 < a.timeout < float('inf')):
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
              'environment': env_policy['effective_controls'],
              'environment_policy': env_policy,
              'validation_scope': 'Selected checker. Use accept-public20 for the supplied verifier workflow.',
              'configuration': {'threads': a.threads, 'batch': a.batch, 'mode': a.mode,
                                'backend': a.backend},
              'scope': 'Each sample is a fresh process; warmups affect OS caches, not reusable process resources.',
              'summary': {'metadata_bytes': a.metadata.stat().st_size},
              'samples': []}
    report['input_cases'] = input_cases(a.input, a.io_format)
    if a.io_format == 'original':
        if a.application == 'scalar':
            blocks = [1] * len(report['input_cases'])
        else:
            shared = env_base.get('DAG_BLOCK_SIZE', '')
            per_case = env_base.get('DAG_BLOCK_SIZES', '')
            if bool(shared) == bool(per_case):
                p.error('original blocked input requires exactly one block-size setting')
            try:
                blocks = ([int(shared)] * len(report['input_cases']) if shared else
                          [int(value) for value in per_case.split(',')])
            except ValueError:
                p.error('invalid original-format block size')
            if len(blocks) != len(report['input_cases']):
                p.error('original-format block-size count mismatch')
        for case, block in zip(report['input_cases'], blocks):
            if block <= 0 or block > case['n'] or case['n'] % block:
                p.error('invalid original-format block size')
            case['b'] = block

    report['kernel_library'] = kernel_identity(a.program, a.kernel_library)
    report['environment_identity'] = environment_identity(env_base)
    report['environment_fingerprint'] = fingerprint(report['environment_identity'])
    report['requirements'] += ['R-25', 'R-26']
    failed = False
    with tempfile.TemporaryDirectory(prefix='dag-benchmark-') as d:
        d = Path(d)
        for i in range(a.warmups + a.repeats):
            timing, stats, out = d / 'time.txt', d / 'stats.json', d / 'output.bin'
            for path in [timing, stats, out]:
                path.unlink(missing_ok=True)
            env = env_base.copy()
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
                # Keep emitted evidence even on failure, but eligibility still
                # requires successful execution and independent validation.
                if timing.exists():
                    score = float(timing.read_text().strip())
                    if not (0 <= score < float('inf')):
                        raise ValueError('invalid score time')
                    sample['score_seconds'] = score
                if stats.exists():
                    sample['runtime_stats'] = json.loads(stats.read_text())
                if run.returncode:
                    failed = True
                elif not timing.exists():
                    sample['error'] = 'program did not emit timing file'
                    failed = True
                else:
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
            try:
                classify_sample(sample, report['input_cases'])
            except (ValueError, TypeError, KeyError) as error:
                sample['error'] = str(error)
                sample['correctness_passed'] = False
                sample['full_batch_eligible'] = False
                sample['valid_case_indices'] = []
                failed = True
            report['samples'].append(sample)
            # Persist after every sample, including failed or interrupted runs.
            a.report.parent.mkdir(parents=True, exist_ok=True)
            a.report.write_text(json.dumps(report, indent=2) + '\n')
    summarize(report)
    report['passed'] = report['passed'] and not failed
    a.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], **report['summary']}))
    return int(not report['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
