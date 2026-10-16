#!/usr/bin/env python3
"""Network-free syntax/format checks, used by pre-commit."""
import ast
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
for directory in ("scripts", "tests"):
    for path in sorted((root / directory).glob("*.py")):
        ast.parse(path.read_text(), filename=str(path))
for path in sorted((root / "scripts").glob("*.sh")):
    subprocess.run(["bash", "-n", str(path)], check=True)
for directory in ("src", "scripts", "tests", "docs", "configs"):
    for path in sorted((root / directory).rglob("*")):
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        content = path.read_text()
        if not content.endswith("\n"):
            raise SystemExit(f"missing final newline: {path.relative_to(root)}")
        for number, line in enumerate(content.splitlines(), 1):
            if line.rstrip() != line:
                raise SystemExit(f"trailing whitespace: {path.relative_to(root)}:{number}")
print("Offline syntax/format checks passed")
