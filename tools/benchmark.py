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
    parser.add_argument('--loop-chunk', type=int)
    parser.add_argument('--loop-parts', type=int)
    parser.add_argument('--schedule', choices=['auto', 'graph', 'pattern'])
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
    for attr, key in (('loop_chunk', 'DAG_LOOP_CHUNK'), ('loop_parts', 'DAG_LOOP_PARTS')):
        value = getattr(args, attr, None)
        if value is not None:
            if value < 0:
                parser.error('--' + attr.replace('_', '-') + ' must be nonnegative')
            env[key] = str(value)
            explicit[key] = str(value)
    schedule = getattr(args, 'schedule', None)
    if schedule is not None:
        env['DAG_SCHEDULE'] = schedule
        explicit['DAG_SCHEDULE'] = schedule
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


def cgroup_constraint_identity(membership_path=Path('/proc/self/cgroup'),
                               mountinfo_path=Path('/proc/self/mountinfo')):
    """Record stable limits at the process group and all visible ancestors.

    Resolve cgroup mount roots like resources.hpp, including namespace-root
    fallback. Usage/free counters are intentionally absent: they fluctuate
    without changing the imposed resource allocation.
    """
    def read(path):
        try:
            return Path(path).read_text().strip()
        except OSError:
            return None

    def unescape(value):
        return re.sub(r'\\([0-7]{3})', lambda m: chr(int(m.group(1), 8)), value)

    membership, mountinfo = read(membership_path), read(mountinfo_path)
    groups = {}
    for line in (membership or '').splitlines():
        fields = line.split(':', 2)
        if len(fields) != 3:
            continue
        _, controllers, path = fields
        for controller in controllers.split(',') if controllers else ['unified']:
            groups[controller] = path
    mounts = []
    for line in (mountinfo or '').splitlines():
        fields = line.split(' - ', 1)
        if len(fields) != 2:
            continue
        before, after = fields[0].split(), fields[1].split()
        if len(before) < 6 or len(after) < 3:
            continue
        kind, _, super_options = after[:3]
        controllers = set(super_options.split(','))
        if kind == 'cgroup2':
            selected = ['unified'] if 'unified' in groups else []
            names = ['memory.max', 'cpuset.cpus', 'cpuset.mems',
                     'cpuset.cpus.effective', 'cpuset.mems.effective']
        elif kind == 'cgroup':
            selected = [c for c in ('memory', 'cpuset') if c in controllers and c in groups]
            names = (['memory.limit_in_bytes'] if 'memory' in selected else []) + (
                ['cpuset.cpus', 'cpuset.mems', 'cpuset.effective_cpus',
                 'cpuset.effective_mems'] if 'cpuset' in selected else [])
        else:
            continue
        root, mount = Path(unescape(before[3])), Path(unescape(before[4]))
        for controller in selected:
            member = Path(groups[controller])
            relative = Path(os.path.relpath(member, root))
            safe = '..' not in relative.parts
            group = Path(os.path.normpath(mount / relative)) if safe else mount
            levels = []
            while True:
                levels.append({'path': str(group), 'limits': {name: read(group / name) for name in names}})
                if group == mount or group.parent == group:
                    break
                group = group.parent
            mounts.append({'type': kind, 'controller': controller, 'root': str(root),
                           'mount': str(mount), 'membership': str(member),
                           'namespace_root_fallback': not safe, 'ancestors': levels})
    mounts.sort(key=lambda x: (x['type'], x['controller'], x['mount'], x['membership']))
    return {'membership': membership, 'mountinfo_available': mountinfo is not None, 'mounts': mounts}


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
    allocation['cgroup_constraints'] = cgroup_constraint_identity()
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


CASE_CATEGORIES = ('valid', 'resource_limited', 'completed_unvalidated',
                   'resource_failed', 'not_run', 'error_or_unknown')


