"""Local-only web controls for the optional visual demonstration."""
import json
import mimetypes
from pathlib import Path
import struct
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit


def serve(options):
    from visual_demo import main as generate
    root = Path(__file__).resolve().parents[1]
    output = options.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    state = {'status': 'idle', 'stage': '', 'error': None, 'result_url': None,
             'input': None, 'metadata': None, 'directory': None}
    labels = {'starting': '准备本次运行', 'configure': '检查构建配置',
              'build': '构建程序并生成 Metadata', 'generate': '生成输入矩阵 A',
              'execute': '计算输出矩阵 L', 'public_verifier': '原包 verifier 校验',
              'dag_check': '加强数值校验', 'graph': '准备矩阵和依赖图展示',
              'complete': '完成：输入、Metadata 和输出已保存', 'failed': '运行失败'}

    def progress(stage, dest, **extra):
        with lock:
            state['stage'] = labels.get(stage, labels.get(stage.removesuffix('-done'), stage))
            state['directory'] = str(dest)
            state['files_url'] = '/runs/' + dest.name + '/'
            if stage == 'generate-done':
                raw = (dest / 'input.bin').read_bytes()
                count, n, b = struct.unpack_from('=III', raw)
                values = struct.unpack_from('=' + str(n*n) + 'd', raw, 12)
                state['input'] = {'n': n, 'b': b,
                                  'values': [list(values[i*n:(i+1)*n]) for i in range(n)]}
                state['metadata'] = {'bytes': (dest / 'block_cholesky.meta').stat().st_size,
                                     'program': json.loads((dest / 'metadata.json').read_text())['program']}
            if stage == 'complete':
                state['status'] = 'complete'
                state['result_url'] = state['files_url'] + 'index.html'
            elif stage == 'failed':
                state['status'] = 'failed'
                state['error'] = extra.get('error', '运行失败，请查看保留日志')

    def worker(params):
        args = ['--export', '--build-dir', str(options.build_dir.resolve()),
                '--output-dir', str(output), '--jobs', str(options.jobs)]
        for name in ('n', 'b', 'seed', 'threads'):
            args += ['--' + name, str(params[name])]
        if options.clang:
            args += ['--clang', str(options.clang.resolve())]
        try:
            code = generate(args, on_progress=progress)
            if code:
                with lock:
                    state['status'] = 'failed'
                    state['error'] = state['error'] or '本次运行未通过，请查看日志。'
        except Exception as error:
            with lock:
                state['status'] = 'failed'
                state['error'] = str(error)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, code, data, kind='application/json; charset=utf-8'):
            if not isinstance(data, bytes):
                data = json.dumps(data, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = unquote(urlsplit(self.path).path)
            if path == '/':
                page = (root / 'examples/visual_demo_control.html').read_text()
                defaults = {k: getattr(options, k) for k in ('n', 'b', 'threads', 'seed')}
                return self.send(200, page.replace('__DEFAULTS__', json.dumps(defaults)).encode(),
                                 'text/html; charset=utf-8')
            if path == '/api/state':
                with lock:
                    return self.send(200, state)
            if path.startswith('/runs/'):
                target = (output / path[len('/runs/'):]).resolve()
                if output in target.parents and target.is_file():
                    kind = mimetypes.guess_type(target.name)[0] or 'application/octet-stream'
                    return self.send(200, target.read_bytes(), kind)
            self.send(404, {'error': '没有找到文件'})

        def do_POST(self):
            if self.path != '/api/run':
                return self.send(404, {'error': '没有这个操作'})
            origin = self.headers.get('Origin')
            if origin and urlsplit(origin).netloc != self.headers.get('Host'):
                return self.send(403, {'error': '请从本地演示页面启动运行'})
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 4096 or not self.headers.get('Content-Type', '').startswith('application/json'):
                    raise ValueError('需要 JSON 参数')
                params = json.loads(self.rfile.read(length))
                if not isinstance(params, dict) or set(params) != {'n', 'b', 'threads', 'seed'}:
                    raise ValueError('需要 n、b、threads、seed 四个参数')
                if any(type(x) is not int for x in params.values()):
                    raise ValueError('参数必须是整数')
                n, b, threads, seed = (params[k] for k in ('n', 'b', 'threads', 'seed'))
                if not (3 <= n <= 16 and 0 < b <= n and n % b == 0 and n//b <= 4):
                    raise ValueError('小演示要求 3≤n≤16、n 能被 b 整除、每行最多4个块')
                if not (1 <= threads <= 24 and 0 <= seed <= 0xffffffff):
                    raise ValueError('线程数须为1～24，种子须为0～4294967295')
            except (ValueError, TypeError, json.JSONDecodeError) as error:
                return self.send(400, {'error': str(error)})
            with lock:
                if state['status'] == 'running':
                    return self.send(409, {'error': '已有一次运行正在进行，请等待完成'})
                state.update(status='running', stage='准备本次运行', error=None,
                             result_url=None, input=None, metadata=None, directory=None)
            threading.Thread(target=worker, args=(params,), daemon=True).start()
            self.send(202, {'started': True})

    if not 0 <= options.port <= 65535:
        raise ValueError('port must be 0..65535')
    server = ThreadingHTTPServer(('127.0.0.1', options.port), Handler)
    print(f'打开浏览器：http://127.0.0.1:{server.server_port}', flush=True)
    print('在页面点击“生成并运行”；Ctrl+C 停止服务。', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('Demo server stopped.', flush=True)
    finally:
        server.server_close()
    return 0
