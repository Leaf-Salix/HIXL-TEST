#!/usr/bin/env python3
"""独立 DRAM_A→DRAM_B 探针：启动、导出互连计数、汇总两端证据。"""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD_SOURCES = ("src/dram_route.cpp", "src/control.h", "src/pattern.h")
PHASES = ("idle", "n", "2n")


def hashes(paths):
    return {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in paths}


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def phase_records(log, rank, iterations, size):
    records = {}
    for line in log.splitlines():
        if not line.startswith("PHASE_PASS "):
            continue
        row = dict(item.split("=", 1) for item in line.split()[1:])
        phase = row.pop("phase")
        if phase not in PHASES or phase in records or row.pop("guards") != "PASS":
            raise ValueError("invalid/duplicate phase record")
        row = {key: int(value) for key, value in row.items()}
        count = {"idle": 0, "n": iterations, "2n": iterations * 2}[phase]
        times = [row[key] for key in ("sample_start_ns", "payload_start_ns", "payload_end_ns", "sample_stop_ns")]
        if row["rank"] != rank or row["copies"] != count or row["payload_bytes"] != count * size:
            raise ValueError("phase count/rank mismatch")
        if not all(a < b for a, b in zip(times, times[1:])):
            raise ValueError("invalid sampling window timestamps")
        row["sample_seconds"] = (times[-1] - times[0]) / 1e9
        row["payload_seconds"] = (times[2] - times[1]) / 1e9
        records[phase] = row
    if set(records) != set(PHASES) or log.count(f"ROUTE_RANK_PASS rank={rank} cleanup=PASS\n") != 1:
        raise ValueError("missing phases or successful cleanup")
    return records


def endpoint_records(log, rank, device, numa):
    devices = re.findall(r"^DEVICE rank=(\d+) logical=(\d+) physical=(\d+) numa=(\d+) dram_bytes=4194304 hbm_allocations=0$", log, re.M)
    if len(devices) != 1:
        raise ValueError("missing actual device record")
    actual_rank, logical, physical, actual_numa = map(int, devices[0])
    if (actual_rank, logical, actual_numa) != (rank, device, numa):
        raise ValueError("actual device/NUMA mismatch")
    queries = [line for line in log.splitlines() if line.startswith("DRIVER_QUERY ")]
    if len(queries) != (1 if rank == 0 else 0):
        raise ValueError("missing/unexpected driver query record")
    hosts = re.findall(r"^HOSTS same_host=([01])$", log, re.M)
    if len(hosts) != 1:
        raise ValueError("missing peer hostname record")
    return dict(physical_device=physical, driver_query=queries, same_host=hosts[0] == "1")


def read_metrics(directory, physical=None):
    """只识别 CANN 9.0 实际导出 CSV；不把未知 schema、缺文件或 NaN 当作零。"""
    metrics = {}
    errors = []
    files = []
    for link, average in (("hccs", "Average"), ("pcie", "Avg")):
        paths = sorted(directory.glob(f"PROF_*/mindstudio_profiler_output/{link}_*.csv"))
        if not paths:
            errors.append(f"{link}: missing CSV")
        for path in paths:
            files.append(str(path.relative_to(directory)))
            with path.open(newline="") as stream:
                reader = csv.DictReader(stream)
                required = {"Device_id", "Mode", "Min", "Max", average}
                if not required.issubset(reader.fieldnames or []):
                    errors.append(f"{link}: unknown CSV columns in {path.name}")
                    continue
                for row in reader:
                    # Latency is not bandwidth. Keep its raw CSV, but never integrate it as bytes.
                    if not row["Mode"].endswith("(MB/s)"):
                        continue
                    try:
                        values = [float(row[key]) for key in ("Min", "Max", average)]
                        if not all(math.isfinite(value) and value >= 0 for value in values):
                            raise ValueError("invalid bandwidth")
                        key = f"{link}/device-{int(row['Device_id'])}/{row['Mode']}"
                        if key in metrics:
                            raise ValueError("duplicate metric")
                        metrics[key] = dict(min=values[0], max=values[1], average=values[2], unit="MB/s")
                    except (ValueError, TypeError) as error:
                        errors.append(f"{link}: {error}")
        expected = ("Tx (MB/s)", "Rx (MB/s)") if link == "hccs" else (
            "Tx_p_avg(MB/s)", "Tx_np_avg(MB/s)", "Tx_cpl_avg(MB/s)",
            "Rx_p_avg(MB/s)", "Rx_np_avg(MB/s)", "Rx_cpl_avg(MB/s)")
        devices = {key.split("/", 2)[1] for key in metrics if key.startswith(link + "/")}
        if physical is not None and devices != {f"device-{physical}"}:
            errors.append(f"{link}: device does not match sampled physical device {physical}")
        if not devices:
            errors.append(f"{link}: missing bandwidth modes")
        for device in devices:
            observed = {key.split("/", 2)[2] for key in metrics if key.startswith(f"{link}/{device}/")}
            if not set(expected).issubset(observed):
                errors.append(f"{link}/{device}: missing bandwidth modes")
    return dict(metrics=metrics, errors=errors, csv_files=files,
                status="AVAILABLE" if not errors else "INCONCLUSIVE")


