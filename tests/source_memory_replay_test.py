"""Source provenance, live labels and native lifecycle replay contracts."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("source_replay", ROOT / "tools/source_memory_replay.py")
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)
BINARY = Path(sys.argv.pop()).resolve() if len(sys.argv) > 1 and not sys.argv[-1].startswith("-") else None


class Provenance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "corpus.json"
        self.blob = "Old policy.\nNew policy.\n"
        self.data = dict(schema="cogg:source-corpus/v1", sources=[dict(id="s", commit="a"*40,
            path="policy.md", sha256=hashlib.sha256(self.blob.encode()).hexdigest())],
            memories=[dict(key="old", logical_key="policy", text="Old policy.", source="s", span=[0,11], role="condition"),
                      dict(key="new", logical_key="policy", text="New policy.", source="s", span=[12,23], role="evidence")],
            queries=[dict(id="q", text="Which policy?", expected=["new"], forbidden=["old"])])

    def tearDown(self):
        self.temp.cleanup()

    def load(self):
        self.path.write_text(json.dumps(self.data))
        with patch.object(replay.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, self.blob.encode())) as call:
            result = replay.load_corpus(self.path, Path(self.temp.name))
            self.assertEqual(call.call_args.args[0][-1], "a"*40+":policy.md")
            return result

    def test_pinned_exact_spans_and_current_version(self):
        self.assertEqual(self.load()["queries"][0]["expected"], ["new"])
        self.data["queries"][0]["expected"] = ["old"]
        with self.assertRaisesRegex(ValueError, "inactive"):
            self.load()

    def test_blob_tampering(self):
        self.blob += "injected"
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.load()

    def test_excerpt_tampering(self):
        self.data["memories"][1]["text"] = "Invented policy."
        with self.assertRaisesRegex(ValueError, "excerpt"):
            self.load()

    def test_unpinned_commit(self):
        self.data["sources"][0]["commit"] = "HEAD"
        with self.assertRaisesRegex(ValueError, "unpinned"):
            self.load()

    def test_retraction_and_unknown_forbidden(self):
        self.data["memories"][1]["status"] = "retracted"
        with self.assertRaisesRegex(ValueError, "inactive"):
            self.load()
        self.data["queries"][0]["expected"] = []
        self.data["queries"][0]["forbidden"] = ["missing"]
        with self.assertRaisesRegex(ValueError, "unknown forbidden"):
            self.load()


@unittest.skipIf(BINARY is None, "pass cogg-neural-memory-eval to test native replay")
class NativeLifecycle(unittest.TestCase):
    def test_obsolete_and_retracted_never_return(self):
        # A constant-positive scorer deliberately accepts every candidate; Store,
        # not model behaviour, must suppress superseded/retracted sources.
        model = dict(schema="cogg:neural-memory/v1", features="token-alignment/v1",
                     encoder="test", dimension=1, threshold=0.25,
                     layers=[dict(weight=[[0.0]*16], bias=[0.0]),
                             dict(weight=[[0.0],[0.0]], bias=[0.0,0.0])])
        data = dict(memories=[dict(key="old", logical_key="p", text="old"),
                             dict(key="new", logical_key="p", text="new"),
                             dict(key="gone-old", logical_key="gone", text="gone-old"),
                             dict(key="gone", text="gone", status="retracted")],
                    queries=[dict(id="q", text="query", expected=["new"], forbidden=["old","gone-old","gone"])])
        payload = dict(schema="cogg:neural-memory-replay/v2", model=model, encoder="test",
                       dataset=data, encodings={s:[[1.0]] for s in ["old","new","gone-old","gone","query"]},
                       noise_count=0, cosine_threshold=0.55,
                       methods=["balanced","cosine","neural","cosine_topk","neural_topk"],
                       budgets=dict(max_items=4,max_bytes=3200,candidate_limit=64))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/"input.json"; path.write_text(json.dumps(payload))
            r = subprocess.run([str(BINARY),str(path)], check=True, text=True, capture_output=True)
            report = json.loads(r.stdout)
            self.assertTrue(report["store_rebuild_verify"])
            for method in report["strategies"]:
                self.assertEqual(method["rows"][0]["selected"], ["new"])
                self.assertEqual(method["forbidden_selected"], 0)
            payload["dataset"]["queries"][0]["expected"] = ["old"]
            path.write_text(json.dumps(payload))
            bad = subprocess.run([str(BINARY),str(path)], text=True, capture_output=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("inactive", bad.stderr)


if __name__ == "__main__":
    unittest.main()