def classify_case_status(sample, cases):
    """Partition case status without inventing times or numerical validation."""
    status = {name: [] for name in CASE_CATEGORIES}
    all_indices = [case['index'] for case in cases]
    failures = sample.get('resource_failures', [])
    if len(failures) == 1 and sample.get('returncode') not in (None, 0):
        failure = failures[0]
        index, stage = failure['index'], failure['stage']
        if index is not None:
            # Runtime protocol is strictly sequential: reaching i proves that
            # earlier calls completed, not that their results were validated.
            status['completed_unvalidated'] = [i for i in all_indices if i < index]
            status['resource_failed'] = [index]
            status['not_run'] = [i for i in all_indices if i > index]
        elif stage in ('configuration', 'metadata'):
            status['not_run'] = all_indices
        elif stage == 'finalize':
            status['completed_unvalidated'] = all_indices
        else:
            # Unknown stage or indexless evidence cannot establish progress.
            status['error_or_unknown'] = all_indices
    elif sample.get('correctness_passed'):
        status['valid'] = list(sample.get('valid_case_indices', []))
        status['resource_limited'] = [c['index'] for c in sample.get('resource_exclusions', [])]
    elif (sample.get('returncode') == 0 and
          len(sample.get('runtime_stats', {}).get('cases', [])) == len(cases)):
        # The whole execution returned, but checker failure/timeout is still
        # an error. Resource causes remain separately recorded, not excused.
        status['completed_unvalidated'] = all_indices
    else:
        status['error_or_unknown'] = all_indices
    sample['case_categories'] = status
    sample['case_counts'] = {name: len(indices) for name, indices in status.items()}


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
    classify_case_status(sample, cases)


def diagnostic_scope(report, indices):
    case_rows = [c for c in report['input_cases'] if c['index'] in indices]
    identity = {'input_sha256': report['input_sha256'], 'protocol': report['protocol'],
                'application': report['application'], 'environment_fingerprint': report['environment_fingerprint'],
                'kernel_sha256': (report.get('kernel_library') or {}).get('sha256'),
                'cases': case_rows}
    return identity, fingerprint(identity)


def common_diagnostic(report, indices=None):
    if report.get('application') != 'blocked' or report.get('performance_eligible') is False:
        return {'kind': 'functional_only', 'performance_eligible': False, 'case_indices': [],
                'reason': 'Only blocked Cholesky participates in performance evaluation (R-27).'}
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
    result['case_status_by_sample'] = [
        {'sample': s['index'], 'case_categories': s.get('case_categories', {}),
         'case_counts': s.get('case_counts', {})} for s in samples]
    result['case_sample_counts'] = {name: sum(s.get('case_counts', {}).get(name, 0) for s in samples)
                                    for name in CASE_CATEGORIES}
    result['case_sample_counts_unit'] = 'case appearances across measured samples; not unique cases'
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
    report['performance_eligible'] = report.get('application') == 'blocked'
    report['evaluation_kind'] = 'performance' if report['performance_eligible'] else 'functional_only'
    if not report['performance_eligible']:
        report['correctness_passed'] = bool(report['samples']) and all(
            s.get('correctness_passed') for s in report['samples'])
        for sample in report['samples']:
            sample['full_batch_eligible'] = False
            sample['performance_eligible'] = False
        report['summary'] = {'metadata_bytes': report['metadata_bytes'],
                             'evaluation_kind': 'functional_only', 'performance_eligible': False,
                             'correctness_passed': report['correctness_passed'],
                             'measured_samples': len(measured)}
        report['passed'] = report['correctness_passed']
        return
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
    if any(r.get('application') != 'blocked' or r.get('performance_eligible') is False
           or r.get('kind') == 'functional_only' for r in reports):
        parser.error('functional-only algorithms cannot enter performance comparisons (R-27)')
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
        report['case_categories'] = sample['case_categories']
        report['case_counts'] = sample['case_counts']
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
        functional = functional_algorithms(build, result_dir / 'functional', env, a.timeout)
        report['functional_validation'] = {
            'passed': functional['passed'], 'performance_eligible': False,
            'report': str(result_dir / 'functional/functional-algorithms.json')}
        report['requirements'].append('R-27')
        report['passed'] = report['passed'] and functional['passed']
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
        if 'expected' in locals():
            execute = report['stages'].get('execute', {})
            sample = {'returncode': execute.get('returncode'),
                      'resource_failures': execute.get('resource_failures', [])}
            cases = [{'index': i, 'n': n, 'b': b} for i, (n, b) in enumerate(expected)]
            classify_case_status(sample, cases)
            report['case_categories'] = sample['case_categories']
            report['case_counts'] = sample['case_counts']
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
    elif report.get('functional_validation', {}).get('passed') is False:
        print('Functional algorithm validation: FAIL; see functional report.', flush=True)
    elif report.get('public_acceptance_passed') and report.get('additional_checks_passed'):
        print('PUBLIC20 correctness: PASS; complete performance: INELIGIBLE (resource-limited cases)', flush=True)
    return int(not report['passed'])


