#!/usr/bin/env python3
"""Read-only model preflight; --run explicitly opts into one GPU-only token."""
import argparse
import json
import math
import os
import re
from pathlib import Path
import signal
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODEL = Path.home() / '.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit'


def census(model):
    config = json.loads((model / 'config.json').read_text())
    text = config.get('text_config', config)
    quant = config.get('quantization', text.get('quantization', {}))
    if quant.get('bits') != 6 or quant.get('group_size') != 64:
        raise ValueError('This qualification requires the local affine6bit/group64 checkpoint')
    if not (model / 'tokenizer.json').is_file():
        raise ValueError('Local tokenizer.json is required; downloads are not permitted')
    tensors = {}
    for shard in sorted(model.glob('*.safetensors')):
        with shard.open('rb') as stream:
            raw = stream.read(8)
            if len(raw) != 8:
                raise ValueError(f'Truncated tensor file: {shard.name}')
            length = struct.unpack('<Q', raw)[0]
            if length > 64 * 1024 * 1024 or length + 8 > shard.stat().st_size:
                raise ValueError(f'Invalid tensor header: {shard.name}')
            header = json.loads(stream.read(length))
        for name, entry in header.items():
            if name == '__metadata__':
                continue
            begin, end = entry['data_offsets']
            if not 0 <= begin <= end <= shard.stat().st_size - length - 8 or name in tensors:
                raise ValueError(f'Invalid/duplicate tensor: {name}')
            tensors[name] = end - begin
    if not tensors:
        raise ValueError('No safetensors payload found')
    payload = sum(tensors.values())
    text_tensors = {name: size for name, size in tensors.items()
                    if name.startswith('language_model.')}
    if not text_tensors:
        raise ValueError('Expected Qwen language_model tensor namespace')
    text_payload = sum(text_tensors.values())
    slab = 2 << 30  # Native policy for the known32GiB GPU; verified at device open.
    remaining = []
    for size in text_tensors.values():
        need = (size + 4095) & ~4095
        for index, free in enumerate(remaining):
            if free >= need:
                remaining[index] -= need
                break
        else:
            remaining.append(max(slab, need) - need)
    layers = text['num_hidden_layers']
    full_layers = layers // text['full_attention_interval']
    recurrent_layers = layers - full_layers
    state = recurrent_layers * text['linear_num_value_heads'] * text['linear_value_head_dim'] ** 2 * 4
    conv = recurrent_layers * (text['linear_conv_kernel_dim'] - 1) * (
        2 * text['linear_num_key_heads'] * text['linear_key_head_dim'] +
        text['linear_num_value_heads'] * text['linear_value_head_dim']) * 4
    kv = full_layers * 2 * 128 * text['num_key_value_heads'] * text['head_dim'] * 4
    return {
        'model': str(model), 'tensors': len(tensors), 'weight_payload_bytes': payload,
        'weight_payload_gib': payload / 2**30, 'largest_tensor_bytes': max(tensors.values()),
        'text_tensors': len(text_tensors), 'text_weight_payload_bytes': text_payload,
        'text_weight_payload_gib': text_payload / 2**30,
        'unused_vision_tensors': sum(name.startswith('vision_tower.') for name in tensors),
        'unused_vision_payload_bytes': sum(size for name, size in tensors.items()
                                          if name.startswith('vision_tower.')),
        'weight_slab_bytes_on_32gib_gpu': slab,
        'minimum_weight_slabs': math.ceil(text_payload / slab),
        'header_order_packing_estimate_slabs': len(remaining),
        'packing_note': 'Estimate only: actual binder order can differ; live slabs are logged.',
        'recurrent_state_bytes_batch1': state, 'convolution_tail_bytes_batch1': conv,
        'kv128_f32_upper_bound_bytes_batch1': kv,
        'memory_note': 'Excludes compiler/runtime allocations, retained intermediate tensors and fragmentation.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, default=DEFAULT_MODEL)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--timeout-seconds', type=int, default=1200)
    parser.add_argument('--log-dir', type=Path, default=ROOT / 'build/tests/lse-qwen-smoke')
    args = parser.parse_args()
    if not 30 <= args.timeout_seconds <= 3600:
        parser.error('timeout must be30..3600 seconds')
    model = args.model.resolve()
    metadata = census(model)
    executable = ROOT / 'build/lse-macos-adapter/lse'
    command = [str(executable), '--pool', 'hrx:0', '--dialect', 'loom', '--model', str(model),
               '--kv-len', '128', '--no-mtp', '-n', '1', '-t', '0', '--stats',
               'The capital of France is']
    metadata['command'] = command
    print(json.dumps(metadata, indent=2), flush=True)
    if not args.run:
        print('Preflight only: model payloads unchanged; no GPU calls.', flush=True)
        return 0
    args.log_dir.mkdir(parents=True, exist_ok=True)
    (args.log_dir / 'preflight.json').write_text(json.dumps(metadata, indent=2) + '\n')
    env = os.environ.copy()
    env['DYLD_LIBRARY_PATH'] = str(ROOT / 'build/hsa')
    env['LSE_REQUIRE_DEVICE_KERNELS'] = '1'
    env['LSE_TIME_LOAD'] = '1'
    # Keep the already-validated signal implementation for this qualification.
    env.pop('MAC_HSA_SIGNAL_BACKEND', None)
    started = time.monotonic()
    timed_out = False
    log = args.log_dir / 'inference.log'
    with log.open('wb') as output:
        child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        print(f'pid={child.pid} log={log} deadline={args.timeout_seconds}s', flush=True)
        try:
            child.wait(timeout=args.timeout_seconds)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            timed_out = True
            for sig, grace in [(signal.SIGINT, 15), (signal.SIGTERM, 15), (signal.SIGKILL, 5)]:
                print(f'Requesting {sig.name}; GPU retirement must be checked separately.', flush=True)
                try:
                    os.killpg(child.pid, sig)
                except ProcessLookupError:
                    break
                try:
                    child.wait(timeout=grace)
                    break
                except subprocess.TimeoutExpired:
                    continue
    transcript = log.read_text(errors='replace')
    groups = re.search(r'groups device=(\d+) host=(\d+) views=\d+ fallbacks=(\d+)', transcript)
    generated = re.search(r'generated (\d+) tokens', transcript)
    qualified = bool(not timed_out and child.returncode == 0 and groups and generated and
                     int(groups[1]) > 0 and int(groups[2]) == 0 and int(groups[3]) == 0 and
                     int(generated[1]) == 1)
    result = {'qualified_gpu_one_token': qualified, 'returncode': child.poll(), 'elapsed_seconds': time.monotonic() - started,
              'timed_out_or_interrupted': timed_out,
              'retirement': 'unconfirmed; inspect driver state' if timed_out else 'inspect normal shutdown and driver state'}
    (args.log_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return 124 if timed_out else (0 if qualified else 1)


if __name__ == '__main__':
    raise SystemExit(main())
