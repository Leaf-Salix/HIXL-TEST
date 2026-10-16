import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("collect", ROOT / "scripts/collect.py")
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class CollectTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.config = json.loads((ROOT / "configs/two-node.json").read_text())
        for node in self.config["nodes"]:
            node["devices"] = [0]
        ranks = collector.runner.validate(self.config)
        self.directories = []
        for rank in ranks:
            directory = Path(self.temp.name) / str(rank["rank"])
            directory.mkdir()
            manifest = dict(status="PASS", returncodes=[0], config=self.config, world=2,
                            mode="baseline", allocator="vmm", ranks=[rank], run_id="a" * 32,
                            session=collector.runner.digest(self.config, "baseline", "vmm"),
                            hostname=rank["node"], build_info=dict(source_sha256={"src/matrix.cpp": "test-hash"}))
            (directory / "manifest.json").write_text(json.dumps(manifest))
            (directory / f"rank-{rank['rank']}.log").write_text(
                "RUN_ID=" + "a" * 32 + "\nBASELINE_PASS \nBASELINE_PASS \n" +
                f"RANK_PASS rank={rank['rank']} hixl_cases=0 kernel_cases=0 handoff_cases=0 cleanup=PASS\n")
            self.directories.append(directory)

    def change(self, directory, **changes):
        path = directory / "manifest.json"
        data = json.loads(path.read_text())
        data.update(changes)
        path.write_text(json.dumps(data))

    def test_global_pass(self):
        result = collector.collect(self.directories)
        self.assertEqual(result["status"], "GLOBAL_PASS")
        self.assertEqual(result["observed_hosts"], 2)

    def test_missing_rank_rejected(self):
        with self.assertRaises(ValueError):
            collector.collect(self.directories[:1])

    def test_duplicate_rank_rejected(self):
        with self.assertRaises(ValueError):
            collector.collect(self.directories + self.directories[:1])

    def test_different_execution_rejected(self):
        self.change(self.directories[1], run_id="b" * 32)
        with self.assertRaises(ValueError):
            collector.collect(self.directories)

    def test_same_host_not_multinode(self):
        self.change(self.directories[1], hostname="node-a")
        with self.assertRaises(ValueError):
            collector.collect(self.directories)

    def test_different_source_rejected(self):
        self.change(self.directories[1], build_info=dict(source_sha256={"src/matrix.cpp": "different"}))
        with self.assertRaises(ValueError):
            collector.collect(self.directories)

    def test_failure_rejected(self):
        self.change(self.directories[1], status="FAIL")
        with self.assertRaises(ValueError):
            collector.collect(self.directories)

    def enable_shared(self):
        model = collector.runner.shared_model(self.config)
        session = collector.runner.digest(self.config, "baseline", "vmm", "node-shared")
        for rank, directory in enumerate(self.directories):
            self.change(directory, layout="node-shared", model=model, session=session)
            (directory / f"rank-{rank}.log").write_text(
                "RUN_ID=" + "a" * 32 + "\n" +
                "SHARED_MODEL cpu_actors=2 npu_actors=2 windows=4 dram_allocations=2 hbm_allocations=2\n" +
                f"ALLOC_OK rank={rank} kind=DRAM address=example\n" +
                f"ALLOC_OK rank={rank} kind=HBM address=example\n" +
                "BASELINE_PASS \nBASELINE_PASS \n" +
                f"RANK_PASS rank={rank} hixl_cases=0 kernel_cases=0 handoff_cases=0 shared_cases=0 layout=node-shared cleanup=PASS\n")

    def test_shared_model_collects(self):
        self.enable_shared()
        result = collector.collect(self.directories)
        self.assertEqual(result["layout"], "node-shared")
        self.assertEqual(len(result["model"]["windows"]), 4)
        self.assertEqual(result["expected_global"]["shared_cases"], 0)

    def test_shared_manifest_must_match_real_plan(self):
        self.enable_shared()
        self.change(self.directories[1], model=dict(cpu_owners=[0], windows=[]))
        with self.assertRaises(ValueError):
            collector.collect(self.directories)
