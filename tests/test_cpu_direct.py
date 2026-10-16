import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cpu_direct", ROOT / "scripts/cpu_direct.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def log(rank, unsupported=False, local_ready=True):
    text = (f"DEVICE rank={rank} logical=0 physical=0 numa=0\nHOSTS same_host=0\n"
            f"HOST_ACCESS rank={rank} " + ("set_rc=0 get_rc=0 flags=3 ready=1\n" if local_ready else
                                           "set_rc=207000 get_rc=-1 flags=0 ready=0\n"))
    if not unsupported:
        text += "CPU_IO_BEGIN streams=0 memcpy_calls=0 kernel_launches=0 fallback=NONE\n"
        for actor in (0, 1):
            for operation in ("READ", "WRITE"):
                text += f"CPU_CASE_PASS actor={actor} operation={operation} bytes=65536 guards=PASS\n"
    return text + f"CPU_DIRECT_RESULT rank={rank} status={'UNSUPPORTED' if unsupported else 'PASS'} cleanup=PASS fallback=NONE\n"


class CpuDirectTests(unittest.TestCase):
    def test_pass_needs_both_actor_reads_and_writes(self):
        result = probe.evidence(log(0), 0, 0, 0, 0)
        self.assertEqual(result["status"], "PASS")
        for changed in (log(0).replace("actor=1 operation=READ", "actor=0 operation=READ"),
                        log(0).replace("flags=3 ready=1", "flags=0 ready=1"),
                        log(0).replace("cleanup=PASS", "cleanup=FAIL"),
                        log(0).replace("numa=0", "numa=1")):
            with self.assertRaises(ValueError):
                probe.evidence(changed, 0, 0, 0, 0)

    def test_unsupported_never_payload_or_success(self):
        text = log(0, True, False)
        self.assertEqual(probe.evidence(text, 0, 0, 0, 3)["status"], "UNSUPPORTED")
        for changed, code in ((text, 0), (text.replace("207000", "107000"), 3),
                              (text + "CPU_IO_BEGIN streams=0 memcpy_calls=0 kernel_launches=0 fallback=NONE\n", 3)):
            with self.assertRaises(ValueError):
                probe.evidence(changed, 0, 0, 0, code)

    def test_fault_cannot_pass(self):
        with self.assertRaises(ValueError):
            probe.evidence(log(0) + "CPU_DIRECT_ACCESS_FAULT\n", 0, 0, 0, 0)

    def fixtures(self, root, unsupported=False):
        directories = []
        for rank in (0, 1):
            directory = root / str(rank)
            directory.mkdir()
            text = log(rank, unsupported, rank == 0 or not unsupported)
            code = 3 if unsupported else 0
            record = dict(rank=rank, hostname=f"host-{rank}", device=0, numa=0, config=dict(run_id="test-run-1"),
                          session="a" * 64, returncode=code, **probe.evidence(text, rank, 0, 0, code))
            probe.save(directory / "cpu-direct.json", record)
            (directory / "rank.log").write_text(text)
            directories.append(directory)
        return directories

    def test_collect_pass_and_unsupported_are_distinct(self):
        for unsupported in (False, True):
            with tempfile.TemporaryDirectory() as temp:
                directories = self.fixtures(Path(temp), unsupported)
                result = probe.collect(directories)
                self.assertEqual(result["status"], "UNSUPPORTED" if unsupported else "TWO_HOST_CPU_DIRECT_PASS")
                self.assertEqual(result["hccs_route"], "NOT_MEASURED")

    def test_stale_record_and_duplicate_rank_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            directories = self.fixtures(Path(temp))
            with self.assertRaises(ValueError):
                probe.collect([directories[0], directories[0]])
            path = directories[0] / "cpu-direct.json"
            record = json.loads(path.read_text())
            record["physical_device"] = 1
            probe.save(path, record)
            with self.assertRaises(ValueError):
                probe.collect(directories)

    def test_same_host_is_only_control(self):
        with tempfile.TemporaryDirectory() as temp:
            directories = self.fixtures(Path(temp))
            for directory in directories:
                path = directory / "cpu-direct.json"
                record = json.loads(path.read_text())
                record.update(hostname="same", same_host=True)
                probe.save(path, record)
                path = directory / "rank.log"
                path.write_text(path.read_text().replace("same_host=0", "same_host=1"))
            with self.assertRaises(ValueError):
                probe.collect(directories)
            self.assertEqual(probe.collect(directories, True)["status"], "SINGLE_HOST_CPU_PASS")


if __name__ == "__main__":
    unittest.main()
