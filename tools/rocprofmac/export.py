#!/usr/bin/env python3
"""Convert iree-profile export JSONL into a clock-honest Chrome trace and summary."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re


def label_map(text):
    labels = {}
    for line in text.splitlines():
        match = re.search(r'(lse_loom_\d+)( anchor=.*)$', line)
        if match:
            labels[match[1]] = match[1] + match[2]
    return labels


def convert(records, labels=None):
    labels = labels or {}
    ends = [r for r in records if r.get('record_type') == 'session' and r.get('event') == 'end']
    begins = [r for r in records if r.get('record_type') == 'session' and r.get('event') == 'begin']
    session_key = lambda r: (r.get('session_id', 0), r.get('stream_id', 0))
    if (not ends or Counter(map(session_key, begins)) != Counter(map(session_key, ends))
            or any(r['session_status_code'] != 0 for r in ends)):
        raise ValueError('capture has no successful session end, or a failed session')
    if any(r.get('record_type') == 'diagnostic' and r.get('severity') == 'error' for r in records):
        raise ValueError('profile exporter reported a capture error')
    frequencies = {}
    for r in records:
        if r.get('record_type') == 'device' and r.get('timestamp_frequency_hz_present'):
            device, hz = r['physical_device_ordinal'], r['timestamp_frequency_hz']
            if not hz or (device in frequencies and frequencies[device] != hz):
                raise ValueError('missing or changing device clock frequency')
            frequencies[device] = hz
    dispatches = [r for r in records if r.get('record_type') == 'dispatch_event']
    if not dispatches:
        raise ValueError('capture contains no dispatch timestamps')
    origins = {}
    for r in dispatches:
        device = r['physical_device_ordinal']
        if device not in frequencies or not r['valid'] or not 0 < r['start_tick'] <= r['end_tick']:
            raise ValueError('invalid dispatch timestamp or unknown device frequency')
        origins[device] = min(origins.get(device, r['start_tick']), r['start_tick'])
    traces, groups, lanes = [], defaultdict(list), defaultdict(list)
    for r in dispatches:
        device, queue = r['physical_device_ordinal'], r['queue_ordinal']
        factor = 1e6 / frequencies[device]
        key = r['key']
        match = re.search(r'lse_loom_\d+', key)
        name = labels.get(match[0], key) if match else key
        duration = (r['end_tick'] - r['start_tick']) * factor
        groups[name].append(duration)
        lanes[(device, queue)].append((r['start_tick'], r['end_tick']))
        traces.append(dict(ph='X', name=name, cat='CP dispatch', pid=100+device,
                           tid=queue, ts=(r['start_tick']-origins[device])*factor,
                           dur=duration, args={k:r[k] for k in ['event_id','submission_id','workgroup_count','workgroup_size','start_tick','end_tick']}))
    gaps = []
    for (device, queue), intervals in lanes.items():
        last = None
        for start, end in sorted(intervals):
            if last is not None and start > last:
                duration = (start-last)*1e6/frequencies[device]
                gaps.append(duration)
                traces.append(dict(ph='X', name='No captured dispatch interval', cat='queue gap',
                                   pid=100+device, tid=queue, ts=(last-origins[device])*1e6/frequencies[device],
                                   dur=duration, args={'meaning':'May contain submission delay, transfer, harvest or other uncaptured work; not GPU idle proof'}))
            last = max(last or end, end)
    host = [r for r in records if r.get('record_type') == 'queue_event']
    host_origin = min((r['host_time_ns'] for r in host), default=0)
    for r in host:
        traces.append(dict(ph='i', s='t', name=r['op'], cat='host queue event',pid=1,
                           tid=r['queue_ordinal'], ts=(r['host_time_ns']-host_origin)/1000,
                           args={k:v for k,v in r.items() if k not in ['record_type','record_index','schema_version']}))
    summary = dict(dispatch_count=len(dispatches), host_queue_event_count=len(host),
                   diagnostics=[r for r in records if r.get("record_type") == "diagnostic"],
                   device_frequency_hz=frequencies, clocks_correlated=False,
                   positive_queue_gap_count=len(gaps), positive_queue_gap_sum_us=sum(gaps),
                   kernel_duration_sum_is_not_gpu_busy_time=True,
                   kernels=sorted([dict(label=k,count=len(v),sum_us=sum(v),mean_us=sum(v)/len(v),min_us=min(v),max_us=max(v)) for k,v in groups.items()],key=lambda r:r['sum_us'],reverse=True))
    trace = dict(displayTimeUnit='ns',clock_domains_correlated=False,
                 notes='Each GPU and the CPU have independent zero origins. Gaps include uncaptured work. No bandwidth/occupancy/counter claim. Profiling perturbation unmeasured.',
                 traceEvents=traces)
    return trace, summary


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('input',type=Path,help='iree-profile export --format=ireeperf-jsonl output')
    p.add_argument('--trace',required=True,type=Path)
    p.add_argument('--summary',required=True,type=Path)
    p.add_argument('--lse-log',type=Path,help='optional LSE_PROFILE_DISPATCH=submit log for operation/shape labels')
    a=p.parse_args()
    try:
        records=[json.loads(line) for line in a.input.read_text().splitlines() if line.strip()]
        labels=label_map(a.lse_log.read_text()) if a.lse_log else {}
        trace,summary=convert(records,labels)
        a.trace.write_text(json.dumps(trace,separators=(',',':'))+'\n')
        a.summary.write_text(json.dumps(summary,indent=2)+'\n')
    except (OSError,ValueError,KeyError,TypeError) as e:
        p.exit(1,f'rocprofmac export: {e}\n')
    print(f"rocprofmac: {summary['dispatch_count']} GPU dispatches, {summary['host_queue_event_count']} host queue events; independent clock origins")

if __name__=='__main__': main()
