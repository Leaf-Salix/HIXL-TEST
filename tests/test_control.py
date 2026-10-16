import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = str(Path(cls.temp.name) / "control-test")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src"), str(ROOT / "tests/control_test.cpp"), "-o", cls.binary], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_group(self, mode):
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            endpoint = f"127.0.0.1:{reserved.getsockname()[1]}"
        children = [subprocess.Popen([self.binary, str(rank), endpoint, mode, "unused"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE) for rank in (2, 1, 0)]
        try:
            for child in children:
                child.communicate(timeout=12)
            return [child.returncode for child in children]
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.communicate()

    def test_three_rank_collectives(self):
        self.assertEqual(self.run_group("pass"), [0, 0, 0])

    def test_sequence_mismatch_fails(self):
        self.assertTrue(all(code != 0 for code in self.run_group("mismatch")))

    def test_peer_exit_fails(self):
        self.assertTrue(all(code != 0 for code in self.run_group("exit")))

    def test_pattern_has_no_exact_window_alias(self):
        subprocess.run([self.binary, "pattern"], check=True)
