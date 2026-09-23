import importlib.util
from pathlib import Path
import unittest
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("pcie_atomics", Path(__file__).resolve().parents[1] / "scripts/check-pcie-atomics.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def node(kind, caps, **extra):
    return {"IORegistryEntryName": "device", "IOPCIExpressCapabilities": kind << 4,
            "IOPCIExpressDeviceCapabilities2": caps, **extra}


class AtomicCapabilities(unittest.TestCase):
    def test_root_and_every_bridge_required(self):
        good = [node(4, 0x180), node(5, 0x40), node(6, 0x40), node(0, 0)]
        self.assertFalse(module.assess(good)[1])
        for index in range(3):
            bad = [dict(entry) for entry in good]
            bad[index]["IOPCIExpressDeviceCapabilities2"] = 0
            self.assertEqual(len(module.assess(bad)[1]), 1)

    def test_endpoint_cannot_substitute_for_root(self):
        self.assertTrue(module.assess([node(0, 0x180)])[1])
        self.assertTrue(module.assess([node(4, 0x80), node(0, 0x180)])[1])

    def test_missing_capabilities_fail_closed(self):
        self.assertTrue(module.assess([node(4, None)])[1])
        self.assertTrue(module.assess([node(4, 0x180), node(5, None)])[1])

    def test_multi_gpu_ancestry_not_sibling_capabilities(self):
        first = node(0, 0x180, **{"vendor-id": b"\x02\x10\0\0", "device-id": b"\x51\x75\0\0"})
        second = dict(first)
        tree = [node(4, 0, IORegistryEntryChildren=[first]), node(4, 0x180, IORegistryEntryChildren=[second])]
        paths = list(module.gpu_paths(tree, 0x1002, 0x7551))
        self.assertEqual(len(paths), 2)
        self.assertTrue(module.assess(paths[0])[1])
        self.assertFalse(module.assess(paths[1])[1])


if __name__ == "__main__":
    unittest.main()
