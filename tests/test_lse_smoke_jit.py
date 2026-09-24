"""Host-only checks of benchmark attribution from cumulative HTTP counters."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / 'scripts/run-lse-server-smoke.py'
spec = importlib.util.spec_from_file_location('lse_smoke', path)
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


def totals(memory=10, disk=3, compiles=5, ms=7.25):
    return dict(zip(smoke.JIT_TOTAL_FIELDS, (memory, disk, compiles, ms)))


class JitObservability(unittest.TestCase):
    def test_first_snapshot_is_not_a_request_delta(self):
        first = smoke.jit_observation(totals())
        self.assertIsNone(first['delta'])
        self.assertIsNone(first['compiled'])

    def test_warm_and_compiling_requests(self):
        before = totals()
        warm = smoke.jit_observation(totals(memory=15), before)
        self.assertEqual(warm['delta']['jit_memory_hits'], 5)
        self.assertFalse(warm['compiled'])
        cold = smoke.jit_observation(totals(memory=15, compiles=7, ms=8.5), before)
        self.assertEqual(cold['delta']['jit_compiles'], 2)
        self.assertEqual(cold['delta']['jit_compile_ms'], 1.25)
        self.assertTrue(cold['compiled'])
        summary = smoke.jit_benchmark_summary([{'jit': warm}, {'jit': cold}])
        self.assertTrue(summary['measured_requests_compiled'])
        self.assertFalse(summary['all_measured_requests_compile_free'])

    def test_legacy_missing_fields_and_counter_reset_remain_unknown(self):
        missing = smoke.jit_observation({'prompt_ms': 1.5}, totals())
        reset = smoke.jit_observation(totals(compiles=0), totals())
        self.assertFalse(missing['available'])
        self.assertEqual(reset['reason'], 'counter_reset')
        summary = smoke.jit_benchmark_summary([{'jit': missing}, {'jit': reset}])
        self.assertIsNone(summary['measured_requests_compiled'])
        self.assertIsNone(summary['all_measured_requests_compile_free'])

    def test_compile_free_requires_all_measured_deltas(self):
        warm = smoke.jit_observation(totals(), totals())
        self.assertTrue(smoke.jit_benchmark_summary([{'jit': warm}])[
            'all_measured_requests_compile_free'])
        first = smoke.jit_observation(totals())
        self.assertIsNone(smoke.jit_benchmark_summary([{'jit': warm}, {'jit': first}])[
            'all_measured_requests_compile_free'])

    def test_invalid_counters_do_not_silently_claim_a_warm_cache(self):
        for field, bad in [('jit_compiles_total', -1), ('jit_compiles_total', 1.5),
                           ('jit_compile_ms_total', float('nan')),
                           ('jit_disk_hits_total', True)]:
            value = totals(); value[field] = bad
            with self.assertRaises(ValueError):
                smoke.jit_observation(value)


if __name__ == '__main__':
    unittest.main()
