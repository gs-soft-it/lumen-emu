# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/PrepareCompatibilitySources.py"
spec = importlib.util.spec_from_file_location("compatibility", TOOL)
compatibility = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compatibility)


class CompatibilitySourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        for subtree in ("engine/src", "engine/tests", "compatibility/src", "compatibility/tests"):
            (self.source / subtree).mkdir(parents=True)
        (self.source / "engine/src/common.cpp").write_text("common\n")
        (self.source / "engine/src/direct-only.cpp").write_text("direct\n")
        (self.source / "engine/tests/common.cpp").write_text("test\n")
        (self.source / "compatibility/src/facade.cpp").write_text("facade\n")
        self.manifest = {"schema": compatibility.SCHEMA,
                         "shared_files": ["src/common.cpp", "tests/common.cpp"]}

    def prepare(self):
        (self.source / "compatibility/shared-sources.json").write_text(json.dumps(self.manifest))
        return compatibility.prepare(self.source, self.root / "output")

    def test_complete_view_excludes_direct_only_units_and_keeps_sparse_source(self):
        output = self.prepare()
        self.assertEqual(set(compatibility.inventory(output)),
                         {"src/common.cpp", "src/facade.cpp", "tests/common.cpp"})
        self.assertFalse((self.source / "compatibility/src/common.cpp").exists())
        self.assertEqual(output, self.prepare())
        (self.source / "engine/src/common.cpp").write_text("updated\n")
        updated = self.prepare()
        self.assertNotEqual(updated, output)
        self.assertEqual((updated / "src/common.cpp").read_text(), "updated\n")
        self.assertEqual((output / "src/common.cpp").read_text(), "common\n")

    def test_modified_generated_source_is_rejected(self):
        output = self.prepare()
        (output / "src/common.cpp").write_text("tampered\n")
        with self.assertRaisesRegex(ValueError, "were modified"):
            self.prepare()

    def test_duplicate_and_missing_shared_sources_are_rejected(self):
        duplicate = self.source / "compatibility/src/common.cpp"
        duplicate.write_text("ambiguous\n")
        with self.assertRaisesRegex(ValueError, "Duplicate shared source"):
            self.prepare()
        duplicate.unlink()
        (self.source / "engine/src/common.cpp").unlink()
        with self.assertRaisesRegex(ValueError, "Missing regular shared source"):
            self.prepare()

    def test_escape_and_generated_output_in_source_are_rejected(self):
        self.manifest["shared_files"] = ["src/../../outside"]
        with self.assertRaisesRegex(ValueError, "Invalid shared source path"):
            self.prepare()
        with self.assertRaisesRegex(ValueError, "outside the source tree"):
            compatibility.prepare(self.source, self.source / "generated")


if __name__ == "__main__":
    unittest.main()
