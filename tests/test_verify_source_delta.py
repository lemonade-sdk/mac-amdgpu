#!/usr/bin/env python3
import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("verify_delta", ROOT / "scripts/verify-source-delta.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
PATCH = """diff --git a/src/code.cpp b/src/code.cpp
--- a/src/code.cpp
+++ b/src/code.cpp
@@ -1 +1 @@
-old_call();
+new_call();
"""


class VerifySourceDelta(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.base, self.candidate = root / "base", root / "nested/candidate"
        for tree in [self.base, self.candidate]:
            (tree / "src").mkdir(parents=True)
            (tree / "src/code.cpp").write_text("old_call();\n")
        self.patch = root / "change.patch"
        self.patch.write_text(PATCH)

    def verify(self, extra=None):
        return module.verify(self.base, self.candidate, self.patch, extra)

    def test_applied_delta_and_unchanged_guard(self):
        (self.candidate / "src/code.cpp").write_text("new_call();\n")
        for tree in [self.base, self.candidate]:
            (tree / "src/guard.cpp").write_text("strict_guard();\n")
        self.assertEqual(len(self.verify(["src/guard.cpp"])["verified_files"]), 2)
        self.assertEqual((self.base / "src/code.cpp").read_text(), "old_call();\n")

    def test_skipped_patch_is_not_success(self):
        with self.assertRaisesRegex(ValueError, "candidate differs"):
            self.verify()

    def test_unrelated_change_in_checked_file_is_rejected(self):
        (self.candidate / "src/code.cpp").write_text("new_call();\nremoved_guard();\n")
        with self.assertRaisesRegex(ValueError, "candidate differs"):
            self.verify()

    def test_new_and_removed_files(self):
        self.patch.write_text("""diff --git a/src/new.cpp b/src/new.cpp
new file mode 100644
--- /dev/null
+++ b/src/new.cpp
@@ -0,0 +1 @@
+new();
diff --git a/src/code.cpp b/src/code.cpp
deleted file mode 100644
--- a/src/code.cpp
+++ /dev/null
@@ -1 +0,0 @@
-old_call();
""")
        (self.candidate / "src/code.cpp").unlink()
        (self.candidate / "src/new.cpp").write_text("new();\n")
        self.assertEqual(sum(x["exists"] for x in self.verify()["verified_files"]), 1)

    def test_symlink_candidate_is_rejected(self):
        target = self.candidate / "src/code.cpp"
        target.unlink()
        target.symlink_to(self.base / "src/code.cpp")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
