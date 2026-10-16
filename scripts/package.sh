#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/dist"
commit=$(git -C "$root" rev-parse --short HEAD)
# Only committed sources/docs, never generated results, environment files, or binaries.
git -C "$root" archive --format=tar.gz --prefix=HIXL-TEST/ \
  -o "$root/dist/HIXL-TEST-$commit.tar.gz" HEAD
if command -v sha256sum >/dev/null; then
  (cd "$root/dist" && sha256sum "HIXL-TEST-$commit.tar.gz" > "HIXL-TEST-$commit.tar.gz.sha256")
else
  (cd "$root/dist" && shasum -a 256 "HIXL-TEST-$commit.tar.gz" > "HIXL-TEST-$commit.tar.gz.sha256")
fi
printf '%s\n' "$root/dist/HIXL-TEST-$commit.tar.gz"
