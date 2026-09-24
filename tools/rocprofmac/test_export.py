import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('export',Path(__file__).with_name('export.py'))
export=importlib.util.module_from_spec(spec);spec.loader.exec_module(export)
class ExportTest(unittest.TestCase):
    def records(self):
        rows=[dict(record_type='device',physical_device_ordinal=0,timestamp_frequency_hz_present=True,timestamp_frequency_hz=100000000),dict(record_type='session',event='end',session_status_code=0),dict(record_type='session',event='begin',session_status_code=0)]
        for i,(start,end) in enumerate([(100,200),(150,180),(230,260)]):
            rows.append(dict(record_type='dispatch_event',physical_device_ordinal=0,queue_ordinal=0,key='lse_loom_42',valid=True,start_tick=start,end_tick=end,event_id=i,submission_id=i,workgroup_count=[1,1,1],workgroup_size=[32,1,1]))
        return rows
    def test_overlap_does_not_invent_gap(self):
        trace,s=export.convert(self.records(),{'lse_loom_42':'matvec shape=128'})
        self.assertEqual(s['dispatch_count'],3);self.assertEqual(s['positive_queue_gap_count'],1)
        self.assertAlmostEqual(s['positive_queue_gap_sum_us'],.3)
        self.assertEqual(s['kernels'][0]['label'],'matvec shape=128')
        self.assertFalse(trace['clock_domains_correlated'])
    def test_incomplete_and_failed_capture(self):
        rows=self.records();rows[1]['session_status_code']=2
        with self.assertRaises(ValueError):export.convert(rows)
        with self.assertRaises(ValueError):export.convert([r for r in rows if r['record_type']!='session'])
        rows=self.records();rows.append(dict(record_type='session',event='begin',session_status_code=0,session_id=2))
        with self.assertRaises(ValueError):export.convert(rows)
    def test_invalid_timestamp_unknown_frequency(self):
        rows=self.records();rows[-1]['end_tick']=0
        with self.assertRaises(ValueError):export.convert(rows)
        rows=self.records();rows[0]['timestamp_frequency_hz_present']=False
        with self.assertRaises(ValueError):export.convert(rows)
    def test_join_label_without_changing_timing(self):
        labels=export.label_map('[dispatch-profile] count=32 lse_loom_42 anchor=quant_linear phase=0 grid=4')
        self.assertIn('anchor=quant_linear',labels['lse_loom_42'])
if __name__=='__main__':unittest.main()
