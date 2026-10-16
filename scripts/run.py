#!/usr/bin/env python3
"""Launch only local children; the TCP coordinator exchanges metadata, never payloads."""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MODES = ("baseline", "hixl", "kernel", "combined")


def numbers(value):
    try:
        result = [int(item) for item in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected comma-separated nonnegative integers") from error
    if not result or any(item < 0 for item in result):
        raise argparse.ArgumentTypeError("expected comma-separated nonnegative integers")
    return result


def endpoint(value):
    if not isinstance(value, str) or value.count(":") != 1:
        raise ValueError("endpoint must be IPv4:port")
    address, port = value.split(":")
    parsed = ipaddress.IPv4Address(address)
    if parsed.is_unspecified or parsed.is_multicast or not port.isdecimal() or not 1 <= int(port) <= 65535:
        raise ValueError("invalid endpoint address/port")
    return address, int(port)


def validate(config):
    if set(config) - {"coordinator", "nodes"}:
        raise ValueError("unknown configuration keys")
    coordinator_ip, coordinator_port = endpoint(config["coordinator"])
    nodes = config["nodes"]
    if not isinstance(nodes, list) or not nodes:
        raise ValueError("nodes must be a nonempty list")
    ranks, names, addresses, engines = [], set(), set(), set()
    for node in nodes:
        if set(node) - {"name", "ip", "devices", "numa_ids", "engine_port_base"}:
            raise ValueError("unknown node configuration keys")
        name, address, devices = node["name"], node["ip"], node["devices"]
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", name) or name in names:
            raise ValueError("node names must be unique simple identifiers")
        ip = ipaddress.IPv4Address(address)
        if ip.is_unspecified or ip.is_multicast or address in addresses:
            raise ValueError("nodes need distinct unicast IPs")
        names.add(name)
        addresses.add(address)
        if not isinstance(devices, list) or not devices or any(type(x) is not int or x < 0 for x in devices):
            raise ValueError("devices must be nonempty nonnegative integer lists")
        if len(devices) != len(set(devices)):
            raise ValueError("duplicate device on one node")
        numa = node.get("numa_ids", [-1] * len(devices))
        if not isinstance(numa, list) or len(numa) != len(devices) or any(type(x) is not int or x < -1 for x in numa):
            raise ValueError("numa_ids must match devices (-1 means A3 auto heuristic)")
        base = node.get("engine_port_base", 39100)
        if type(base) is not int:
            raise ValueError("engine_port_base must be an integer")
        for local, device in enumerate(devices):
            engine = f"{address}:{base + local}"
            endpoint(engine)
            if engine in engines or engine == config["coordinator"]:
                raise ValueError("control and HIXL ports must be distinct")
            engines.add(engine)
            ranks.append(dict(rank=len(ranks), node=name, device=device, numa=numa[local], engine=engine))
    if len(ranks) > 64:
        raise ValueError("at most 64 ranks (start small: exhaustive kernel matrix scales cubically)")
    if coordinator_ip != nodes[0]["ip"]:
        raise ValueError("coordinator must use the first node's IP (rank 0 hosts it)")
    if len(nodes) > 1 and any(ipaddress.IPv4Address(node["ip"]).is_loopback for node in nodes):
        raise ValueError("multi-node config cannot use loopback IPs")
    return ranks


def counts(world, mode):
    return dict(hixl_cases=32 * world * (world - 1) if mode in ("hixl", "combined") else 0,
                kernel_cases=2 * world * (2 * world) * (2 * world - 1) if mode in ("kernel", "combined") else 0,
                handoff_cases=4 * world * (world - 1) if mode == "combined" else 0)


def digest(config, mode, allocator):
    text = json.dumps(dict(config=config, mode=mode, allocator=allocator), sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode()).hexdigest()


def command(rank, config, world, args):
    return [str(ROOT / "build/matrix"), "--rank", str(rank["rank"]), "--world", str(world),
            "--device", str(rank["device"]), "--numa", str(rank["numa"]),
            "--engine", rank["engine"], "--coordinator", config["coordinator"],
            "--mode", args.mode, "--allocator", args.allocator,
            "--session", digest(config, args.mode, args.allocator),
            "--kernel-library", str(ROOT / "build/libfabric_copy.so"),
            "--control-timeout", str(args.control_timeout)]


def check_log(text, rank, world, mode):
    run_id(text)
    expected = counts(world, mode)
    line = (f"RANK_PASS rank={rank} hixl_cases={expected['hixl_cases']} "
            f"kernel_cases={expected['kernel_cases']} handoff_cases={expected['handoff_cases']} cleanup=PASS")
    if text.splitlines().count(line) != 1 or "_FAIL" in text:
        raise ValueError(f"rank {rank}: missing exact success record or failure in log")
    if len(re.findall(r"^BASELINE_PASS ", text, re.M)) != 2:
        raise ValueError(f"rank {rank}: baseline count mismatch")
    own = dict(CASE_PASS=32 * (world - 1) if expected["hixl_cases"] else 0,
               KERNEL_CASE_PASS=2 * (2 * world) * (2 * world - 1) if expected["kernel_cases"] else 0,
               HANDOFF_PASS=4 * (world - 1) if expected["handoff_cases"] else 0)
    if mode == "combined":
        own.update(LIFECYCLE_ACQUIRE_PASS=2, LIFECYCLE_BUSY_PASS=1, LIFECYCLE_RELEASE_PASS=2)
    for label, number in own.items():
        if len(re.findall(rf"^{label} ", text, re.M)) != number:
            raise ValueError(f"rank {rank}: {label} count mismatch")


def run_id(text):
    values = re.findall(r"^RUN_ID=([0-9a-f]{32})$", text, re.M)
    if len(values) != 1:
        raise ValueError("missing or repeated control-group run ID")
    return values[0]


def stop(children):
    # Only children created by this launcher; no scheduler/global process operations.
    for process in children:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + 3
    while any(process.poll() is None for process in children) and time.monotonic() < deadline:
        time.sleep(0.05)
    for process in children:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()


def launch(config, ranks, args):
    world = len(validate(config))
    if args.mode != "baseline" and world < 2:
        raise ValueError("hixl/kernel/combined require at least two ranks")
    if not ranks:
        raise ValueError("no ranks selected")
    if args.dry_run:
        for rank in ranks:
            print(json.dumps(dict(rank=rank, argv=command(rank, config, world, args)), ensure_ascii=False))
        print(json.dumps(dict(expected=counts(world, args.mode), world=world)))
        return 0
    binary = ROOT / "build/matrix"
    if not binary.is_file():
        raise ValueError("build/matrix missing; run bash scripts/build.sh first")
    if args.mode in ("kernel", "combined") and not (ROOT / "build/libfabric_copy.so").is_file():
        raise ValueError("kernel library missing; rebuild without --host-only")
    # Capture the selected libraries and fail before touching NPU if a dependency is absent.
    linked = subprocess.run(["ldd", str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout
    if "not found" in linked:
        raise ValueError("unresolved shared library:\n" + linked)
    output = Path(args.output or ROOT / "results" / (time.strftime("%Y%m%d-%H%M%S") + f"-{os.getpid()}"))
    output.mkdir(parents=True, exist_ok=False)
    expected = counts(world, args.mode)
    manifest = dict(status="RUNNING", mode=args.mode, allocator=args.allocator, world=world,
                    config=config, ranks=ranks, expected_global=expected,
                    binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    build_info=json.loads((ROOT / "build/build-info.json").read_text()),
                    selected_libraries=linked, session=digest(config, args.mode, args.allocator),
                    hostname=socket.gethostname(), result_scope="local ranks only; collect every node's result")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print(f"RESULT_DIR={output}", flush=True)
    children, streams = [], []
    try:
        for rank in ranks:
            stream = (output / f"rank-{rank['rank']}.log").open("w")
            streams.append(stream)
            children.append(subprocess.Popen(command(rank, config, world, args), stdout=stream,
                                             stderr=subprocess.STDOUT, start_new_session=True))
        deadline = time.monotonic() + args.timeout
        while True:
            returns = [child.poll() for child in children]
            if any(value is not None and value != 0 for value in returns):
                raise RuntimeError(f"rank process failed: {returns}")
            if all(value == 0 for value in returns):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("launcher overall timeout")
            time.sleep(0.1)
        for stream in streams:
            stream.close()
        group_id = None
        for rank in ranks:
            text = (output / f"rank-{rank['rank']}.log").read_text()
            check_log(text, rank["rank"], world, args.mode)
            if group_id is None:
                group_id = run_id(text)
            if group_id != run_id(text):
                raise ValueError("local ranks came from different control groups")
        manifest["run_id"] = group_id
        manifest["status"] = "PASS"
        print("LOCAL_RANKS_PASS " + json.dumps(dict(ranks=[rank["rank"] for rank in ranks], **expected)), flush=True)
        return 0
    except BaseException as error:
        manifest["status"] = "FAIL"
        manifest["error"] = str(error)
        raise
    finally:
        stop(children)
        for stream in streams:
            stream.close()
        manifest["returncodes"] = [child.returncode for child in children]
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")


def main(argv=None):
    def terminated(signum, frame):
        raise RuntimeError(f"launcher received signal {signum}")
    signal.signal(signal.SIGTERM, terminated)
    parser = argparse.ArgumentParser(description="HIXL FabricMem 全窗口测试（不需要公网；请在已预约 NPU 上执行）")
    sub = parser.add_subparsers(dest="launch", required=True)
    for name in ("single", "node", "mpi-rank"):
        item = sub.add_parser(name)
        if name == "single":
            devices = item.add_mutually_exclusive_group(required=True)
            devices.add_argument("--devices", type=numbers, help="当前 ACL 逻辑设备 ID，不能填未映射的物理卡号")
            devices.add_argument("--scheduled-devices", type=numbers, help="task-submit 的 {}：有可见性掩码时转为 0..N-1")
            item.add_argument("--numa-ids", type=numbers)
            item.add_argument("--port", type=int, default=0, help="控制端口，0 表示自动选空闲端口")
            item.add_argument("--engine-port-base", type=int, default=39100)
        else:
            item.add_argument("--config", type=Path, required=True)
            if name == "node":
                item.add_argument("--node", required=True)
        item.add_argument("--mode", choices=MODES, default="hixl")
        item.add_argument("--allocator", choices=("vmm", "adxl"), default="vmm")
        item.add_argument("--control-timeout", type=int, default=60)
        item.add_argument("--timeout", type=int, default=1800, help="本机子进程的总时限，秒")
        item.add_argument("--output", help="新建结果目录；MPI 每 rank 请使用不同目录")
        item.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not 1 <= args.control_timeout <= 3600 or args.timeout < 1:
        raise ValueError("invalid timeout")
    if args.launch == "single":
        devices = args.devices
        if devices is None:
            devices = args.scheduled_devices
            if os.environ.get("ASCEND_RT_VISIBLE_DEVICES"):
                devices = list(range(len(devices)))
        if args.port == 0:
            with socket.socket() as reserved:
                reserved.bind(("127.0.0.1", 0))
                args.port = reserved.getsockname()[1]
        node = dict(name="local", ip="127.0.0.1", devices=devices, engine_port_base=args.engine_port_base)
        if args.numa_ids is not None:
            node["numa_ids"] = args.numa_ids
        config = dict(coordinator=f"127.0.0.1:{args.port}", nodes=[node])
    else:
        config = json.loads(args.config.read_text())
    ranks = validate(config)
    if args.launch == "node":
        ranks = [rank for rank in ranks if rank["node"] == args.node]
    elif args.launch == "mpi-rank":
        if os.environ.get("OMPI_COMM_WORLD_RANK") is not None:
            rank_key, size_key = "OMPI_COMM_WORLD_RANK", "OMPI_COMM_WORLD_SIZE"
        elif os.environ.get("PMI_RANK") is not None:
            rank_key, size_key = "PMI_RANK", "PMI_SIZE"
        else:
            raise ValueError("mpi-rank needs Open MPI or MPICH rank/size environment")
        if int(os.environ[size_key]) != len(ranks):
            raise ValueError("MPI world size disagrees with config")
        rank = int(os.environ[rank_key])
        if not 0 <= rank < len(ranks):
            raise ValueError("MPI rank out of range")
        ranks = [ranks[rank]]
    return launch(config, ranks, args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, TypeError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