def run(args):
    if not re.fullmatch(r"[A-Za-z0-9_-]{8,80}", args.run_id):
        raise ValueError("run-id must be 8..80 alphanumeric/underscore/hyphen characters")
    if not (4096 <= args.bytes <= 1024 * 1024 and 1 <= args.iterations <= 1000000 and
            100 <= args.idle_ms <= 120000 and args.device >= 0 and args.numa >= 0 and args.timeout >= 30):
        raise ValueError("invalid size/iterations/idle/device/NUMA/timeout")
    build = json.loads((ROOT / "build/route-build.json").read_text())
    if build["source_sha256"] != hashes(BUILD_SOURCES) or build["binary_sha256"] != hashes(("build/dram_route",))["build/dram_route"]:
        raise ValueError("stale build: rerun bash scripts/build_route.sh")
    config = dict(run_id=args.run_id, coordinator=args.coordinator, bytes=args.bytes,
                  iterations=args.iterations, idle_ms=args.idle_ms,
                  source_sha256=hashes(BUILD_SOURCES + ("scripts/route.py",)))
    session = hashlib.sha256(json.dumps(config, sort_keys=True).encode()).hexdigest()
    directory = Path(args.output).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    manifest = dict(status="FAIL", rank=args.rank, hostname=socket.gethostname(), device=args.device,
                    numa=args.numa, config=config, session=session, build=build)
    save(directory / "route.json", manifest)
    command = [str(ROOT / "build/dram_route"), str(args.rank), str(args.device), str(args.numa),
               args.coordinator, str(directory), str(args.bytes), str(args.iterations), str(args.idle_ms), session]
    with (directory / "rank.log").open("w") as log:
        # CANN/driver versions and topology are diagnostics, outside every sampling window.
        try:
            info = subprocess.run(["npu-smi", "info"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, timeout=20)
            (directory / "npu-smi.txt").write_text(info.stdout)
        except (OSError, subprocess.TimeoutExpired) as error:
            (directory / "npu-smi.txt").write_text(str(error) + "\n")
        libraries = subprocess.run(["ldd", str(ROOT / "build/dram_route")], capture_output=True, text=True, check=True)
        (directory / "libraries.txt").write_text(libraries.stdout)
        try:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
            manifest["returncode"] = completed.returncode
        except subprocess.TimeoutExpired:
            manifest["returncode"] = -1
            manifest["error"] = "probe timeout"
    text = (directory / "rank.log").read_text()
    print(text, end="")
    try:
        if manifest["returncode"] != 0:
            raise ValueError("probe failed; see rank.log")
        manifest["phases"] = phase_records(text, args.rank, args.iterations, args.bytes)
        manifest.update(endpoint_records(text, args.rank, args.device, args.numa))
        manifest["status"] = "PASS"
    finally:
        save(directory / "route.json", manifest)
    print(f"TRANSFER_PASS rank={args.rank}; next: export {directory}")


def export(args):
    directory = Path(args.output).resolve()
    manifest = json.loads((directory / "route.json").read_text())
    if manifest["status"] != "PASS":
        raise ValueError("cannot export a failed/incomplete probe")
    profiles = {}
    for phase in PHASES:
        target = directory / phase
        # Export after all three sample windows, never during peer collection.
        with (directory / f"export-{phase}.log").open("w") as log:
            result = subprocess.run([args.msprof, "--export=on", f"--output={target}"],
                                    stdout=log, stderr=subprocess.STDOUT, timeout=300)
        profiles[phase] = read_metrics(target, manifest["physical_device"])
        profiles[phase]["export_returncode"] = result.returncode
        if result.returncode:
            profiles[phase]["errors"].append("msprof export failed; see export log")
            profiles[phase]["status"] = "INCONCLUSIVE"
    save(directory / "traffic.json", dict(session=manifest["session"], phases=profiles))
    print(json.dumps({phase: profiles[phase]["status"] for phase in PHASES}))


def collect(directories, allow_single_host=False):
    if len(directories) != 2:
        raise ValueError("exactly two rank directories required")
    records = [json.loads((path / "route.json").read_text()) for path in directories]
    if {record["rank"] for record in records} != {0, 1}:
        raise ValueError("missing/duplicate rank")
    if any(record["status"] != "PASS" or record["returncode"] != 0 for record in records):
        raise ValueError("failed rank")
    if records[0]["config"] != records[1]["config"] or records[0]["session"] != records[1]["session"]:
        raise ValueError("different source/configuration/run-ID")
    single_host = records[0]["hostname"] == records[1]["hostname"]
    if any(record["same_host"] != single_host for record in records):
        raise ValueError("hostname evidence mismatch")
    if single_host and not allow_single_host:
        raise ValueError("same-host smoke test is not a multi-node result")
    evidence = []
    complete = True
    for path, record in zip(directories, records):
        config = record["config"]
        log = (path / "rank.log").read_text()
        actual = endpoint_records(log, record["rank"], record["device"], record["numa"])
        if any(record[key] != value for key, value in actual.items()):
            raise ValueError("device/driver/hostname log evidence mismatch")
        phases = phase_records(log, record["rank"], config["iterations"], config["bytes"])
        if phases != record["phases"]:
            raise ValueError("phase evidence mismatch")
        traffic = json.loads((path / "traffic.json").read_text())
        if traffic["session"] != record["session"]:
            raise ValueError("traffic session mismatch")
        # Re-read CSVs: a stale summary cannot turn missing/changed raw evidence into success.
        samples = {phase: read_metrics(path / phase, record["physical_device"]) for phase in PHASES}
        for phase in PHASES:
            if samples[phase]["metrics"] != traffic["phases"][phase]["metrics"]:
                raise ValueError("traffic CSV/summary mismatch")
            if samples[phase]["status"] != "AVAILABLE" or traffic["phases"][phase]["status"] != "AVAILABLE":
                complete = False
            if set(samples[phase]["metrics"]) != set(samples["idle"]["metrics"]):
                complete = False
                samples[phase]["errors"].append("metric/device set differs from idle")
                samples[phase]["status"] = "INCONCLUSIVE"
        deltas = {}
        for phase in ("n", "2n"):
            for key, metric in samples[phase]["metrics"].items():
                idle = samples["idle"]["metrics"].get(key)
                if idle is not None:
                    deltas[f"{phase}/{key}"] = dict(
                        average_minus_idle_MBps=metric["average"] - idle["average"],
                        phase_average_MBps=metric["average"], idle_average_MBps=idle["average"])
        evidence.append(dict(rank=record["rank"], device=record["device"], physical_device=record["physical_device"], numa=record["numa"],
                             phases=phases, driver_query=record["driver_query"],
                             traffic=samples, differences=deltas))
    return dict(transfer_status="SINGLE_HOST_PASS" if single_host else "TWO_HOST_PASS",
                counter_status="AVAILABLE" if complete else "INCONCLUSIVE", run_id=records[0]["config"]["run_id"],
                route_conclusion="REQUIRES_ANALYSIS", ranks=evidence,
                note="端口聚合带宽不能单独证明完整物理路径；INNER 不等于 PCIe。保留原始 timeline/CSV。")


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
    launch.add_argument("--bytes", type=int, default=1048576)
    launch.add_argument("--iterations", type=int, default=32768)
    launch.add_argument("--idle-ms", type=int, default=3000)
    launch.add_argument("--timeout", type=int, default=900)
    launch.add_argument("--output", required=True)
    analyze = sub.add_parser("export")
    analyze.add_argument("--output", required=True)
    analyze.add_argument("--msprof", default="msprof")
    merge = sub.add_parser("collect")
    merge.add_argument("directories", nargs=2, type=Path)
    merge.add_argument("--allow-single-host", action="store_true")
    merge.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.command == "stamp":
        save(ROOT / "build/route-build.json", dict(source_sha256=hashes(BUILD_SOURCES),
             binary_sha256=hashes(("build/dram_route",))["build/dram_route"], cann=os.environ["ASCEND_HOME_PATH"]))
    elif args.command == "run":
        run(args)
    elif args.command == "export":
        export(args)
    else:
        result = collect(args.directories, args.allow_single_host)
        save(Path(args.output), result)
        print(result["transfer_status"], "counter_status=" + result["counter_status"], "route_conclusion=REQUIRES_ANALYSIS")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(f"ERROR: {error}")
