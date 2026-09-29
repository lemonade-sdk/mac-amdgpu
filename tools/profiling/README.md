# View GPU dispatch profiles in Perfetto

Use the existing HRX/IREE timestamp capture and [Perfetto](https://ui.perfetto.dev)
to inspect GPU kernel durations and dispatch gaps. The viewer runs in a browser
on macOS, Windows, and Linux.

First export an existing `.ireeprof` capture with the `iree-profile` tool from
the HRX build:

```sh
iree-profile export --format=ireeperf-jsonl --output=dispatches.jsonl capture.ireeprof
python3 tools/profiling/iree_to_perfetto.py dispatches.jsonl gpu-trace.json
```

An optional `--kernels kernel-times.json` adds LSE operation families and matrix
dimensions to the slice names. Without it, slice names are kernel entry names.
Each slice includes the original entry name, dispatch event ID, grid, and
workgroup size in its arguments.

In Perfetto, select **Open trace file** and select `gpu-trace.json`. Use **W/S**
to zoom and **A/D** to pan. Click a slice to inspect its arguments. Select an
area to compare the kernels in that interval. The **Query (SQL)** page can list
the largest total costs:

```sql
SELECT name, COUNT(*) AS calls, SUM(dur)/1e6 AS gpu_ms,
       AVG(dur)/1e3 AS mean_us
FROM slice
GROUP BY name
ORDER BY gpu_ms DESC;
```

## Timing limits

The converter reads the device timestamp frequency from the capture. It checks
each dispatch duration against the raw start and end ticks before exporting
microseconds in Chrome Trace JSON format. It rejects invalid dispatches and
captures from multiple devices, whose clocks have not been aligned.

Time zero is the first GPU dispatch. The trace contains compute dispatches;
CPU activity and copies are not included. A gap can contain CPU work, a copy,
compilation, or a wait. It cannot be attributed from this trace alone. Keep
CPU stack captures and application span summaries with the GPU capture.

Perfetto supports this format in its [external trace importer](https://perfetto.dev/docs/getting-started/other-formats).
Opening a local file processes the trace in the browser. Do not select a sharing
or upload action unless you intend to publish it.

## Check the converter

```sh
python3 -m unittest discover -s tools/profiling -p 'test_*.py'
```