def verify_algorithm_outputs(path, expected):
    """Independent known-factor / rational-recurrence oracle, R-15/R-27."""
    import math
    with path.open('rb') as f:
        def exact(size):
            data = f.read(size)
            if len(data) != size:
                raise ValueError('truncated functional output')
            return data
        if struct.unpack('=I', exact(4))[0] != len(expected):
            raise ValueError('functional output case count mismatch')
        results = []
        for index, (n, b, values) in enumerate(expected):
            if struct.unpack('=II', exact(8)) != (n, b):
                raise ValueError('functional output shape/order mismatch')
            actual = struct.unpack('=' + str(n*n) + 'd', exact(n*n*8))
            if any(not math.isfinite(x) for x in actual):
                raise ValueError('non-finite functional output')
            error = max(abs(x-y) for x, y in zip(actual, values))
            if any(not math.isclose(x, y, rel_tol=1e-11, abs_tol=1e-12)
                   for x, y in zip(actual, values)):
                raise ValueError('independent oracle mismatch for case ' + str(index))
            results.append({'index': index, 'n': n, 'b': b, 'max_absolute_error': error,
                            'status': 'PASS'})
        if f.read(1):
            raise ValueError('trailing functional output data')
    return results


def functional_algorithms(build, result_dir, env, timeout):
    """Correctness component of the benchmark, never a performance candidate."""
    from fractions import Fraction
    root = Path(__file__).resolve().parents[1]
    build, result_dir = Path(build).resolve(), Path(result_dir).resolve()
    result_dir.mkdir(parents=True, exist_ok=True)
    report_path = result_dir / 'functional-algorithms.json'
    report = {'kind': 'functional_only', 'performance_eligible': False,
              'requirements': ['R-03', 'R-04', 'R-05', 'R-15', 'R-16', 'R-18', 'R-27'],
              'scope': 'Supplemental supported-subset examples; not the missing original source or full legal-domain coverage.',
              'validation': {'relative_tolerance': 1e-11, 'absolute_tolerance': 1e-12,
                             'cholesky': 'Known dyadic positive-diagonal factor plus supplied verifier and dag_check',
                             'wavefront': 'Exact Fraction reference evaluated column-first, independent of source traversal'},
              'algorithms': [], 'passed': False}
    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    clean = env.copy()
    for key in ('DAG_METADATA', 'DAG_STATS_FILE', 'COMPILER2026_TIMING_FILE',
                'DAG_BLOCK_SIZE', 'DAG_BLOCK_SIZES'):
        clean.pop(key, None)
    clean.update(DAG_APPLICATION='scalar', DAG_IO_FORMAT='public')
    report['effective_controls'] = {k: v for k, v in clean.items() if is_control(k)}
    shapes = [(3, 1), (7, 1), (16, 4), (31, 1)]
    try:
        report['program_sha256'] = digest(build / 'dag_run')
        report['compiler_sha256'] = digest(root / 'compiler/compile.py')
        report['checker_sha256'] = {name: digest(build / name)
                                    for name in ('public_verifier', 'dag_check')}
        with tempfile.TemporaryDirectory(prefix='dag-functional-') as temp:
            temp = Path(temp)
            for name, source, kind in [
                    ('naive_cholesky', 'naive_cholesky.cpp', 'cholesky'),
                    ('right_looking_cholesky', 'right_looking_cholesky.cpp', 'cholesky'),
                    ('scalar_wavefront', 'scalar_wavefront.cpp', 'wavefront')]:
                entry = {'name': name, 'kind': 'functional_only', 'performance_eligible': False,
                         'source': str(root / 'examples' / source),
                         'source_sha256': digest(root / 'examples' / source),
                         'catalog_sha256': digest(root / 'examples/naive_catalog.json'),
                         'runs': [], 'stages': [], 'passed': False}
                report['algorithms'].append(entry)
                print('[functional-only: ' + name + ']', flush=True)
                def run_stage(label, command, child_env):
                    record = {'stage': label, 'command': [str(x) for x in command]}
                    entry['stages'].append(record)
                    try:
                        run = subprocess.run(record['command'], cwd=root, env=child_env,
                                             capture_output=True, text=True, timeout=timeout)
                        record.update(returncode=run.returncode, stdout=run.stdout, stderr=run.stderr)
                    except (OSError, subprocess.TimeoutExpired) as error:
                        record['error'] = str(error)
                        raise
                    if run.returncode:
                        raise RuntimeError(name + ' ' + label + ' failed')
                    return run
                try:
                    metadata = temp / (name + '.meta')
                    run_stage('compile', [sys.executable, root/'compiler/compile.py',
                              root/'examples'/source, '--entry', name, '--catalog',
                              root/'examples/naive_catalog.json', '-o', metadata], clean)
                    entry.update(metadata_bytes=metadata.stat().st_size, metadata_sha256=digest(metadata))
                    inp = temp / (name + '.bin')
                    expected = []
                    with inp.open('wb') as f:
                        f.write(struct.pack('=I', len(shapes)))
                        for n, b in shapes:
                            if kind == 'cholesky':
                                factor = [[(2 + i/32) if i == j else
                                           (((i*7+j*3)%11-5)/32 if j < i else 0.0)
                                           for j in range(n)] for i in range(n)]
                                a = [sum(factor[i][k]*factor[j][k] for k in range(n))
                                     for i in range(n) for j in range(n)]
                                values = [x for row in factor for x in row]
                            else:
                                a = [((i*5+j*3)%17-8)/16 for i in range(n) for j in range(n)]
                                ref = [Fraction(x) for x in a]
                                for col in range(1, n):
                                    for row in range(1, n):
                                        at = row*n+col
                                        ref[at] = Fraction(a[at]) + ref[at-n]/4 + ref[at-1]/2
                                values = [float(x) for x in ref]
                            f.write(struct.pack('=II', n, b))
                            f.write(struct.pack('=' + str(n*n) + 'd', *a))
                            expected.append((n, b, values))
                    entry['input_sha256'] = digest(inp)
                    for mode in ('serial', 'parallel'):
                        out, stats = temp/(name+'-'+mode+'.out'), temp/(name+'-'+mode+'.json')
                        child = clean.copy()
                        child.update(DAG_METADATA=str(metadata), DAG_STATS_FILE=str(stats), DAG_MODE=mode)
                        run_stage(mode, [build/'dag_run', inp, out], child)
                        runtime = json.loads(stats.read_text())
                        run_record = {'requested_mode': mode, 'performance_eligible': False,
                                      'runtime_diagnostics': runtime, 'output_sha256': digest(out),
                                      'passed': False}
                        entry['runs'].append(run_record)
                        observed = runtime.get('cases', [])
                        if len(observed) != len(shapes) or any(
                                (c.get('index'), c.get('n'), c.get('b')) != (i, n, b)
                                for i, (c, (n, b)) in enumerate(zip(observed, shapes))):
                            raise ValueError('runtime case identity mismatch')
                        if mode == 'parallel' and any(c.get('execution_mode') != 'parallel' for c in observed):
                            raise ValueError('parallel functional coverage not achieved; inspect resource diagnostics')
                        rows = verify_algorithm_outputs(out, expected)
                        if kind == 'cholesky':
                            for checker in ('public_verifier', 'dag_check'):
                                checked = run_stage(mode+'-'+checker, [build/checker, inp, out], child)
                                expected_prefixes = [f'case={i} n={n} b={b} scaled_residual='
                                                     for i, (n, b) in enumerate(shapes)]
                                lines = checked.stdout.splitlines()
                                if len(lines) != len(shapes) or any(
                                        not line.startswith(prefix) or not line.endswith('status=PASS')
                                        for line, prefix in zip(lines, expected_prefixes)):
                                    raise ValueError('checker did not report all matching PASS rows')
                        run_record.update(cases=rows, passed=True)
                    entry['passed'] = True
                except (OSError, ValueError, RuntimeError, KeyError, subprocess.TimeoutExpired) as error:
                    entry['error'] = str(error)
                save()
        report['passed'] = len(report['algorithms']) == 3 and all(x['passed'] for x in report['algorithms'])
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
    save()
    print('Functional-only algorithms: ' + ('PASS' if report['passed'] else 'FAIL') +
          '; excluded from performance statistics. Report: ' + str(report_path), flush=True)
    return report


