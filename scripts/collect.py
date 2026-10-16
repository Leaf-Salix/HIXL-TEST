#!/usr/bin/env python3
"""Collect every node/MPI rank's result; refuse partial or mixed runs."""
import argparse
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runner", ROOT / "scripts/run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def collect(directories):
    seen, session, world, source_hashes, mode, group_id = set(), None, None, None, None, None
    node_hosts = {}
    manifests = []
    for directory in directories:
        directory = Path(directory)
        manifest = json.loads((directory / "manifest.json").read_text())
        if manifest["status"] != "PASS" or manifest["returncodes"] != [0] * len(manifest["ranks"]):
            raise ValueError(f"failed or unfinished result: {directory}")
        current_hashes = manifest["build_info"]["source_sha256"]
        expected_session = runner.digest(manifest["config"], manifest["mode"], manifest["allocator"])
        if manifest["session"] != expected_session:
            raise ValueError("manifest config/session mismatch")
        planned = runner.validate(manifest["config"])
        if manifest["world"] != len(planned):
            raise ValueError("manifest world/config mismatch")
        if session is None:
            session, world, source_hashes, mode = expected_session, manifest["world"], current_hashes, manifest["mode"]
            group_id = manifest["run_id"]
        if (session, world, source_hashes, mode) != (expected_session, manifest["world"], current_hashes, manifest["mode"]):
            raise ValueError("cannot combine different configurations/modes/source builds")
        if group_id != manifest["run_id"]:
            raise ValueError("cannot combine different executions of the same configuration")
        for rank in manifest["ranks"]:
            number = rank["rank"]
            if number in seen or number not in range(world) or rank != planned[number]:
                raise ValueError("duplicate or invalid rank result")
            seen.add(number)
            text = (directory / f"rank-{number}.log").read_text()
            runner.check_log(text, number, world, mode)
            if runner.run_id(text) != group_id:
                raise ValueError("log/manifest run ID mismatch")
            node = rank["node"]
            host = manifest["hostname"]
            if node in node_hosts and node_hosts[node] != host:
                raise ValueError("one configured node executed on different hosts")
            node_hosts[node] = host
        manifests.append(manifest)
    if not manifests or seen != set(range(world)):
        raise ValueError("missing rank results; collect every node/MPI rank output directory")
    if len(set(node_hosts.values())) != len(node_hosts):
        raise ValueError("configured multi-node run has repeated hostnames; use distinct hostnames and real separate nodes")
    return dict(status="GLOBAL_PASS", mode=mode, world=world, observed_hosts=len(node_hosts),
                session=session, run_id=group_id, expected_global=runner.counts(world, mode),
                note="Explicit completion and byte checks passed; no cache coherence/atomics claim")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="汇总所有节点的结果目录；缺 rank 或混用不同测试会失败")
    parser.add_argument("directories", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(collect(args.directories), indent=2, ensure_ascii=False))
    except (ValueError, KeyError, OSError, TypeError) as error:
        parser.exit(1, f"ERROR: {error}\n")
