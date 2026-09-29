import json
from pathlib import Path
import tempfile
import unittest

from iree_to_perfetto import export


class TraceConversionTest(unittest.TestCase):
    def convert(self, dispatches, frequency=100_000_000):
        device = {
            'record_type': 'device', 'physical_device_ordinal': 0,
            'timestamp_frequency_hz_present': True,
            'timestamp_frequency_hz': frequency,
        }
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / 'in.jsonl', Path(directory) / 'out.json'
            source.write_text('\n'.join(json.dumps(x) for x in [device, *dispatches]))
            export(source, output)
            return json.loads(output.read_text())

    def event(self, start=9_000_000_000, end=9_000_000_125, **changes):
        row = {
            'record_type': 'dispatch_event', 'physical_device_ordinal': 0,
            'queue_ordinal': 0, 'valid': True, 'duration_scale_available': True,
            'start_tick': start, 'end_tick': end, 'duration_ns': (end - start) * 10,
            'key': 'kernel', 'event_id': 1,
            'workgroup_count': [2, 1, 1], 'workgroup_size': [128, 1, 1],
        }
        return row | changes

    def test_ticks_become_microseconds_without_absolute_clock_precision_loss(self):
        result = self.convert([
            self.event(),
            self.event(start=9_000_000_500, end=9_000_000_750, event_id=2, queue_ordinal=1),
        ])
        slices = [x for x in result['traceEvents'] if x['ph'] == 'X']
        self.assertEqual([(x['ts'], x['dur'], x['tid']) for x in slices],
                         [(0, 1.25, 0), (5, 2.5, 1)])
        self.assertEqual(result['metadata']['gpu_duration_ns'], 3750)
        self.assertEqual(slices[1]['args']['event_id'], 2)

    def test_invalid_timestamps_are_not_silently_dropped(self):
        for changes in ({'valid': False}, {'duration_scale_available': False},
                        {'duration_ns': 2000}, {'end_tick': 8_999_999_999}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.convert([self.event(**changes)])

    def test_unaligned_device_clocks_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'one device'):
            self.convert([self.event(), self.event(physical_device_ordinal=1)])

    def test_invalid_frequency_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'positive'):
            self.convert([self.event()], frequency=0)


if __name__ == '__main__':
    unittest.main()
