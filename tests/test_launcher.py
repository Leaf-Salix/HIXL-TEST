import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("launcher", ROOT / "scripts/run.py")
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.config = json.loads((ROOT / "configs/two-node.json").read_text())

    def test_rank_mapping_and_counts(self):
        ranks = launcher.validate(self.config)
        self.assertEqual([(r["node"], r["device"]) for r in ranks],
                         [("node-a", 0), ("node-a", 1), ("node-b", 0), ("node-b", 1)])
        self.assertEqual(launcher.counts(2, "combined"), dict(hixl_cases=64, kernel_cases=48, handoff_cases=8))
        self.assertEqual(launcher.counts(4, "combined"), dict(hixl_cases=384, kernel_cases=448, handoff_cases=48))

    def test_invalid_configs(self):
        bad = []
        duplicate = copy.deepcopy(self.config)
        duplicate["nodes"][0]["devices"] = [0, 0]
        bad.append(duplicate)
        conflict = copy.deepcopy(self.config)
        conflict["coordinator"] = "198.51.100.10:39100"
        bad.append(conflict)
        wrong_host = copy.deepcopy(self.config)
        wrong_host["coordinator"] = "198.51.100.99:39000"
        bad.append(wrong_host)
        bad_numa = copy.deepcopy(self.config)
        bad_numa["nodes"][0]["numa_ids"] = [0]
        bad.append(bad_numa)
        loopback = copy.deepcopy(self.config)
        loopback["nodes"][1]["ip"] = "127.0.0.1"
        bad.append(loopback)
        for config in bad:
            with self.subTest(config=config), self.assertRaises(ValueError):
                launcher.validate(config)

    def test_consistent_config_digest(self):
        reordered = dict(reversed(list(self.config.items())))
        self.assertEqual(launcher.digest(self.config, "hixl", "vmm"), launcher.digest(reordered, "hixl", "vmm"))
        self.assertNotEqual(launcher.digest(self.config, "hixl", "vmm"), launcher.digest(self.config, "kernel", "vmm"))

    def test_success_requires_every_expected_case(self):
        world, mode, rank = 2, "combined", 0
        lines = ["BASELINE_PASS "] * 2 + ["CASE_PASS "] * 32 + ["KERNEL_CASE_PASS "] * 24
        lines += ["RUN_ID=" + "a" * 32]
        lines += ["HANDOFF_PASS "] * 4 + ["LIFECYCLE_ACQUIRE_PASS "] * 2
        lines += ["LIFECYCLE_BUSY_PASS "] + ["LIFECYCLE_RELEASE_PASS "] * 2
        final = "RANK_PASS rank=0 hixl_cases=64 kernel_cases=48 handoff_cases=8 cleanup=PASS"
        launcher.check_log("\n".join(lines + [final]), rank, world, mode)
        with self.assertRaises(ValueError):
            launcher.check_log("\n".join(lines[1:] + [final]), rank, world, mode)
        with self.assertRaises(ValueError):
            launcher.check_log("\n".join(lines + [final, "CLEANUP_FAIL"]), rank, world, mode)

    def test_scheduled_visibility_mapping(self):
        with patch.dict(os.environ, {"ASCEND_RT_VISIBLE_DEVICES": "2,4"}), patch.object(launcher, "launch", return_value=0) as launch:
            launcher.main(["single", "--scheduled-devices", "2,4", "--dry-run", "--port", "39000"])
        self.assertEqual([rank["device"] for rank in launch.call_args.args[1]], [0, 1])

    def test_mpi_rank_mapping(self):
        with patch.dict(os.environ, {"OMPI_COMM_WORLD_RANK": "2", "OMPI_COMM_WORLD_SIZE": "4"}), patch.object(launcher, "launch", return_value=0) as launch:
            launcher.main(["mpi-rank", "--config", str(ROOT / "configs/two-node.json"), "--dry-run"])
        self.assertEqual(launch.call_args.args[1][0]["node"], "node-b")

    def test_sigterm_reaps_child(self):
        # Exercise the actual signal handler and launch finally path with a sleeping fake binary.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "build").mkdir()
            (root / "build/matrix").write_text("placeholder")
            (root / "build/build-info.json").write_text("{}")
            pid_file = root / "child.pid"
            driver = root / "driver.py"
            driver.write_text(f'''import importlib.util, pathlib, subprocess, sys
spec=importlib.util.spec_from_file_location("run", {str(ROOT / 'scripts/run.py')!r})
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.ROOT=pathlib.Path({temp!r})
original=m.subprocess.run
m.subprocess.run=lambda *a,**k: subprocess.CompletedProcess(a,0,stdout="linked")
m.command=lambda *a: [sys.executable,"-c", "import os,time,pathlib; pathlib.Path({str(pid_file)!r}).write_text(str(os.getpid())); time.sleep(60)"]
try: m.main(["single","--devices","0","--mode","baseline","--output",{str(root/'result')!r}])
except RuntimeError: sys.exit(1)
''')
            process = subprocess.Popen([sys.executable, str(driver)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 10
                while not pid_file.exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertTrue(pid_file.exists())
                child_pid = int(pid_file.read_text())
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=10)
                self.assertNotEqual(process.returncode, 0)
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)
                self.assertEqual(json.loads((root / "result/manifest.json").read_text())["status"], "FAIL")
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate()
