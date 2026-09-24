import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location("cpu", Path(__file__).parents[1]/"scripts/rocprofmac-cpu.py")
cpu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cpu)

class CPUCommandTest(unittest.TestCase):
    def args(self, **changes):
        values = dict(seconds=30, attach=None, program=["/tmp/app name", "literal;$()"], env=[], output="/tmp/rocprofmac-unit-nonexistent.trace")
        values.update(changes)
        return SimpleNamespace(**values)
    def test_launch_preserves_argv(self):
        argv = cpu.command(self.args(env=["LSE_DIALECT=loom"]))
        self.assertEqual(argv[-4:], ["--launch", "--", "/tmp/app name", "literal;$()"])
        self.assertIn("LSE_DIALECT=loom", argv)
    def test_attach_only_pid(self):
        argv = cpu.command(self.args(attach=123, program=[]))
        self.assertEqual(argv[-2:], ["--attach", "123"])
        self.assertNotIn("--launch", argv)
    def test_invalid_bounds_and_ambiguous_target(self):
        for values in [dict(seconds=0), dict(seconds=3601), dict(attach=123), dict(program=[]), dict(output="/tmp/no.json"), dict(env=["invalid"]), dict(attach=123,program=[],env=["A=B"])]:
            with self.assertRaises(ValueError): cpu.command(self.args(**values))

if __name__ == "__main__": unittest.main()
