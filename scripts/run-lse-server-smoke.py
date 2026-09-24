#!/usr/bin/env python3
"""Explicit GPU HTTP qualification: session reuse, timings, and clean shutdown."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import socket
import statistics
import subprocess
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--chat-only', action='store_true', help='isolate chat from session-reuse checks')
    parser.add_argument('--benchmark-repeats', type=int, default=0,
                        help='one warmup, then N identical completion requests (1..20)')
    parser.add_argument('--generated-tokens', type=int, default=33,
                        help='benchmark output cap; 33 gives 32 decode steps unless EOS ends early')
    parser.add_argument('--expected-prompt-tokens', type=int,
                        help='require this exact input token count in benchmark responses')
    parser.add_argument('--require-full-output', action='store_true',
                        help='fail if generation stops before the requested output token count')
    parser.add_argument('--prompt-file', type=Path, help='benchmark prompt text; default: France fixture')
    parser.add_argument('--kv-len', type=int, default=128)
    parser.add_argument('--request-timeout', type=int, default=180)
    parser.add_argument('--hsa-library-dir', type=Path, default=ROOT / 'build/hsa',
                        help='runtime build directory, recorded in the benchmark')
    parser.add_argument('--server', type=Path, default=ROOT / 'build/lse-macos-adapter/lse-server',
                        help='server executable to qualify, recorded with its content hash')
    parser.add_argument('--model', type=Path, default=Path.home() /
                        '.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit')
    parser.add_argument('--log-dir', type=Path, default=ROOT / 'build/tests/lse-server-smoke')
    args = parser.parse_args()
    if not 0 <= args.benchmark_repeats <= 20 or not 1 <= args.generated_tokens <= 4096:
        parser.error('benchmark repeats must be 0..20 and generated tokens must be 1..4096')
    if not 128 <= args.kv_len <= 8192 or args.generated_tokens >= args.kv_len:
        parser.error('KV length must be 128..8192 and exceed the output token cap')
    if not 30 <= args.request_timeout <= 3600:
        parser.error('request timeout must be 30..3600 seconds')
    if args.chat_only and args.benchmark_repeats:
        parser.error('--chat-only and --benchmark-repeats select different scenarios')
    if args.prompt_file and not args.benchmark_repeats:
        parser.error('--prompt-file requires --benchmark-repeats')
    if args.expected_prompt_tokens is not None:
        if not args.benchmark_repeats or args.expected_prompt_tokens < 1:
            parser.error('--expected-prompt-tokens requires a benchmark and a positive count')
        if args.expected_prompt_tokens + args.generated_tokens > args.kv_len:
            parser.error('KV length must cover the requested input plus output tokens')
    if args.require_full_output and not args.benchmark_repeats:
        parser.error('--require-full-output requires --benchmark-repeats')
    if not args.run:
        print('Pass --run to load the local model, submit GPU HTTP requests, and stop the server.')
        return 0
    args.log_dir.mkdir(parents=True, exist_ok=True)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    command = [str(args.server.resolve()),
               '--model', str(args.model.resolve()), '--pool', 'hrx:0', '--dialect', 'loom',
               '--no-mtp', '--kv-len', str(args.kv_len), '--host', '127.0.0.1', '--port', str(port),
               '--served-name', 'gpu-qwen-check', '--max-tokens', str(max(64, args.generated_tokens)),
               '--shutdown-grace-seconds', '30']
    env = os.environ.copy()
    env['DYLD_LIBRARY_PATH'] = str(args.hsa_library_dir.resolve())
    env['LSE_REQUIRE_DEVICE_KERNELS'] = '1'
    scenario = 'benchmark' if args.benchmark_repeats else ('chat-only' if args.chat_only else 'interleaved')
    result = {'command': command, 'scenario': scenario,
              'requests': [], 'qualified_http_gpu': False}
    result['server_sha256'] = hashlib.sha256(Path(command[0]).read_bytes()).hexdigest()
    hsa_library = args.hsa_library_dir.resolve() / 'libhsa-runtime64.dylib'
    result['hsa_library'] = str(hsa_library)
    result['hsa_sha256'] = hashlib.sha256(hsa_library.read_bytes()).hexdigest()
    result['model_config_sha256'] = hashlib.sha256((args.model / 'config.json').read_bytes()).hexdigest()
    result['measurement_environment'] = {key: env[key] for key in
        ('LSE_REQUIRE_DEVICE_KERNELS', 'LSE_TIME_SPANS', 'LSE_TIME_STEPS',
         'LSE_FLUSH_INTERVAL', 'LSE_AUTO_BATCH', 'LSE_AUTO_BATCH_TRACE',
         'LSE_SHARED_SCORE_SDPA', 'LSE_PROFILE_DISPATCH', 'MAC_HSA_SIGNAL_BACKEND',
         'MAC_HSA_BLOCKED_POLL_US') if key in env}
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, payload=None, timeout=args.request_timeout):
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
            if args.benchmark_repeats:
                prompt = args.prompt_file.read_text() if args.prompt_file else 'The capital of France is'
                if not prompt.strip():
                    raise ValueError('benchmark prompt is empty')
                result['benchmark'] = {'warmup_requests': 1, 'measured_requests': args.benchmark_repeats,
                    'prompt_sha256': hashlib.sha256(prompt.encode()).hexdigest(),
                    'requested_generated_tokens': args.generated_tokens,
                    'requested_decode_steps': args.generated_tokens - 1,
                    'scope': 'HTTP generation timings include sampling; not llama-bench kernel timings'}
                cases = [('/v1/completions', {'prompt': prompt, 'max_tokens': args.generated_tokens})
                         for _ in range(args.benchmark_repeats + 1)]
            first_completion = None
            benchmark_completion = None
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
                if args.require_full_output and generated != args.generated_tokens:
                    raise RuntimeError(f'output ended at {generated} tokens; required {args.generated_tokens}')
                if (args.expected_prompt_tokens is not None and
                        usage['prompt_tokens'] != args.expected_prompt_tokens):
                    raise RuntimeError(f'input has {usage["prompt_tokens"]} tokens; required {args.expected_prompt_tokens}')
                if timing['generated_n'] != generated or timing['decode_n'] != generated - 1:
                    raise RuntimeError('decode count includes the prefill token or loses tokens')
                if timing['prompt_n'] != usage['prompt_tokens']:
                    raise RuntimeError('prompt timing and usage counts differ')
                if args.benchmark_repeats:
                    current = (text, usage)
                    if benchmark_completion is None:
                        benchmark_completion = current
                    elif current != benchmark_completion:
                        raise RuntimeError('greedy benchmark response changed between identical requests')
                for prefix in ('prompt', 'decode'):
                    count, ms, rate = (timing[prefix + suffix] for suffix in
                                       ('_n', '_ms', '_per_second'))
                    if (ms < 0 or rate < 0 or not math.isfinite(ms) or not math.isfinite(rate)
                            or (count > 0 and ms == 0)):
                        raise RuntimeError('invalid timing')
                    expected = count * 1000 / ms if ms > 0 else 0
                    if not math.isclose(rate, expected, rel_tol=1e-9, abs_tol=1e-9):
                        raise RuntimeError('timing rate does not match its count and duration')
                print(json.dumps({'endpoint': path, 'text': text, 'timings': timing}), flush=True)
            if args.benchmark_repeats:
                measured = [r['response']['timings'] for r in result['requests'][1:]]
                summary = result['benchmark']
                summary['all_requested_decode_steps_completed'] = all(
                    t['decode_n'] == args.generated_tokens - 1 for t in measured)
                for prefix in ('prompt', 'decode'):
                    rates = [t[prefix + '_per_second'] for t in measured]
                    summary[prefix] = {'token_counts': [t[prefix + '_n'] for t in measured],
                        'rates': rates, 'median_tokens_per_second': statistics.median(rates),
                        'min_tokens_per_second': min(rates), 'max_tokens_per_second': max(rates)}
                print(json.dumps({'benchmark': summary}), flush=True)
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
