import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("route", ROOT / "scripts/route.py")
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)


def phase_log(rank, same_host=False):
    text = (f"DEVICE rank={rank} logical=0 physical=0 numa=0 dram_bytes=4194304 hbm_allocations=0\n"
            f"HOSTS same_host={int(same_host)}\n")
    if rank == 0:
        text += "DRIVER_QUERY src=0x100 dst=0x200 rc=0 raw=0 classification=INNER initiator=0\n"
    for phase, count in (("idle", 0), ("n", 4), ("2n", 8)):
        text += (f"PHASE_PASS phase={phase} rank={rank} copies={count} payload_bytes={count * 4096} "
                 "sample_start_ns=1000000000 payload_start_ns=2000000000 "
                 "payload_end_ns=3000000000 sample_stop_ns=4000000000 guards=PASS\n")
    return text + f"ROUTE_RANK_PASS rank={rank} cleanup=PASS\n"


class RouteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def csv(self, path, bad=False):
        output = path / "PROF_test/mindstudio_profiler_output"
        output.mkdir(parents=True)
        hccs = "Device_id,Mode,Max,Min,Average\n0,Tx (MB/s),20,1,10\n0,Rx (MB/s),20,1,10\n"
        (output / "hccs_1.csv").write_text(hccs.replace(",10\n", ",nan\n") if bad else hccs)
        rows = [f"0,{mode}(MB/s),0,10,5\n" for mode in (
            "Tx_p_avg", "Tx_np_avg", "Tx_cpl_avg", "Rx_p_avg", "Rx_np_avg", "Rx_cpl_avg")]
        (output / "pcie_1.csv").write_text("Device_id,Mode,Min,Max,Avg\n" + "".join(rows) +
                                          "0,Tx_latency_avg(us),0,1,0.5\n")

    def fixtures(self):
        directories = [self.root / str(rank) for rank in (0, 1)]
        for rank, directory in enumerate(directories):
            directory.mkdir()
            log = phase_log(rank)
            (directory / "rank.log").write_text(log)
            record = dict(status="PASS", returncode=0, rank=rank, hostname=f"node-{rank}",
                          config=dict(bytes=4096, iterations=4, run_id="test-run-1"),
                          session="a" * 64, device=0, numa=0, **route.endpoint_records(log, rank, 0, 0),
                          phases=route.phase_records(log, rank, 4, 4096))
            route.save(directory / "route.json", record)
            for phase in route.PHASES:
                self.csv(directory / phase)
            traffic = dict(session="a" * 64, phases={phase: route.read_metrics(directory / phase)
                           for phase in route.PHASES})
            route.save(directory / "traffic.json", traffic)
        return directories

    def test_window_and_counts(self):
        records = route.phase_records(phase_log(0), 0, 4, 4096)
        self.assertEqual(records["2n"]["payload_bytes"], 32768)
        self.assertEqual(records["n"]["sample_seconds"], 3)
        for log in (phase_log(0).replace("copies=8", "copies=7"),
                    phase_log(0).replace("payload_end_ns=3000000000", "payload_end_ns=5000000000"),
                    phase_log(0) + phase_log(0), phase_log(0).replace("cleanup=PASS", "cleanup=FAIL")):
            with self.assertRaises(ValueError):
                route.phase_records(log, 0, 4, 4096)

    def test_metrics_missing_is_not_zero(self):
        result = route.read_metrics(self.root)
        self.assertEqual(result["status"], "INCONCLUSIVE")
        self.assertEqual(result["metrics"], {})

    def test_metrics_units_and_latency(self):
        self.csv(self.root)
        result = route.read_metrics(self.root)
        self.assertEqual(result["status"], "AVAILABLE")
        self.assertEqual(len(result["metrics"]), 8)
        self.assertTrue(all(row["unit"] == "MB/s" for row in result["metrics"].values()))

    def test_metrics_nan_not_zero(self):
        self.csv(self.root, bad=True)
        self.assertEqual(route.read_metrics(self.root)["status"], "INCONCLUSIVE")

    def test_metric_modes_cannot_be_split_across_devices(self):
        self.csv(self.root)
        path = next(self.root.glob("PROF_*/mindstudio_profiler_output/hccs_*.csv"))
        path.write_text(path.read_text().replace("0,Rx", "1,Rx"))
        self.assertEqual(route.read_metrics(self.root)["status"], "INCONCLUSIVE")

    def test_metrics_must_belong_to_sampled_device(self):
        self.csv(self.root)
        self.assertEqual(route.read_metrics(self.root, 1)["status"], "INCONCLUSIVE")

    def test_two_host_data_and_traffic_separate(self):
        result = route.collect(self.fixtures())
        self.assertEqual(result["transfer_status"], "TWO_HOST_PASS")
        self.assertEqual(result["counter_status"], "AVAILABLE")
        self.assertEqual(result["route_conclusion"], "REQUIRES_ANALYSIS")
        self.assertEqual(len(result["ranks"][0]["differences"]), 16)

    def test_mixed_or_incomplete_run_rejected(self):
        directories = self.fixtures()
        original = json.loads((directories[1] / "route.json").read_text())
        for changes in (dict(session="b" * 64), dict(config=dict(bytes=8192)),
                        dict(rank=0), dict(returncode=1), dict(status="FAIL")):
            modified = copy.deepcopy(original)
            modified.update(changes)
            route.save(directories[1] / "route.json", modified)
            with self.assertRaises(ValueError):
                route.collect(directories)

    def test_same_host_never_reported_as_dual_host(self):
        directories = self.fixtures()
        for directory in directories:
            record = json.loads((directory / "route.json").read_text())
            record.update(hostname="same", same_host=True)
            route.save(directory / "route.json", record)
            log = directory / "rank.log"
            log.write_text(log.read_text().replace("same_host=0", "same_host=1"))
        with self.assertRaises(ValueError):
            route.collect(directories)
        self.assertEqual(route.collect(directories, True)["transfer_status"], "SINGLE_HOST_PASS")

    def test_stale_traffic_not_accepted(self):
        directories = self.fixtures()
        path = next((directories[0] / "n").glob("PROF_*/mindstudio_profiler_output/hccs_*.csv"))
        path.write_text(path.read_text().replace(",10\n", ",11\n"))
        with self.assertRaisesRegex(ValueError, "CSV/summary"):
            route.collect(directories)

    def test_stale_driver_or_device_evidence_not_accepted(self):
        directories = self.fixtures()
        path = directories[0] / "route.json"
        record = json.loads(path.read_text())
        record["driver_query"] = ["different query"]
        route.save(path, record)
        with self.assertRaisesRegex(ValueError, "log evidence"):
            route.collect(directories)


if __name__ == "__main__":
    unittest.main()
