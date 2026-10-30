"""Generate a small, self-contained HTML demonstration from an actual run."""
import argparse
import csv
import datetime
import json
import math
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile


def main(argv=None, on_progress=None):
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export', action='store_true', help='generate a static snapshot instead of opening the web demo')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--n', type=int, default=6)
    parser.add_argument('--b', type=int, default=2)
    parser.add_argument('--threads', type=int, default=2)
    parser.add_argument('--seed', type=int, default=17)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--clang', type=Path, help='Clang executable for offline source analysis')
    parser.add_argument('--build-dir', type=Path, default=root / 'build-visual-demo')
    parser.add_argument('--output-dir', type=Path, default=root / 'artifacts/visual-demo')
    args = parser.parse_args(argv)
    if not (3 <= args.n <= 16 and 0 < args.b <= args.n and args.n % args.b == 0
            and args.n // args.b <= 4 and args.threads >= 1 and args.jobs >= 1
            and 0 <= args.seed <= 0xffffffff):
        parser.error('demo requires 3<=n<=16, b>0, n%b=0, n/b<=4, positive threads/jobs')
    if not args.export:
        from visual_demo_server import serve
        return serve(args)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    dest = Path(tempfile.mkdtemp(prefix=f'n{args.n}-b{args.b}-', dir=args.output_dir.resolve()))
    build = args.build_dir.resolve()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('DAG_', 'COMPILER2026_'))
           and k not in ('VERIFIER_THREADS', 'SPD_GENERATOR_THREADS')}
    env.update(SPD_GENERATOR_THREADS='1', VERIFIER_THREADS='1', DAG_CHECK_THREADS='1',
               DAG_IO_FORMAT='public', DAG_APPLICATION='blocked', DAG_MODE='parallel',
               DAG_SCHEDULE='graph', DAG_LOOP_CHUNK='1', DAG_LOOP_PARTS='0',
               DAG_BACKEND='atomic', DAG_BATCH='1', DAG_AUDIT='0',
               DAG_THREADS=str(args.threads), DAG_METADATA=str(dest / 'block_cholesky.meta'),
               DAG_STATS_FILE=str(dest / 'runtime.json'),
               COMPILER2026_TIMING_FILE=str(dest / 'time.txt'))
    commands = []

    def progress(stage, **extra):
        if on_progress:
            on_progress(stage, dest, **extra)

    progress('starting')

    def run(name, command):
        progress(name)
        print('[' + name + ']', flush=True)
        result = subprocess.run([str(x) for x in command], cwd=root, env=env,
                                capture_output=True, text=True)
        (dest / (name + '.log')).write_text(result.stdout + result.stderr)
        commands.append({'stage': name, 'args': [str(x) for x in command],
                         'returncode': result.returncode})
        if result.returncode:
            raise RuntimeError(name + ' failed: ' + result.stderr[-3000:])
        progress(name + '-done')
        return result.stdout

    try:
        configure = ['cmake', '-S', root, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
                     '-DDAG_SANITIZE=OFF', '-DDAG_TSAN=OFF', '-DDAG_REQUIRE_OFFICIAL=OFF',
                     '-DDAG_OFFICIAL_LIBRARY=', '-DDAG_METADATA_ENCODING=auto']
        if args.clang:
            configure.append('-DDAG_LLVM_COMPILER=' + str(args.clang.resolve()))
        run('configure', configure)
        run('build', ['cmake', '--build', build, '--parallel', args.jobs, '--target',
                      'dag_run', 'spd_generator', 'public_verifier', 'dag_check', 'dag_demo_export'])
        shutil.copy2(build / 'block_cholesky.meta', dest / 'block_cholesky.meta')
        shutil.copy2(build / 'catalog.generated.json', dest / 'catalog.generated.json')
        source = root / 'vendor/demo/src/baseline/block_cholesky.cpp'
        shutil.copy2(source, dest / 'block_cholesky.cpp')
        # Readable diagnostics are generated offline and are not part of the
        # execution Metadata. Names/digests are not added to the scored file.
        compiler = runpy.run_path(str(root / 'compiler/compile.py'))
        document = compiler['compile_source'](source.read_text(),
                    (dest / 'catalog.generated.json').read_bytes(), include_provenance=False)
        (dest / 'metadata.json').write_text(json.dumps(document, indent=2) + '\n')
        assert compiler['encode_metadata'](document) == (dest / 'block_cholesky.meta').read_bytes()
        run('generate', [build / 'spd_generator', dest / 'input.bin', args.n, args.b, args.seed])
        run('execute', [build / 'dag_run', dest / 'input.bin', dest / 'result.out'])
        checks = {}
        for checker in ('public_verifier', 'dag_check'):
            output = run(checker, [build / checker, dest / 'input.bin', dest / 'result.out'])
            expected = f'case=0 n={args.n} b={args.b} scaled_residual='
            lines = output.strip().splitlines()
            if len(lines) != 1 or not lines[0].startswith(expected) or not lines[0].endswith('status=PASS'):
                raise ValueError('checker did not report the expected case as PASS')
            checks[checker] = {'passed': True, 'output': output.strip(),
                              'scaled_residual': float(lines[0].split('scaled_residual=')[1].split()[0])}
        graph = json.loads(run('graph', [build / 'dag_demo_export', dest / 'block_cholesky.meta',
                                        args.n, args.b]))
        (dest / 'graph.json').write_text(json.dumps(graph, indent=2) + '\n')

        def matrix(path):
            raw = path.read_bytes()
            if len(raw) != 12 + 8 * args.n * args.n or struct.unpack_from('=III', raw) != (1, args.n, args.b):
                raise ValueError('unexpected demo matrix file layout')
            values = struct.unpack_from('=' + str(args.n * args.n) + 'd', raw, 12)
            if not all(math.isfinite(x) for x in values):
                raise ValueError('non-finite demo matrix')
            return [list(values[i*args.n:(i+1)*args.n]) for i in range(args.n)]

        a, lower = matrix(dest / 'input.bin'), matrix(dest / 'result.out')
        reconstructed = [[sum(lower[i][k] * lower[j][k] for k in range(args.n))
                          for j in range(args.n)] for i in range(args.n)]
        error = [[a[i][j] - reconstructed[i][j] for j in range(args.n)] for i in range(args.n)]
        for filename, values in [('input.csv', a), ('result.csv', lower),
                                 ('reconstructed.csv', reconstructed)]:
            with (dest / filename).open('w', newline='') as stream:
                csv.writer(stream).writerows([[format(x, '.17g') for x in row] for row in values])
        runtime = json.loads((dest / 'runtime.json').read_text())
        case = runtime['cases'][0]
        if (case['resource_limited'] or case['tasks'] != graph['semantic_task_count']
                or case['edges'] != len(graph['edges'])):
            raise ValueError('exported graph does not match the executed configuration')
        payload = {'n': args.n, 'b': args.b, 'seed': args.seed,
                   'created': datetime.datetime.now().astimezone().isoformat(),
                   'matrix_a': a, 'matrix_l': lower, 'reconstructed': reconstructed,
                   'error': error, 'max_abs_error': max(abs(x) for row in error for x in row),
                   'graph': graph, 'metadata': document, 'runtime': runtime, 'checks': checks,
                   'metadata_bytes': (dest / 'block_cholesky.meta').stat().st_size,
                   'source': source.read_text(),
                   'note': 'Real runtime output; graph playback illustrates dependencies, not measured thread chronology. Display/export/validation are outside runtime timing.'}
        (dest / 'demo.json').write_text(json.dumps(payload, ensure_ascii=False, indent=2) + '\n')
        template = (root / 'examples/visual_demo.html').read_text()
        html = template.replace('__DEMO_DATA__', json.dumps(payload, ensure_ascii=False).replace('<', '\\u003c'))
        (dest / 'index.html').write_text(html)
        manifest = {'passed': True, 'n': args.n, 'b': args.b, 'commands': commands,
                    'runtime_configuration': {k: v for k, v in env.items() if k.startswith('DAG_')},
                    'files': sorted(p.name for p in dest.iterdir()),
                    'note': 'Small visual demonstration; not a benchmark or official target result.'}
        (dest / 'run.json').write_text(json.dumps(manifest, indent=2) + '\n')
        progress('complete')
        print('Demo: ' + str(dest / 'index.html'))
        print('Retained: input.bin, block_cholesky.meta, result.out, result.csv, metadata.json')
        print(f'PASS; n={args.n}, b={args.b}, metadata={payload["metadata_bytes"]} bytes')
        return 0
    except (OSError, ValueError, RuntimeError, AssertionError) as error:
        (dest / 'run.json').write_text(json.dumps({'passed': False, 'error': str(error),
                                                  'commands': commands}, indent=2) + '\n')
        progress('failed', error=str(error))
        print('Visual demo failed: ' + str(error) + '; logs: ' + str(dest), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
