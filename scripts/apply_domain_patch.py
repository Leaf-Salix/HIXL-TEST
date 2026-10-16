#!/usr/bin/env python3
"""Patch a user-provided isolated upstream source tree; never download or install."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description="只对匹配 beta2 的隔离源码应用实验补丁；不修改系统 HIXL")
parser.add_argument("source", type=Path)
parser.add_argument("--check", action="store_true", help="只检查，不修改源码")
args = parser.parse_args()
source = args.source.resolve()
expected = json.loads((root / "patches/base-sha256.json").read_text())
for name, digest in expected.items():
    path = source / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise SystemExit(f"源码不匹配或已修改：{name}；请使用原始 HIXL v9.1.0-beta.2 隔离副本")
if (source / "include/hixl/experimental_domain_window.h").exists():
    raise SystemExit("实验头文件已经存在，拒绝重复应用")
patch = root / "patches/hixl-v9.1.0-beta.2-domain.patch"
with patch.open("rb") as stream:
    subprocess.run(["patch", "--dry-run", "--batch", "-p1", "-d", str(source)], stdin=stream, check=True)
if not args.check:
    with patch.open("rb") as stream:
        subprocess.run(["patch", "--batch", "-p1", "-d", str(source)], stdin=stream, check=True)
print("PATCH_CHECK_PASS" if args.check else "PATCH_APPLIED")