def verify_algorithms(argv):
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description='Build and verify supplemental algorithms; no performance ranking.')
    parser.add_argument('--build-dir', type=Path, default=root/'build')
    parser.add_argument('--output-dir', type=Path, default=root/'artifacts/functional')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--timeout', type=float, default=600)
    add_controls(parser)
    args = parser.parse_args(argv)
    env, policy = execution_environment(args, parser)
    if args.jobs < 1 or not (0 < args.timeout < float('inf')):
        parser.error('invalid jobs/timeout')
    for command in ([ 'cmake', '-S', str(root), '-B', str(args.build_dir), '-DCMAKE_BUILD_TYPE=Release'],
                    ['cmake', '--build', str(args.build_dir), '--parallel', str(args.jobs),
                     '--target', 'dag_run', 'public_verifier', 'dag_check']):
        run = subprocess.run(command, env=env, timeout=args.timeout)
        if run.returncode:
            return run.returncode
    report = functional_algorithms(args.build_dir, args.output_dir, env, args.timeout)
    return int(not report['passed'])



def estimate_schedule(argv):
    """Explicit DAG-kernel calibration and scheduling model; never a score."""
    p = argparse.ArgumentParser(description=estimate_schedule.__doc__)
    p.add_argument('--metadata', type=Path, required=True)
    p.add_argument('--n', type=int, required=True)
    p.add_argument('--b', type=int, required=True)
    p.add_argument('--workers', type=int, nargs='+', default=[1, 2, 4, 8])
    p.add_argument('--samples', type=int, default=5)
    p.add_argument('--warmups', type=int, default=0)
    p.add_argument('--graph-memory-mb', type=int, default=1024)
    p.add_argument('--max-tasks', type=int, default=0)
    p.add_argument('--max-steps', type=int, default=0)
    p.add_argument('--loop-chunk', type=int, default=1,
                   help='maximum iterations represented by one compressed task')
    p.add_argument('--loop-parts', type=int, default=0,
                   help='maximum balanced parts per affine For; overrides loop-chunk when positive')
    p.add_argument('--calibration-in', type=Path,
                   help='reuse operator timing samples from a prior estimator run')
    p.add_argument('--calibration-out', type=Path,
                   help='write operator timing samples for reuse by another run')
    p.add_argument('--backend', choices=['atomic', 'owner'], default='atomic')
    p.add_argument('--policy', choices=['current', 'fifo'], nargs='+', default=['current', 'fifo'])
    p.add_argument('--task-overhead-ns', type=int, default=0)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--dag-estimate', type=Path, help=argparse.SUPPRESS)
    a = p.parse_args(argv)
    if a.n < 3 or a.b < 1 or a.n % a.b or a.samples < 1 or a.warmups < 0:
        p.error('require n>=3, b>0, n%b==0, samples>=1 and warmups>=0')
    if any(w < 1 for w in a.workers) or any(x < 0 for x in (a.graph_memory_mb, a.max_tasks, a.max_steps, a.task_overhead_ns, a.loop_chunk, a.loop_parts)):
        p.error('workers and resource limits must be positive/nonnegative')
    exe = a.dag_estimate or (a.metadata.resolve().parent / 'dag_estimate')
    if not exe.exists():
        p.error('dag_estimate executable not found; build target dag_estimate first')
    base = [str(exe), '--metadata', str(a.metadata.resolve()), '--n', str(a.n), '--b', str(a.b),
            '--workers', ','.join(map(str, a.workers)), '--samples', str(a.samples), '--warmups', str(a.warmups),
            '--graph-memory-mb', str(a.graph_memory_mb), '--max-tasks', str(a.max_tasks), '--max-steps', str(a.max_steps),
            '--loop-chunk', str(a.loop_chunk), '--loop-parts', str(a.loop_parts),
            '--policies', ','.join(a.policy), '--task-overhead-ns', str(a.task_overhead_ns)]
    if a.calibration_in:
        base += ['--calibration-in', str(a.calibration_in.resolve())]
    if a.calibration_out:
        a.calibration_out.parent.mkdir(parents=True, exist_ok=True)
        base += ['--calibration-out', str(a.calibration_out.resolve())]
    run = subprocess.run(base, capture_output=True, text=True)
    report = {'command': 'estimate-schedule', 'configuration': {k: (str(v) if isinstance(v, Path) else v) for k, v in vars(a).items()} ,
              'host': {'platform': platform.platform(), 'logical_cpus': os.cpu_count()},
              'metadata': {'path': str(a.metadata.resolve()), 'bytes': a.metadata.stat().st_size, 'sha256': digest(a.metadata)},
              'kernel_library': 'linked by dag_estimate (current CMake kernel target)',
              'backend': a.backend, 'assumptions': ['MODEL ONLY', 'NOT AN END-TO-END PERFORMANCE RESULT', 'NOT A CORRECTNESS RESULT',
                 'warmups=0 by default; samples initialize buffers outside timing, which does not guarantee cold caches',
                 'single-threaded kernel calibration; no cache contention, bandwidth, NUMA, synchronization, wakeup or frequency effects',
                 'scheduler model is atomic/current or FIFO only; --backend is recorded for runtime comparison and does not model owner inbox routing',
                 'copy, zero_upper, metadata decode, expansion, graph build, allocation and executor startup are excluded from modeled kernel work'],
              'performance_eligible': False, 'correctness_validated': False}
    if run.returncode:
        report['status'] = 'resource_limited' if run.returncode == 3 else 'error'
        report['error'] = run.stderr.strip() or 'dag_estimate failed'
    else:
        try:
            report.update(json.loads(run.stdout))
            report['results_by_policy'] = {pol: [x for x in report.get('results', []) if x.get('policy') == pol]
                                           for pol in a.policy}
            report['status'] = 'ok'
            svg = a.report.with_suffix('.svg')
            vals = [('baseline', report.get('baseline_timing', {}).get('median', 0.0))]
            vals += [(str(x.get('policy')) + '-' + str(x.get('workers')), x.get('estimated_dag_seconds', 0.0))
                     for x in report.get('results', [])]
            scale = max([v for _, v in vals] or [1.0]) or 1.0
            bars = ''.join('<text x="10" y="%d">%s</text><rect x="120" y="%d" width="%.1f" height="16" fill="#4472c4"/><text x="%d" y="%d">%.6g s</text>'
                           % (25+i*25, name, 10+i*25, 300*v/scale, 130+300*v/scale, 23+i*25, v)
                           for i, (name, v) in enumerate(vals))
            svg.parent.mkdir(parents=True, exist_ok=True)
            svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="520" height="%d"><text x="10" y="15">DAG model vs demo baseline (diagnostic)</text>%s</svg>\n'
                           % (35+25*len(vals), bars))
            report['comparison_plot'] = str(svg.resolve())
        except (ValueError, OSError) as ex:
            report['status'] = 'error'; report['error'] = str(ex)
    a.report.parent.mkdir(parents=True, exist_ok=True); a.report.write_text(json.dumps(report, indent=2) + '\n')
    print('MODEL ONLY\nNOT AN END-TO-END PERFORMANCE RESULT\nNOT A CORRECTNESS RESULT')
    print(json.dumps({'status': report['status'], 'task_count': report.get('task_count'), 'edge_count': report.get('edge_count'), 'report': str(a.report)}))
    return 0 if report['status'] == 'ok' else 1

