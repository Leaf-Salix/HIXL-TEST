#!/usr/bin/env python3
"""CPU 远端 DRAM 映射能力探针；不支持时退出 3，绝不回退。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("src/cpu_direct.cpp", "src/control.h", "src/pattern.h")


def hashes(paths):
    return {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in paths}


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def evidence(log, rank, device, numa, returncode):
    devices = re.findall(r"^DEVICE rank=(\d+) logical=(\d+) physical=(\d+) numa=(\d+)$", log, re.M)
    hosts = re.findall(r"^HOSTS same_host=([01])$", log, re.M)
    imports = re.findall(r"^HOST_IMPORT rank=(\d+) target=HOST rc=(\d+) ready=([01])$", log, re.M)
    access = re.findall(r"^HOST_ACCESS rank=(\d+) set_rc=(-?\d+) get_rc=(-?\d+) flags=(\d+) ready=([01])$", log, re.M)
    results = re.findall(r"^CPU_DIRECT_RESULT rank=(\d+) status=(PASS|UNSUPPORTED) cleanup=PASS fallback=NONE$", log, re.M)
    if len(devices) != 1 or len(hosts) != 1 or len(imports) != 1 or len(access) != 1 or len(results) != 1:
        raise ValueError("missing/duplicate endpoint/access/cleanup evidence")
    actual_rank, logical, physical, actual_numa = map(int, devices[0])
    if (actual_rank, logical, actual_numa) != (rank, device, numa) or int(access[0][0]) != rank or int(results[0][0]) != rank:
        raise ValueError("rank/device/NUMA mismatch")
    status = results[0][1]
    import_rank, import_rc, import_ready = map(int, imports[0])
    if import_rank != rank or (import_rc, import_ready) not in ((0, 1), (207000, 0)):
        raise ValueError("invalid Host-target import evidence")
    set_rc, get_rc, flags, ready = map(int, access[0][1:])
    if not import_ready:
        if (set_rc, get_rc, flags, ready) != (-1, -1, 0, 0):
            raise ValueError("denied import must skip HOST access and CPU payload")
    elif ready:
        if (set_rc, get_rc, flags) != (0, 0, 3):
            raise ValueError("ready without HOST READWRITE capability")
    elif not ((set_rc == 207000 and get_rc == -1) or (set_rc == 0 and get_rc == 207000)):
        raise ValueError("denied capability must report FEATURE_NOT_SUPPORT, not a generic error")
    cases = re.findall(r"^CPU_CASE_PASS actor=([01]) operation=(READ|WRITE) bytes=65536 guards=PASS$", log, re.M)
    io_marker = "CPU_IO_BEGIN streams=0 memcpy_calls=0 kernel_launches=0 fallback=NONE\n"
    if status == "PASS":
        if returncode != 0 or not ready:
            raise ValueError("PASS requires confirmed HOST READWRITE access and zero returncode")
        if len(cases) != 4 or set(cases) != {(str(actor), operation) for actor in (0, 1) for operation in ("READ", "WRITE")}:
            raise ValueError("missing/duplicate CPU READ/WRITE cases")
        if log.count(io_marker) != 1:
            raise ValueError("missing CPU-only data-plane marker")
    else:
        if returncode != 3 or cases or io_marker in log:
            raise ValueError("UNSUPPORTED must have no CPU payload access and exit 3")
    if "CPU_DIRECT_FAIL" in log or "CPU_DIRECT_ACCESS_FAULT" in log:
        raise ValueError("runtime/CPU access failure")
    return dict(status=status, physical_device=physical, same_host=hosts[0] == "1",
                import_access=dict(rc=import_rc, ready=import_ready, target="HOST"),
                access=dict(zip(("set_rc", "get_rc", "flags", "ready"), map(int, access[0][1:]))), cases=cases)


def run(args):
    if not re.fullmatch(r"[A-Za-z0-9_-]{8,80}", args.run_id) or min(args.device, args.numa) < 0 or args.timeout < 30:
        raise ValueError("invalid run-ID/device/NUMA/timeout")
    build = json.loads((ROOT / "build/cpu-direct-build.json").read_text())
    if build["source_sha256"] != hashes(SOURCES) or build["binary_sha256"] != hashes(("build/cpu_direct",))["build/cpu_direct"]:
        raise ValueError("stale build; run bash scripts/build_cpu_direct.sh")
    config = dict(run_id=args.run_id, coordinator=args.coordinator,
                  source_sha256=hashes(SOURCES + ("scripts/cpu_direct.py",)))
    session = hashlib.sha256(json.dumps(config, sort_keys=True).encode()).hexdigest()
    directory = Path(args.output).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    result = dict(status="FAIL", rank=args.rank, hostname=socket.gethostname(), device=args.device,
                  numa=args.numa, config=config, session=session, build=build, returncode=-1)
    save(directory / "cpu-direct.json", result)
    command = [str(ROOT / "build/cpu_direct"), str(args.rank), str(args.device), str(args.numa), args.coordinator, session]
    try:
        with (directory / "rank.log").open("w") as log:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
        result["returncode"] = completed.returncode
        text = (directory / "rank.log").read_text()
        print(text, end="")
        parsed = evidence(text, args.rank, args.device, args.numa, completed.returncode)
        result.update(parsed)
        # Tuples become JSON lists; normalize so collect can compare exact records.
        result["cases"] = [list(case) for case in result["cases"]]
        return completed.returncode
    finally:
        save(directory / "cpu-direct.json", result)


def collect(directories, allow_single_host=False):
    records = [json.loads((path / "cpu-direct.json").read_text()) for path in directories]
    if len(records) != 2 or {record["rank"] for record in records} != {0, 1}:
        raise ValueError("exactly one directory per rank required")
    if any(record["status"] not in ("PASS", "UNSUPPORTED") for record in records):
        raise ValueError("failed/incomplete rank")
    if records[0]["session"] != records[1]["session"] or records[0]["config"] != records[1]["config"]:
        raise ValueError("source/config/run-ID mismatch")
    single_host = records[0]["hostname"] == records[1]["hostname"]
    if single_host and not allow_single_host:
        raise ValueError("same-host control is not a two-host result")
    for path, record in zip(directories, records):
        parsed = evidence((path / "rank.log").read_text(), record["rank"], record["device"], record["numa"], record["returncode"])
        parsed["cases"] = [list(case) for case in parsed["cases"]]
        if any(record[key] != value for key, value in parsed.items()) or record["same_host"] != single_host:
            raise ValueError("manifest/log evidence mismatch")
    statuses = {record["status"] for record in records}
    if len(statuses) != 1:
        raise ValueError("peers disagree about collective capability gate")
    unsupported = statuses == {"UNSUPPORTED"}
    if unsupported and not any(record["access"]["ready"] == 0 for record in records):
        raise ValueError("UNSUPPORTED without a denied capability")
    return dict(status="UNSUPPORTED" if unsupported else "SINGLE_HOST_CPU_PASS" if single_host else "TWO_HOST_CPU_DIRECT_PASS",
                observed_hosts=1 if single_host else 2, run_id=records[0]["config"]["run_id"],
                hccs_route="NOT_MEASURED", endpoints=records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("stamp")
    launch = sub.add_parser("run")
    launch.add_argument("--rank", type=int, choices=(0, 1), required=True)
    launch.add_argument("--device", type=int, required=True)
    launch.add_argument("--numa", type=int, required=True)
    launch.add_argument("--coordinator", required=True)
    launch.add_argument("--run-id", required=True)
    launch.add_argument("--output", required=True)
    launch.add_argument("--timeout", type=int, default=300)
    merge = sub.add_parser("collect")
    merge.add_argument("directories", nargs=2, type=Path)
    merge.add_argument("--allow-single-host", action="store_true")
    merge.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.command == "stamp":
        save(ROOT / "build/cpu-direct-build.json", dict(source_sha256=hashes(SOURCES),
             binary_sha256=hashes(("build/cpu_direct",))["build/cpu_direct"], cann=os.environ["ASCEND_HOME_PATH"]))
        return 0
    if args.command == "run":
        return run(args)
    result = collect(args.directories, args.allow_single_host)
    save(Path(args.output), result)
    print(result["status"], "hccs_route=NOT_MEASURED")
    return 3 if result["status"] == "UNSUPPORTED" else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(f"ERROR: {error}")
