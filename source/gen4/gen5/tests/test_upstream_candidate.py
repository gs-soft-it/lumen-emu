# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/PrepareUpstreamCandidate.py"
spec = importlib.util.spec_from_file_location("candidate", TOOL)
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)


class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        (self.source / "src").mkdir(parents=True)
        (self.source / "tests").mkdir()
        (self.source / "src/example.cpp").write_text("old\n")
        self.patch = self.root / "kyty.patch"
        self.patch.write_text("diff --git a/src/example.cpp b/src/example.cpp\n"
                              "--- a/src/example.cpp\n+++ b/src/example.cpp\n"
                              "@@ -1 +1 @@\n-old\n+new\n")
        self.manifest = self.root / "manifest.json"
        self.record = {
            "schema": "lumen.ps5.upstream-candidate.v1", "upstream_commit": "fixture",
            "patch_sha256": candidate.sha256(self.patch.read_bytes()),
            "files": [{"path": "src/example.cpp", "before_sha256": candidate.sha256(b"old\n"),
                       "after_sha256": candidate.sha256(b"new\n")}],
        }

    def prepare(self):
        self.manifest.write_text(json.dumps(self.record))
        return candidate.prepare(self.source, self.manifest, self.root / "output")

    def test_separate_cached_overlay_and_tamper_rejection(self):
        output = self.prepare()
        self.assertEqual((self.source / "src/example.cpp").read_text(), "old\n")
        self.assertEqual((output / "src/example.cpp").read_text(), "new\n")
        self.assertEqual(output, self.prepare())
        (output / "src/example.cpp").write_text("modified\n")
        with self.assertRaisesRegex(ValueError, "was modified"):
            self.prepare()

    def test_changed_baseline_is_rejected(self):
        (self.source / "src/example.cpp").write_text("user edit\n")
        with self.assertRaisesRegex(ValueError, "Baseline changed"):
            self.prepare()

    def test_unreviewed_patch_output_is_rejected(self):
        self.record["files"][0]["after_sha256"] = candidate.sha256(b"wrong\n")
        with self.assertRaisesRegex(ValueError, "complete reviewed inventory"):
            self.prepare()

    def test_path_escape_is_rejected(self):
        self.record["files"][0]["path"] = "src/../../outside"
        with self.assertRaisesRegex(ValueError, "Invalid update path"):
            self.prepare()


if __name__ == "__main__":
    unittest.main()