def main():
    commands = {'accept-public20': accept_public20, 'compare-reports': compare_reports, 'submit': submit,
                'verify-algorithms': verify_algorithms, 'estimate-schedule': estimate_schedule}
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
    p.add_argument('--validate-algorithms', action='store_true',
                   help='run supplemental functional checks after timing; excluded from performance statistics')
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
    report['requirements'] += ['R-25', 'R-26', 'R-27']
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
                # Invalid/mismatched evidence cannot establish any progress.
                sample['case_categories'] = {name: [] for name in CASE_CATEGORIES}
                sample['case_categories']['error_or_unknown'] = [c['index'] for c in report['input_cases']]
                sample['case_counts'] = {name: len(rows) for name, rows in sample['case_categories'].items()}
                failed = True
            report['samples'].append(sample)
            # Persist after every sample, including failed or interrupted runs.
            a.report.parent.mkdir(parents=True, exist_ok=True)
            a.report.write_text(json.dumps(report, indent=2) + '\n')
    summarize(report)
    report['passed'] = report['passed'] and not failed
    if a.validate_algorithms:
        functional_dir = a.report.parent / (a.report.stem + '-functional')
        functional = functional_algorithms(a.program.resolve().parent, functional_dir, env_base, a.timeout)
        report['functional_validation'] = {
            'passed': functional['passed'], 'performance_eligible': False,
            'report': str((functional_dir / 'functional-algorithms.json').resolve())}
        report['passed'] = report['passed'] and functional['passed']
    a.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], **report['summary']}))
    return int(not report['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
