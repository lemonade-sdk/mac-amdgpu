#!/usr/bin/env python3
"""Export single-device IREE GPU dispatch timestamps as Chrome Trace JSON."""
import argparse
import json
from pathlib import Path


def export(source, output, kernels=None):
    catalog = {row['entry']: row for row in kernels or []}
    frequencies = {}
    events = []
    with source.open() as stream:
        for line in stream:
            row = json.loads(line)
            if row['record_type'] == 'device':
                if not row.get('timestamp_frequency_hz_present'):
                    raise ValueError('device timestamp frequency is missing')
                frequencies[row['physical_device_ordinal']] = row['timestamp_frequency_hz']
            elif row['record_type'] == 'dispatch_event':
                if not row.get('valid') or not row.get('duration_scale_available'):
                    raise ValueError('invalid or unscaled GPU dispatch')
                events.append(row)
    devices = {row['physical_device_ordinal'] for row in events}
    if len(devices) != 1:
        raise ValueError('one device is required; separate device clocks cannot be aligned')
    device = next(iter(devices))
    frequency = frequencies[device]
    if frequency <= 0:
        raise ValueError('device timestamp frequency must be positive')
    origin = min(row['start_tick'] for row in events)
    traces = [{'ph': 'M', 'name': 'process_name', 'pid': 1, 'tid': 0,
               'args': {'name': 'LSE GPU dispatches — device clock'}}]
    queues = sorted({row['queue_ordinal'] for row in events})
    for queue in queues:
        traces.append({'ph': 'M', 'name': 'thread_name', 'pid': 1, 'tid': queue,
                       'args': {'name': f'GPU queue {queue}'}})
    total_ns = 0
    for row in events:
        ticks = row['end_tick'] - row['start_tick']
        if ticks < 0 or abs(ticks * 1e9 / frequency - row['duration_ns']) > 1:
            raise ValueError('GPU duration and timestamp frequency disagree')
        info = catalog.get(row['key'], {})
        geometry = info.get('quant')
        name = info.get('family', row['key'])
        if geometry:
            name += ' ' + ' '.join(f'{key.upper()}{geometry[key]}' for key in ('m', 'n', 'k'))
        traces.append({'ph': 'X', 'name': name, 'cat': info.get('scope', 'gpu'),
                       'pid': 1, 'tid': row['queue_ordinal'],
                       'ts': (row['start_tick'] - origin) * 1e6 / frequency,
                       'dur': row['duration_ns'] / 1000,
                       'args': {'kernel': row['key'], 'event_id': row['event_id'],
                                'workgroups': row['workgroup_count'],
                                'workgroup_size': row['workgroup_size']}})
        total_ns += row['duration_ns']
    result = {'traceEvents': traces, 'displayTimeUnit': 'ms',
              'metadata': {'source': str(source), 'dispatch_count': len(events),
                           'gpu_duration_ns': total_ns,
                           'clock': 'GPU ticks relative to first dispatch; no host-clock alignment',
                           'limits': 'Compute dispatches only. Gaps can contain copies, host work, waits, or compilation.'}}
    output.write_text(json.dumps(result, separators=(',', ':')) + '\n')
    return result['metadata']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='iree-profile export --format=ireeperf-jsonl output')
    parser.add_argument('output', type=Path, help='Chrome Trace JSON for Perfetto')
    parser.add_argument('--kernels', type=Path, help='optional LSE kernel-times.json catalog')
    args = parser.parse_args()
    catalog = json.loads(args.kernels.read_text()) if args.kernels else None
    print(json.dumps(export(args.source, args.output, catalog), indent=2))
