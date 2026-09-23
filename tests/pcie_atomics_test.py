import importlib.util
from pathlib import Path
import unittest
import sys
import json
import plistlib
import subprocess
import tempfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("pcie_atomics", Path(__file__).resolve().parents[1] / "scripts/check-pcie-atomics.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def node(kind, caps, control=0, **extra):
    return {"IORegistryEntryName": "device", "IOPCIExpressCapabilities": (kind << 4) | 2,
            "IOPCIExpressDeviceCapabilities2": caps,
            "IOPCIExpressDeviceControl2": control, **extra}


def good_path():
    return [node(4, 0x180), node(5, 0x40), node(6, 0x40), node(0, 0, 0x40)]


class AtomicCapabilities(unittest.TestCase):
    def test_root_and_every_bridge_required(self):
        good = good_path()
        self.assertEqual(module.assess(good)["result"], "advertised")
        for index in range(3):
            bad = [dict(entry) for entry in good]
            bad[index]["IOPCIExpressDeviceCapabilities2"] = 0
            report = module.assess(bad)
            self.assertEqual(report["result"], "missing")
            self.assertFalse(report["unknown"])

    def test_endpoint_cannot_substitute_for_root(self):
        report = module.assess([node(0, 0x380, 0x40)])
        self.assertEqual(report["result"], "unknown")
        self.assertFalse(report["missing"])
        report = module.assess([node(4, 0x80), node(0, 0x380, 0x40)])
        self.assertEqual(report["result"], "missing")
        self.assertIn("64-bit", report["missing"][0])

    def test_missing_register_is_unknown_not_absent(self):
        path = good_path()
        path[1].pop("IOPCIExpressDeviceCapabilities2")
        path[-1].pop("IOPCIExpressDeviceControl2")
        report = module.assess(path)
        self.assertEqual(report["result"], "unknown")
        self.assertFalse(report["missing"])
        self.assertEqual(len(report["unknown"]), 2)

    def test_requester_and_upstream_egress(self):
        path = good_path()
        path[-1]["IOPCIExpressDeviceControl2"] = 0
        self.assertIn("requester enable", module.assess(path)["missing"][0])
        path = good_path()
        path[1]["IOPCIExpressDeviceControl2"] = 0x80
        self.assertIn("egress unblocked", module.assess(path)["missing"][0])
        path = good_path()
        path[2]["IOPCIExpressDeviceControl2"] = 0x80
        self.assertEqual(module.assess(path)["result"], "advertised")

    def test_128_bit_completion_separate_and_optional(self):
        path = good_path()
        self.assertFalse(module.assess(path)["rows"][0]["completer128"])
        self.assertEqual(module.assess(path, (128,))["result"], "missing")
        path[0]["IOPCIExpressDeviceCapabilities2"] |= 0x200
        self.assertEqual(module.assess(path, (32, 64, 128))["result"], "advertised")

    def test_multi_gpu_ancestry_not_sibling_capabilities(self):
        first = node(0, 0x180, 0x40, **{"vendor-id": b"\x02\x10\0\0", "device-id": b"\x51\x75\0\0"})
        tree = [node(4, 0, IORegistryEntryChildren=[first]),
                node(4, 0x180, IORegistryEntryChildren=[dict(first)])]
        paths = list(module.gpu_paths(tree, 0x1002, 0x7551))
        self.assertEqual(len(paths), 2)
        self.assertEqual(module.assess(paths[0])["result"], "missing")
        self.assertEqual(module.assess(paths[1])["result"], "advertised")

    def test_pci_function_without_type_cannot_be_skipped(self):
        path = good_path()
        path.insert(1, {"vendor-id": b"\x86\x80\0\0", "IORegistryEntryName": "tunnel"})
        self.assertEqual(module.assess(path)["result"], "unknown")

    def test_saved_archive_json_and_exit_codes(self):
        for expected, control, root_caps in ((0, 0x40, 0x180), (1, 0, 0x180), (2, None, 0x180)):
            endpoint = node(0, 0, control, **{"vendor-id": 0x1002, "device-id": 0x7551})
            if control is None:
                endpoint.pop("IOPCIExpressDeviceControl2")
            tree = [node(4, root_caps, IORegistryEntryChildren=[endpoint])]
            with tempfile.NamedTemporaryFile(suffix=".plist") as archive:
                plistlib.dump(tree, archive)
                archive.flush()
                result = subprocess.run([sys.executable, spec.origin, "--ioreg-plist", archive.name, "--json"],
                                        text=True, capture_output=True)
            self.assertEqual(result.returncode, expected, result.stderr)
            report = json.loads(result.stdout)
            self.assertTrue(report["cached_only"])
            self.assertEqual(len(report["devices"]), 1)

    def test_cached_actual_path_and_byte_decoding(self):
        path = [node(4, 0xc1f), node(5, 0x10800, None), node(6, 0x330840), node(0, 0x73099f, None)]
        report = module.assess(path)
        self.assertEqual(len(report["missing"]), 3)
        self.assertEqual(len(report["unknown"]), 2)
        self.assertTrue(report["rows"][-1]["completer32"])
        self.assertTrue(report["rows"][-1]["completer64"])
        self.assertEqual(module.number(b"\x40\0"), 0x40)
        self.assertIsNone(module.number(b""))
        self.assertIsNone(module.number(True))


if __name__ == "__main__":
    unittest.main()
