#!/usr/bin/env python3
"""Host-only precision/tie policy tests; never imports MLX or loads weights."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest

SCRIPT = Path(__file__).with_name('lse-mlx-reference.py')
spec = importlib.util.spec_from_file_location('reference', SCRIPT)
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


class FakeMx:
    float32, bfloat16, uint32, floating = 'float32', 'bfloat16', 'uint32', 'floating'

    @staticmethod
    def issubdtype(dtype, category):
        return category == FakeMx.floating and dtype in (FakeMx.float32, FakeMx.bfloat16)


class Array:
    def __init__(self, dtype, shape=(2, 4)):
        self.dtype, self.shape = dtype, shape

    def astype(self, dtype):
        return Array(dtype, self.shape)


class Model:
    bits, group_size, mode = 6, 64, 'affine'

    def __init__(self):
        self.values = {'linear.weight': Array(FakeMx.uint32),
                       'linear.scales': Array(FakeMx.bfloat16),
                       'linear.biases': Array(FakeMx.bfloat16),
                       'norm.weight': Array(FakeMx.float32)}
        self.corrupt_integer = self.corrupt_metadata = False

    def parameters(self):
        return self.values

    def named_modules(self):
        return [('linear', self)]

    def set_dtype(self, dtype, predicate):
        self.values = {name: value.astype(dtype) if predicate(value.dtype) else value
                       for name, value in self.values.items()}
        if self.corrupt_integer:
            self.values['linear.weight'] = Array(FakeMx.uint32)
        if self.corrupt_metadata:
            self.bits = 8


def configure(model, precision):
    return reference.configure_precision(model, FakeMx, lambda tree: list(tree.items()), precision)


class ReferenceTests(unittest.TestCase):
    def test_float32_preserves_packed_objects_and_quantization(self):
        model = Model()
        packed = model.values['linear.weight']
        audit = configure(model, 'float32')
        self.assertIs(model.values['linear.weight'], packed)
        self.assertEqual(model.values['linear.scales'].dtype, FakeMx.float32)
        self.assertEqual(model.values['linear.biases'].dtype, FakeMx.float32)
        self.assertTrue(audit['integer_object_identity_preserved'])
        self.assertTrue(audit['quantization_metadata_preserved'])
        self.assertEqual(audit['packed_uint32_weight_tensors'], 1)
        self.assertEqual(audit['parameter_dtype_counts_after'], {'uint32': 1, 'float32': 3})

    def test_native_preserves_every_parameter(self):
        model = Model()
        original = model.values.copy()
        audit = configure(model, 'native')
        self.assertTrue(all(model.values[name] is value for name, value in original.items()))
        self.assertEqual(audit['parameter_dtype_counts_before'], audit['parameter_dtype_counts_after'])

    def test_integer_or_quantization_mutation_is_rejected(self):
        for failure in ('corrupt_integer', 'corrupt_metadata'):
            model = Model()
            setattr(model, failure, True)
            with self.assertRaises(ValueError):
                configure(model, 'float32')

    def test_exact_tie_reports_both_ids_in_deterministic_order(self):
        values = [-10.0] * 6000
        values[4725] = values[5440] = 18.375
        ranked = reference.ranked_logits(values)
        self.assertEqual(ranked['top10_ids'][:2], [4725, 5440])
        self.assertEqual(ranked['maximum_tie_ids'], [4725, 5440])
        self.assertEqual(ranked['maximum_tie_count'], 2)
        self.assertEqual(ranked['top1_top2_margin'], 0)

    def test_nonfinite_logits_rejected(self):
        for values in ([], [float('nan')], [1.0, float('inf')]):
            with self.assertRaises(ValueError):
                reference.ranked_logits(values)

    def test_precision_selection_without_run_does_not_load_model(self):
        completed = subprocess.run([sys.executable, str(SCRIPT), '--precision', 'float32'],
                                   text=True, capture_output=True, timeout=10)
        self.assertEqual(completed.returncode, 2)
        self.assertIn('No model loaded', completed.stdout)
        self.assertNotIn('mlx.core', sys.modules)


if __name__ == '__main__':
    unittest.main()
