import argparse
import copy
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runner_shared", ROOT / "scripts/run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class NodeSharedTests(unittest.TestCase):
    def setUp(self):
        self.config = json.loads((ROOT / "configs/two-node.json").read_text())

    def test_two_nodes_have_six_unique_locations(self):
        model = runner.shared_model(self.config)
        self.assertEqual(model["cpu_owners"], [0, 2])
        self.assertEqual(model["npu_actors"], [0, 1, 2, 3])
        self.assertEqual(model["windows"], [dict(owner=0, kind="DRAM"), dict(owner=0, kind="HBM"),
                                           dict(owner=1, kind="HBM"), dict(owner=2, kind="DRAM"),
                                           dict(owner=2, kind="HBM"), dict(owner=3, kind="HBM")])
        identities = {(entry["owner"], entry["kind"]) for entry in model["windows"]}
        self.assertEqual(len(identities), 6)

    def test_single_node_has_one_dram_two_hbm(self):
        config = copy.deepcopy(self.config)
        config["nodes"] = config["nodes"][:1]
        model = runner.shared_model(config)
        self.assertEqual(len(model["windows"]), 3)
        self.assertEqual(model["cpu_owners"], [0])
        self.assertEqual(runner.counts(2, "combined", "node-shared", 1),
                         dict(hixl_cases=24, kernel_cases=24, handoff_cases=12, shared_cases=4))

    def test_full_actor_matrix_counts(self):
        # 2 CPU actors x 6 locations x READ/WRITE x two completion requests x two sizes.
        self.assertEqual(runner.counts(4, "hixl", "node-shared", 2)["hixl_cases"], 96)
        # 4 real NPU actors x 6 locations x READ/WRITE x two sizes.
        self.assertEqual(runner.counts(4, "kernel", "node-shared", 2),
                         dict(hixl_cases=0, kernel_cases=96, handoff_cases=48, shared_cases=8))
        self.assertEqual(runner.counts(4, "combined", "node-shared", 2),
                         dict(hixl_cases=96, kernel_cases=96, handoff_cases=80, shared_cases=8))

    def test_siblings_receive_same_dram_owner(self):
        ranks = runner.validate(self.config)
        args = argparse.Namespace(mode="combined", allocator="vmm", control_timeout=60, layout="node-shared")
        owners = []
        for rank in ranks:
            command = runner.command(rank, self.config, 4, args)
            owners.append(command[command.index("--node-owner") + 1])
        self.assertEqual(owners, ["0", "0", "2", "2"])
        self.assertNotEqual(runner.digest(self.config, "combined", "vmm", "node-shared"),
                            runner.digest(self.config, "combined", "vmm"))

    def test_reject_slot_layout_over_capacity(self):
        self.config["nodes"][0]["devices"] = list(range(17))
        with self.assertRaises(ValueError):
            runner.shared_model(self.config)

    def shared_log(self, rank):
        model = runner.shared_model(self.config)
        owner = rank in model["cpu_owners"]
        lines = ["RUN_ID=" + "a" * 32,
                 "SHARED_MODEL cpu_actors=2 npu_actors=4 windows=6 dram_allocations=2 hbm_allocations=4"]
        lines += [f"ALLOC_OK rank={rank} kind=HBM address=example"]
        if owner:
            lines += [f"ALLOC_OK rank={rank} kind=DRAM address=example"]
        for label, number in dict(BASELINE_PASS=2, HOST_CASE_PASS=48 if owner else 0,
                                  NODE_KERNEL_CASE_PASS=24, NODE_HANDOFF_PASS=80 if rank == 0 else 0,
                                  SHARED_GROUP_PASS=8 if rank == 0 else 0, SHARED_DRAM_VIEW=2,
                                  LIFECYCLE_ACQUIRE_PASS=2 if owner else 1, LIFECYCLE_BUSY_PASS=1,
                                  LIFECYCLE_RELEASE_PASS=2 if owner else 1).items():
            lines += [label + " "] * number
        lines += [f"RANK_PASS rank={rank} hixl_cases=96 kernel_cases=96 handoff_cases=80 shared_cases=8 layout=node-shared cleanup=PASS"]
        return "\n".join(lines)

    def test_log_requires_actual_allocation_count(self):
        for rank in range(4):
            text = self.shared_log(rank)
            runner.check_log(text, rank, 4, "combined", "node-shared", self.config)
            with self.assertRaises(ValueError):
                runner.check_log(text + f"\nALLOC_OK rank={rank} kind=DRAM address=second",
                                 rank, 4, "combined", "node-shared", self.config)

    def test_missing_shared_or_handoff_round_rejected(self):
        text = self.shared_log(0)
        for label in ("SHARED_GROUP_PASS ", "NODE_HANDOFF_PASS ", "SHARED_DRAM_VIEW "):
            with self.assertRaises(ValueError):
                runner.check_log(text.replace(label + "\n", "", 1), 0, 4, "combined", "node-shared", self.config)
