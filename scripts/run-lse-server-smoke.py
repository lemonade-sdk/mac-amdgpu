#!/usr/bin/env python3
"""Explicit GPU HTTP qualification: session reuse, timings, and clean shutdown."""
import argparse
import json
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--chat-only', action='store_true', help='isolate chat from session-reuse checks')
    parser.add_argument('--model', type=Path, default=Path.home() /
                        '.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit')
    parser.add_argument('--log-dir', type=Path, default=ROOT / 'build/tests/lse-server-smoke')
    args = parser.parse_args()
    if not args.run:
        print('Pass --run to load the local model, submit GPU HTTP requests, and stop the server.')
        return 0
    args.log_dir.mkdir(parents=True, exist_ok=True)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    command = [str(ROOT / 'build/lse-macos-adapter/lse-server'),
               '--model', str(args.model.resolve()), '--pool', 'hrx:0', '--dialect', 'loom',
               '--no-mtp', '--kv-len', '128', '--host', '127.0.0.1', '--port', str(port),
               '--served-name', 'gpu-qwen-check', '--max-tokens', '64',
               '--shutdown-grace-seconds', '30']
    env = os.environ.copy()
    env['DYLD_LIBRARY_PATH'] = str(ROOT / 'build/hsa')
    env['LSE_REQUIRE_DEVICE_KERNELS'] = '1'
    result = {'command': command, 'scenario': 'chat-only' if args.chat_only else 'interleaved',
              'requests': [], 'qualified_http_gpu': False}
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, payload=None, timeout=180):
        data = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(base + path, data=data,
                                     headers={'Content-Type': 'application/json'})
        with opener.open(req, timeout=timeout) as response:
            return json.load(response)

    with (args.log_dir / 'server.log').open('wb') as log:
        child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        result['pid'] = child.pid
        print(f'pid={child.pid} url={base} log={args.log_dir / "server.log"}', flush=True)
        try:
            deadline = time.monotonic() + 180
            while True:
                if child.poll() is not None:
                    raise RuntimeError(f'server exited during startup: {child.returncode}')
                try:
                    result['health'] = request('/health', timeout=2)
                    models = request('/v1/models', timeout=2)
                    if any(m.get('id') == 'gpu-qwen-check' for m in models.get('data', [])):
                        break
                except (OSError, urllib.error.URLError):
                    pass
                if time.monotonic() >= deadline:
                    raise TimeoutError('server did not become ready within 180 seconds')
                time.sleep(0.25)
            cases = [
                ('/v1/completions', {'prompt': 'The', 'max_tokens': 4}),
                # End on a wider prefill pass, then return to the cached one-token
                # path. Ending every request in decode can hide stale carry-ins.
                ('/v1/completions', {'prompt': 'The capital of France is', 'max_tokens': 1}),
                ('/v1/completions', {'prompt': 'The', 'max_tokens': 4}),
                ('/v1/chat/completions', {'messages': [
                    {'role': 'system', 'content': 'Answer briefly.'},
                    {'role': 'user', 'content': 'What is the capital of Germany?'}],
                    'max_tokens': 64}),
                ('/v1/completions', {'prompt': 'The', 'max_tokens': 4}),
            ]
            if args.chat_only:
                cases = [case for case in cases if case[0] == '/v1/chat/completions']
            first_completion = None
            for path, payload in cases:
                payload.update(model='gpu-qwen-check', temperature=0, stream=False)
                started = time.monotonic()
                response = request(path, payload)
                entry = {'endpoint': path, 'elapsed_seconds': time.monotonic() - started,
                         'request': payload, 'response': response}
                result['requests'].append(entry)
                (args.log_dir / f'response-{len(result["requests"])}.json').write_text(
                    json.dumps(entry, indent=2) + '\n')
                choice = response['choices'][0]
                text = choice.get('text', choice.get('message', {}).get('content', ''))
                # A one-token prefill fixture can legitimately emit only a
                # space; its purpose is to exercise state and timing boundaries.
                if not text or (payload['max_tokens'] > 1 and not text.strip()):
                    raise RuntimeError('empty generated response')
                if payload.get('prompt') == 'The':
                    if first_completion is None:
                        first_completion = (text, response['usage'])
                    elif (text, response['usage']) != first_completion:
                        raise RuntimeError('single-token prompt changed after resident session restart')
                timing, usage = response['timings'], response['usage']
                if payload.get('prompt') == 'The' and timing['prompt_n'] != 1:
                    raise RuntimeError('single-token restart fixture did not tokenize to one token')
                generated = usage['completion_tokens']
                if not 0 < generated <= payload['max_tokens']:
                    raise RuntimeError('invalid completion count')
                if timing['generated_n'] != generated or timing['decode_n'] != generated - 1:
                    raise RuntimeError('decode count includes the prefill token or loses tokens')
                for prefix in ('prompt', 'decode'):
                    count, ms, rate = (timing[prefix + suffix] for suffix in
                                       ('_n', '_ms', '_per_second'))
                    if ms < 0 or rate < 0 or not math.isfinite(rate):
                        raise RuntimeError('invalid timing')
                    expected = count * 1000 / ms if ms > 0 else 0
                    if not math.isclose(rate, expected, rel_tol=1e-9, abs_tol=1e-9):
                        raise RuntimeError('timing rate does not match its count and duration')
                print(json.dumps({'endpoint': path, 'text': text, 'timings': timing}), flush=True)
            result['requests_passed'] = True
        except urllib.error.HTTPError as error:
            result['error'] = f'HTTP {error.code}: {error.read().decode(errors="replace")}'
            print(f'FAIL: {result["error"]}', flush=True)
        except Exception as error:
            result['error'] = str(error)
            print(f'FAIL: {error}', flush=True)
        finally:
            if child.poll() is None:
                child.send_signal(signal.SIGINT)
            try:
                result['server_returncode'] = child.wait(timeout=45)
            except subprocess.TimeoutExpired:
                result['shutdown_error'] = 'server still running; inspect PID and GPU retirement'
            result['qualified_http_gpu'] = (result.get('requests_passed', False) and
                                            result.get('server_returncode') == 0)
            (args.log_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'qualified_http_gpu': result['qualified_http_gpu'],
                      'server_returncode': result.get('server_returncode')}), flush=True)
    return 0 if result['qualified_http_gpu'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
